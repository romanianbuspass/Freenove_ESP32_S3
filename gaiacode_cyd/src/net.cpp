#include "net.h"
#include "storage.h"
#include "audio.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>

// ---- Configuration ----------------------------------------------------------
// Credentials come from /wifi.cfg on the SD card (template auto-written on
// first boot). Discovery protocol per GaiaSignal_Rust tau_discovery.rs.
struct NetConfig {
  const char* hostname;
  uint16_t    heartbeat_port;   // passive: Tau application heartbeats
  uint16_t    an1388_port;      // active:  bootloader query
  uint32_t    sweep_ms;         // on-demand scan listen window
};
static NetConfig CFG = { "gaiacode-cyd", 5555, 6234, 3000 };

static char s_ssid[33];
static char s_pass[65];
static volatile bool s_creds = false;

static const uint8_t MAX_DEVS = 16;

static LanDevice s_devs[MAX_DEVS];
static uint8_t   s_count = 0;
static uint32_t  s_generation = 0;
static bool      s_connected = false;
static SemaphoreHandle_t s_mtx = NULL;

static TaskHandle_t    s_net_task = NULL;
static volatile bool   s_scanning = false;

// ---- Device table (call with s_mtx held) -------------------------------------
static void table_add(const char* ip, const char* mac, const char* host,
                      uint16_t sl_port, const char* state) {
  for (uint8_t i = 0; i < s_count; i++) {
    if (strncmp(s_devs[i].ip, ip, sizeof(s_devs[i].ip)) == 0) {
      // Application-mode info overrides a bare bootloader entry.
      if (strcmp(state, "app") == 0 && strcmp(s_devs[i].state, "boot") == 0) {
        LanDevice* d = &s_devs[i];
        snprintf(d->mac,  sizeof(d->mac),  "%s", mac);
        snprintf(d->host, sizeof(d->host), "%s", host);
        d->sl_port = sl_port;
        snprintf(d->state, sizeof(d->state), "%s", state);
        s_generation++;
      }
      return;
    }
  }
  if (s_count >= MAX_DEVS) return;
  LanDevice* d = &s_devs[s_count++];
  snprintf(d->ip,   sizeof(d->ip),   "%s", ip);
  snprintf(d->mac,  sizeof(d->mac),  "%s", mac);
  snprintf(d->host, sizeof(d->host), "%s", host);
  d->sl_port = sl_port;
  snprintf(d->state, sizeof(d->state), "%s", state);
  s_generation++;
}

static void add_self() {
  table_add(WiFi.localIP().toString().c_str(),
            WiFi.macAddress().c_str(),
            CFG.hostname, 0, "app");
}

// ---- Heartbeat parsing (port of parse_heartbeat_meta) -------------------------
static bool parse_heartbeat(const char* payload, uint16_t* port,
                            char* host, size_t host_len,
                            char* mac, size_t mac_len) {
  while (*payload == ' ' || *payload == '\t' || *payload == '!') payload++;

  // Format B: GAIACODE:<DeviceID>:<WebPort>:<SeedLinkPort>
  const char* gb = strstr(payload, "GAIACODE:");
  if (gb) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%s", gb);
    char* parts[4] = {0};
    uint8_t n = 0;
    for (char* t = strtok(buf, ":"); t && n < 4; t = strtok(NULL, ":"))
      parts[n++] = t;
    if (n >= 4) {
      long p = atol(parts[3]);
      if (p > 0 && p <= 65535) {
        *port = (uint16_t)p;
        if (parts[1][0]) snprintf(host, host_len, "%s", parts[1]);
        return true;
      }
    }
  }

  // Format A: GIACODE<Port>:<hex>  (also tolerate GAIACODE without colon)
  const char* prefix = NULL;
  if (strncmp(payload, "GIACODE", 7) == 0)       prefix = payload + 7;
  else if (strncmp(payload, "GAIACODE", 8) == 0) prefix = payload + 8;
  else return false;

  long p = atol(prefix);
  if (p <= 0 || p > 65535) return false;
  *port = (uint16_t)p;

  const char* colon = strchr(prefix, ':');
  if (colon && colon[1]) {
    const char* id = colon + 1;
    size_t n = strlen(id);
    while (n > 0 && (id[n-1] == '\r' || id[n-1] == '\n' || id[n-1] == ' ')) n--;
    if (n == 12) {   // 12 hex chars -> format as MAC
      snprintf(mac, mac_len, "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c",
               id[0], id[1], id[2], id[3], id[4], id[5],
               id[6], id[7], id[8], id[9], id[10], id[11]);
    }
  }
  return true;
}

// ---- AN1388 bootloader query ---------------------------------------------------
static void crc16_ibm(const uint8_t* data, size_t len, uint8_t* lo, uint8_t* hi) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++) {
      bool lsb = crc & 1;
      crc >>= 1;
      if (lsb) crc ^= 0xA001;
    }
  }
  *lo = crc & 0xFF;
  *hi = (crc >> 8) & 0xFF;
}

static void build_an1388_query(uint8_t out[5]) {
  uint8_t lo, hi;
  uint8_t hdr[2] = {0x01, 0x01};
  crc16_ibm(hdr, 2, &lo, &hi);
  out[0] = 0x01; out[1] = 0x01; out[2] = lo; out[3] = hi; out[4] = 0x04;
}

// ---- Discovery ------------------------------------------------------------------
static void log_discovery(const char* host, const char* ip, const char* state) {
  Serial.printf("LAN: %s at %s (%s)\n", host, ip, state);
  char msg[80];
  snprintf(msg, sizeof(msg), "%s %s %s", state, host, ip);
  storage_log("LAN", msg);
}

static void service_heartbeats(WiFiUDP& udp) {
  int n;
  while ((n = udp.parsePacket()) > 0) {
    char buf[256];
    int r = udp.read(buf, sizeof(buf) - 1);
    if (r <= 0) continue;
    buf[r] = '\0';
    uint16_t port = 0;
    char host[32] = "Tau";
    char mac[18]  = "--";
    if (parse_heartbeat(buf, &port, host, sizeof(host), mac, sizeof(mac))) {
      String ip = udp.remoteIP().toString();
      xSemaphoreTake(s_mtx, portMAX_DELAY);
      uint32_t before = s_generation;
      table_add(ip.c_str(), mac, host, port, "app");
      bool is_new = s_generation != before;
      xSemaphoreGive(s_mtx);
      if (is_new) log_discovery(host, ip.c_str(), "app");
    }
  }
}

static void service_an1388_replies(WiFiUDP& udp_bl) {
  int n;
  while ((n = udp_bl.parsePacket()) > 0) {
    uint8_t buf[64];
    udp_bl.read(buf, sizeof(buf));
    String ip = udp_bl.remoteIP().toString();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    uint32_t before = s_generation;
    table_add(ip.c_str(), "--", "Tau (bootloader)", CFG.an1388_port, "boot");
    bool is_new = s_generation != before;
    xSemaphoreGive(s_mtx);
    if (is_new) log_discovery("Tau (bootloader)", ip.c_str(), "boot");
  }
}

// One explicit sweep: clear the table, fire a single AN1388 query, then listen
// passively for heartbeats and bootloader replies for CFG.sweep_ms.
static void sweep(WiFiUDP& udp_hb, WiFiUDP& udp_bl) {
  s_scanning = true;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  s_count = 0;
  s_generation++;
  add_self();
  xSemaphoreGive(s_mtx);

  uint8_t q[5];
  build_an1388_query(q);
  udp_bl.beginPacket(IPAddress(255, 255, 255, 255), CFG.an1388_port);
  udp_bl.write(q, sizeof(q));
  udp_bl.endPacket();
  storage_log("LAN", "scan started");

  uint32_t deadline = millis() + CFG.sweep_ms;
  while ((int32_t)(millis() - deadline) < 0) {
    service_heartbeats(udp_hb);
    service_an1388_replies(udp_bl);
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  s_scanning = false;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  uint8_t found = s_count > 0 ? s_count - 1 : 0;   // exclude self
  xSemaphoreGive(s_mtx);

  char msg[48];
  snprintf(msg, sizeof(msg), "scan complete, %u Tau(s)", found);
  Serial.printf("LAN: %s\n", msg);
  storage_log("LAN", msg);
  if (found > 0) audio_play(SOUND_ACTION_KICK);   // contact confirmed
}

// ---- Network task: blocked on task-notify until a scan is requested ------------
// Wi-Fi housekeeping runs on a 1 s notify timeout; zero network traffic and
// near-zero CPU between explicit "Scan for Taus" triggers.
static void net_task(void* arg) {
  (void)arg;
  WiFiUDP udp_hb, udp_bl;
  bool announced = false;
  uint32_t last_cred_check = 0;

  for (;;) {
    if (!s_creds) {
      if (millis() - last_cred_check > 10000) {
        last_cred_check = millis();
        if (storage_wifi_creds(s_ssid, sizeof(s_ssid), s_pass, sizeof(s_pass))) {
          s_creds = true;
          Serial.printf("LAN: credentials loaded for \"%s\"\n", s_ssid);
          storage_log("LAN", "credentials loaded");
          WiFi.begin(s_ssid, s_pass);
        }
      }
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
      continue;
    }

    if (WiFi.status() != WL_CONNECTED) {
      if (s_connected) {
        Serial.println("LAN: Wi-Fi lost — clearing table");
        storage_log("LAN", "Wi-Fi lost");
        s_connected = false;
        udp_hb.stop();
        udp_bl.stop();
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_count = 0;
        s_generation++;
        xSemaphoreGive(s_mtx);
        WiFi.reconnect();
      }
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
      continue;
    }

    if (!s_connected) {
      s_connected = true;
      Serial.printf("LAN: Wi-Fi up, IP %s  RSSI %d dBm\n",
                    WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
      char msg[64];
      snprintf(msg, sizeof(msg), "Wi-Fi up %s (%d dBm)",
               WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
      storage_log("LAN", msg);
      udp_hb.begin(CFG.heartbeat_port);
      udp_bl.begin(0);
      xSemaphoreTake(s_mtx, portMAX_DELAY);
      add_self();
      xSemaphoreGive(s_mtx);
      if (!announced) {
        announced = MDNS.begin(CFG.hostname);
        Serial.printf("LAN: mDNS announce %s (%s)\n",
                      CFG.hostname, announced ? "ok" : "failed");
      }
      // One automatic sweep on first connect so the tab is not empty at boot.
      sweep(udp_hb, udp_bl);
    }

    // Connected and idle: block until the UI requests a scan (1 s tick keeps
    // the Wi-Fi status checks above alive).
    uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    if (notified && s_connected) sweep(udp_hb, udp_bl);
  }
}

void net_init() {
  s_mtx = xSemaphoreCreateMutex();
  WiFi.mode(WIFI_STA);
  // Modem sleep makes this STA miss buffered unicast (ARP replies, TCP data)
  // on busy enterprise APs — broadcast RX/TX still works, which masks the
  // failure as "discovery fine, TCP dead". Disable power save outright.
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(CFG.hostname);
  s_creds = storage_wifi_creds(s_ssid, sizeof(s_ssid), s_pass, sizeof(s_pass));
  if (s_creds) {
    Serial.printf("LAN: STA connecting to \"%s\"...\n", s_ssid);
    WiFi.begin(s_ssid, s_pass);
  } else {
    Serial.println("LAN: no usable /wifi.cfg — discovery idle");
  }
  xTaskCreatePinnedToCore(net_task, "net", 4096, NULL, 1, &s_net_task, 0);
}

void net_scan() {
  if (s_net_task) xTaskNotifyGive(s_net_task);
}

bool net_connected() { return s_connected; }

const char* net_state_str() {
  if (s_scanning)  return "scanning";
  if (s_connected) return "connected";
  if (!s_creds)    return "no wifi.cfg";
  return "connecting";
}

uint8_t net_snapshot(LanDevice* out, uint8_t max) {
  if (!s_mtx) return 0;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  uint8_t n = s_count < max ? s_count : max;
  memcpy(out, s_devs, n * sizeof(LanDevice));
  xSemaphoreGive(s_mtx);
  return n;
}

uint32_t net_generation() { return s_generation; }

String net_local_ip() {
  return s_connected ? WiFi.localIP().toString() : String("0.0.0.0");
}
