#include "tau_config.h"
#include "storage.h"
#include <stdarg.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>

// ---- Job queue -------------------------------------------------------------
struct TauJob {
  uint8_t type;                       // 0 = command batch, 1 = config fetch
  char    ip[16];
  uint8_t ncmds;
  char    cmds[TAU_MAX_CMDS][TAU_CMD_LEN];
};

static QueueHandle_t     s_queue = NULL;
static SemaphoreHandle_t s_mtx = NULL;

static volatile bool     s_busy = false;
static char              s_status[96] = "idle";
static char              s_log[TAU_LOG_LINES][TAU_LOG_LEN];
static uint8_t           s_log_head = 0;    // next write slot (ring)
static uint8_t           s_log_count = 0;
static uint32_t          s_log_gen = 0;
static char              s_rows[TAU_ROWS_MAX][TAU_ROW_LEN];
static uint8_t           s_row_count = 0;
static uint32_t          s_rows_gen = 0;

static const uint32_t IO_TIMEOUT_MS  = 5000;
static const uint32_t CONN_TIMEOUT_MS = 3000;
static const uint32_t COMMAND_GAP_MS  = 400;

// ---- Result log -------------------------------------------------------------
static void log_line(const char* fmt, ...) {
  char tmp[TAU_LOG_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  snprintf(s_log[s_log_head], TAU_LOG_LEN, "%s", tmp);
  s_log_head = (s_log_head + 1) % TAU_LOG_LINES;
  if (s_log_count < TAU_LOG_LINES) s_log_count++;
  s_log_gen++;
  xSemaphoreGive(s_mtx);
}

static void set_status(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_status, sizeof(s_status), fmt, ap);
  va_end(ap);
}

// ---- Raw HTTP/1.1 client (title-case headers — firmware is case-sensitive) --
struct HttpResult {
  bool     ok = false;
  uint16_t status = 0;
  String   body;
  String   err;
};

// Read until buf ends with \r\n\r\n or deadline; returns head length incl. CRLFCRLF.
static int read_head(WiFiClient& c, String& buf, uint32_t deadline) {
  for (;;) {
    int p = buf.indexOf("\r\n\r\n");
    if (p >= 0) return p + 4;
    if ((int32_t)(millis() - deadline) >= 0) return -1;
    if (c.available()) {
      buf += (char)c.read();
      if (buf.length() > 8192) return -1;
    } else if (!c.connected()) {
      return -1;
    } else {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
}

// Read exactly n more bytes (or as many as arrive before deadline) into body.
static void read_n(WiFiClient& c, String& body, size_t n, uint32_t deadline) {
  while (body.length() < n && (int32_t)(millis() - deadline) < 0) {
    if (c.available()) {
      body += (char)c.read();
    } else if (!c.connected()) {
      break;
    } else {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
}

static HttpResult raw_http(const char* ip, uint16_t port, const char* method,
                           const char* path, const String* body) {
  HttpResult res;
  WiFiClient c;
  c.setTimeout(IO_TIMEOUT_MS / 1000);
  if (!c.connect(ip, port, CONN_TIMEOUT_MS)) {
    res.err = String("connect to ") + ip + " failed";
    return res;
  }
  // Segment separation before the first data segment (see slink.cpp note).
  vTaskDelay(pdMS_TO_TICKS(50));

  String req = String(method) + " " + path + " HTTP/1.1\r\n"
             + "Host: " + ip + "\r\n"
             + "User-Agent: GaiaCode-CYD\r\n"
             + "Accept: */*\r\n"
             + "Connection: close\r\n";
  if (body) {
    req += "Content-Type: application/x-www-form-urlencoded\r\n";
    req += "Content-Length: " + String(body->length()) + "\r\n";
  }
  req += "\r\n";
  if (body) req += *body;
  if (c.write((const uint8_t*)req.c_str(), req.length()) != req.length()) {
    res.err = "write failed";
    c.stop();
    return res;
  }

  uint32_t deadline = millis() + IO_TIMEOUT_MS;
  String head;
  int head_len = read_head(c, head, deadline);
  if (head_len < 0) {
    res.err = "connection closed before headers";
    c.stop();
    return res;
  }

  // Status line
  int sp1 = head.indexOf(' ');
  res.status = (sp1 > 0) ? (uint16_t)head.substring(sp1 + 1).toInt() : 0;

  // Headers (case-insensitive on the RESPONSE — only request names must match)
  bool chunked = false;
  long content_length = -1;
  int pos = head.indexOf("\r\n") + 2;
  while (pos > 1 && pos < head_len - 4) {
    int eol = head.indexOf("\r\n", pos);
    if (eol < 0) break;
    String line = head.substring(pos, eol);
    int colon = line.indexOf(':');
    if (colon > 0) {
      String name = line.substring(0, colon);
      String val  = line.substring(colon + 1);
      name.toLowerCase();
      val.trim();
      if (name == "transfer-encoding" && val.equalsIgnoreCase("chunked")) chunked = true;
      if (name == "content-length") content_length = val.toInt();
    }
    pos = eol + 2;
  }

  // Body: some bytes may already sit in `head` past head_len
  String bodybuf = head.substring(head_len);
  if (chunked) {
    // Chunked decoding: <hex>\r\n<data>\r\n ... 0\r\n\r\n
    for (;;) {
      // need a size line
      int eol;
      while ((eol = bodybuf.indexOf("\r\n")) < 0) {
        if ((int32_t)(millis() - deadline) >= 0) break;
        if (c.available()) bodybuf += (char)c.read();
        else if (!c.connected()) break;
        else vTaskDelay(pdMS_TO_TICKS(2));
      }
      if (eol < 0) break;
      long size = strtol(bodybuf.substring(0, eol).c_str(), NULL, 16);
      while ((long)bodybuf.length() < eol + 2 + size + 2 &&
             (int32_t)(millis() - deadline) < 0 && c.connected()) {
        if (c.available()) bodybuf += (char)c.read();
        else vTaskDelay(pdMS_TO_TICKS(2));
      }
      if (size == 0) break;
      if ((long)bodybuf.length() < eol + 2 + size) break;
      res.body += bodybuf.substring(eol + 2, eol + 2 + size);
      bodybuf.remove(0, eol + 2 + size + 2);
      if (res.body.length() > 256 * 1024) break;
    }
  } else if (content_length >= 0) {
    read_n(c, bodybuf, (size_t)content_length, deadline);
    res.body = bodybuf.substring(0, (size_t)content_length);
  } else {
    // No length: read until close or deadline
    while ((int32_t)(millis() - deadline) < 0 && c.connected()) {
      if (c.available()) bodybuf += (char)c.read();
      else vTaskDelay(pdMS_TO_TICKS(2));
    }
    res.body = bodybuf;
  }
  c.stop();
  res.ok = true;
  return res;
}

// ---- RESP extraction (both firmware dialects; port of extract_resp) ---------
static String strip_tags(const String& frag) {
  String out;
  bool in_tag = false;
  for (size_t i = 0; i < frag.length(); i++) {
    char ch = frag[i];
    if (ch == '<') in_tag = true;
    else if (ch == '>') in_tag = false;
    else if (!in_tag) out += ch;
  }
  out.trim();
  return out;
}

static String extract_resp(const String& html) {
  String lower = html;
  lower.toLowerCase();
  int p = lower.indexOf("id=\"resp-row\"");
  if (p >= 0) {
    int gt = lower.indexOf('>', p);
    if (gt < 0) return "";
    int end = lower.indexOf("</td>", gt + 1);
    if (end < 0) return "";
    return strip_tags(html.substring(gt + 1, end));
  }
  p = lower.indexOf("resp:");
  if (p >= 0) {
    int td = lower.indexOf("<td>", p);
    if (td < 0) return "";
    int end = lower.indexOf("</td>", td + 4);
    if (end < 0) return "";
    return strip_tags(html.substring(td + 4, end));
  }
  return "";
}

// ---- Command dispatch (port of dispatch_command; /filters.htm only) ---------
static String dispatch_command(const char* ip, uint16_t port, const char* cmd,
                               bool* ok) {
  String form = String("CMD=") + cmd;
  HttpResult post = raw_http(ip, port, "POST", "/filters.htm", &form);
  if (!post.ok) { *ok = false; return post.err; }

  String html;
  if (post.status == 302) {
    HttpResult get = raw_http(ip, port, "GET", "/filters.htm", NULL);
    if (!get.ok) { *ok = false; return get.err; }
    if (get.status != 200) {
      *ok = false;
      return String("GET after redirect: HTTP ") + get.status;
    }
    html = get.body;
  } else if (post.status == 200) {
    html = post.body;
  } else {
    *ok = false;
    return String("POST rejected: HTTP ") + post.status;
  }
  String resp = extract_resp(html);
  *ok = true;
  return resp.length() ? resp : String("(no RESP cell found)");
}

// ---- PGA token sanitizer (port of quote_unquoted_pga_values) ----------------
// Quotes bare tokens like 217c / -3c at JSON value positions so the payload
// parses as strict JSON. Firmware quirk; harmless when absent.
static String sanitize_pga(const String& text) {
  String out;
  out.reserve(text.length() + 16);
  bool in_string = false;
  size_t n = text.length();
  for (size_t i = 0; i < n;) {
    char c = text[i];
    if (in_string) {
      out += c;
      if (c == '\\' && i + 1 < n) { out += text[i + 1]; i += 2; continue; }
      if (c == '"') in_string = false;
      i++;
      continue;
    }
    if (c == '"') { in_string = true; out += c; i++; continue; }
    if (c == '-' || isdigit((unsigned char)c)) {
      size_t start = i, j = i;
      if (text[j] == '-') j++;
      size_t digits_start = j;
      while (j < n && isdigit((unsigned char)text[j])) j++;
      if (j > digits_start && j < n && text[j] == 'c') {
        size_t k = start;
        while (k > 0 && isspace((unsigned char)text[k - 1])) k--;
        bool prev_ok = k > 0 && (text[k - 1] == ':' || text[k - 1] == '[' || text[k - 1] == ',');
        size_t m = j + 1;
        while (m < n && isspace((unsigned char)text[m])) m++;
        bool next_ok = m < n && (text[m] == ']' || text[m] == ',' || text[m] == '}');
        if (prev_ok && next_ok) {
          out += '"';
          out += text.substring(start, j + 1);
          out += '"';
          i = j + 1;
          continue;
        }
      }
    }
    out += c;
    i++;
  }
  return out;
}

// ---- Config fetch + flatten ---------------------------------------------------
static void row_push(const char* fmt, ...) {
  char tmp[TAU_ROW_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  if (s_row_count < TAU_ROWS_MAX) {
    snprintf(s_rows[s_row_count++], TAU_ROW_LEN, "%s", tmp);
  }
  xSemaphoreGive(s_mtx);
}

static bool get_json(const char* ip, uint16_t port, const char* path,
                     JsonDocument& doc, String* err) {
  HttpResult r = raw_http(ip, port, "GET", path, NULL);
  if (!r.ok) { *err = r.err; return false; }
  if (r.status != 200) { *err = String("HTTP ") + r.status; return false; }
  String sane = sanitize_pga(r.body);
  DeserializationError de = deserializeJson(doc, sane);
  if (de) { *err = String("JSON: ") + de.c_str(); return false; }
  return true;
}

static void flatten_kv(JsonObject obj, const char* hide1, const char* hide2) {
  for (JsonPair kv : obj) {
    const char* k = kv.key().c_str();
    if ((hide1 && strcmp(k, hide1) == 0) || (hide2 && strcmp(k, hide2) == 0))
      continue;
    JsonVariant v = kv.value();
    if (v.isNull() || (v.is<const char*>() && strlen(v.as<const char*>()) == 0)) {
      row_push("%s = -", k);
    } else if (v.is<JsonArray>()) {
      String joined;
      for (JsonVariant e : v.as<JsonArray>()) {
        if (joined.length()) joined += ", ";
        if (e.is<const char*>()) joined += e.as<const char*>();
        else joined += e.as<String>();
      }
      row_push("%s = %s", k, joined.c_str());
    } else if (v.is<JsonObject>()) {
      String s;
      serializeJson(v, s);
      row_push("%s = %s", k, s.c_str());
    } else {
      String s = v.as<String>();
      s.trim();
      row_push("%s = %s", k, s.c_str());
    }
  }
}

static void fetch_config(const char* ip) {
  const uint16_t port = 80;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  s_row_count = 0;
  xSemaphoreGive(s_mtx);

  String err;

  // Identity (general.json) — best-effort on old firmware
  {
    DynamicJsonDocument doc(4096);
    if (get_json(ip, port, "/general.json", doc, &err)) {
      row_push("## Identity (general.json)");
      if (doc.is<JsonObject>()) flatten_kv(doc.as<JsonObject>(), NULL, NULL);
    }
  }
  // SEED IDs (proto.json -> Seed)
  {
    DynamicJsonDocument doc(6144);
    if (get_json(ip, port, "/proto.json", doc, &err)) {
      row_push("## SEED IDs (proto.json)");
      JsonObject seed = doc["Seed"].as<JsonObject>();
      if (!seed.isNull()) flatten_kv(seed, NULL, NULL);
      else if (doc.is<JsonObject>()) flatten_kv(doc.as<JsonObject>(), NULL, NULL);
    } else {
      row_push("## proto.json error: %s", err.c_str());
    }
  }
  // Channels (filters.json -> channels[])
  {
    DynamicJsonDocument doc(8192);
    if (get_json(ip, port, "/filters.json", doc, &err)) {
      row_push("## Channels (filters.json)");
      JsonArray chs = doc["channels"].as<JsonArray>();
      uint8_t i = 1;
      for (JsonObject ch : chs) {
        const char* chn = ch["chn"] | "-";
        String vpc;
        if (!ch["vpc"].isNull()) vpc = ch["vpc"].as<String>();
        else if (!ch["Uvpc"].isNull()) vpc = ch["Uvpc"].as<String>();
        else vpc = "-";
        String sps = "-";
        JsonArray fir = ch["FIR"].as<JsonArray>();
        if (!fir.isNull() && fir.size() > 0) sps = fir[0].as<String>();
        row_push("Ch %u: %s  %s uV/cnt  %s sps", i, chn, vpc.c_str(), sps.c_str());
        i++;
      }
    } else {
      row_push("## filters.json error: %s", err.c_str());
    }
  }
  // NVM (nvm1.json -> NVM, hiding FTP credentials) — best-effort
  {
    DynamicJsonDocument doc(8192);
    if (get_json(ip, port, "/nvm1.json", doc, &err)) {
      row_push("## NVM Configuration (nvm1.json)");
      JsonObject nvm = doc["NVM"].as<JsonObject>();
      if (!nvm.isNull()) flatten_kv(nvm, "FTP user", "FTP PW");
    }
  }
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  s_rows_gen++;
  xSemaphoreGive(s_mtx);
}

// ---- Worker task ---------------------------------------------------------------
static void tau_task(void* arg) {
  (void)arg;
  TauJob job;
  for (;;) {
    if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) continue;
    s_busy = true;

    if (job.type == 1) {
      set_status("reading config from %s…", job.ip);
      fetch_config(job.ip);
      set_status("config read complete");
      log_line("READ %s: config rows updated", job.ip);
      storage_log("TAU", "config read");
    } else {
      bool all_ok = true;
      for (uint8_t i = 0; i < job.ncmds; i++) {
        set_status("TX %u/%u: %s", i + 1, job.ncmds, job.cmds[i]);
        bool ok = false;
        String resp = dispatch_command(job.ip, 80, job.cmds[i], &ok);
        if (!ok) all_ok = false;
        log_line("%s CMD=%s -> %s", ok ? "OK " : "ERR", job.cmds[i], resp.c_str());
        char smsg[128];
        snprintf(smsg, sizeof(smsg), "%s CMD=%s %s", job.ip, job.cmds[i],
                 ok ? "ok" : "FAIL");
        storage_log("TAU", smsg);
        if (i + 1 < job.ncmds) vTaskDelay(pdMS_TO_TICKS(COMMAND_GAP_MS));
      }
      set_status(all_ok ? "batch complete" : "batch complete with errors");
    }
    s_busy = false;
  }
}

// ---- Public API ------------------------------------------------------------------
void tau_config_init() {
  s_mtx = xSemaphoreCreateMutex();
  s_queue = xQueueCreate(2, sizeof(TauJob));
  xTaskCreatePinnedToCore(tau_task, "tau_cfg", 10240, NULL, 1, NULL, 0);
}

bool tau_send_batch(const char* ip, const char cmds[][TAU_CMD_LEN], uint8_t n) {
  if (!s_queue || !ip || n == 0 || n > TAU_MAX_CMDS) return false;
  TauJob job;
  memset(&job, 0, sizeof(job));
  job.type = 0;
  snprintf(job.ip, sizeof(job.ip), "%s", ip);
  job.ncmds = n;
  for (uint8_t i = 0; i < n; i++) {
    if (strlen(cmds[i]) >= TAU_CMD_LEN) return false;
    snprintf(job.cmds[i], TAU_CMD_LEN, "%s", cmds[i]);
  }
  return xQueueSend(s_queue, &job, 0) == pdTRUE;
}

bool tau_send_one(const char* ip, const char* cmd) {
  char cmds[1][TAU_CMD_LEN];
  snprintf(cmds[0], TAU_CMD_LEN, "%s", cmd);
  return tau_send_batch(ip, cmds, 1);
}

bool tau_fetch(const char* ip) {
  if (!s_queue || !ip) return false;
  TauJob job;
  memset(&job, 0, sizeof(job));
  job.type = 1;
  snprintf(job.ip, sizeof(job.ip), "%s", ip);
  return xQueueSend(s_queue, &job, 0) == pdTRUE;
}

bool tau_busy() { return s_busy; }
const char* tau_status() { return s_status; }
uint32_t tau_log_generation() { return s_log_gen; }
uint32_t tau_rows_generation() { return s_rows_gen; }

uint8_t tau_log_snapshot(char out[][TAU_LOG_LEN], uint8_t max) {
  if (!s_mtx) return 0;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  uint8_t n = s_log_count < max ? s_log_count : max;
  for (uint8_t i = 0; i < n; i++) {
    uint8_t idx = (s_log_head + TAU_LOG_LINES - s_log_count + i) % TAU_LOG_LINES;
    snprintf(out[i], TAU_LOG_LEN, "%s", s_log[idx]);
  }
  xSemaphoreGive(s_mtx);
  return n;
}

uint8_t tau_rows_snapshot(char out[][TAU_ROW_LEN], uint8_t max) {
  if (!s_mtx) return 0;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  uint8_t n = s_row_count < max ? s_row_count : max;
  memcpy(out, s_rows, n * TAU_ROW_LEN);
  xSemaphoreGive(s_mtx);
  return n;
}
