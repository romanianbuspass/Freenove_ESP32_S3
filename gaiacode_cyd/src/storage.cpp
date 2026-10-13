#include "storage.h"
#include <SD_MMC.h>
#include <FS.h>

// SDMMC pin map — FNK0104N internal slot (per board JSON).
static const int PIN_CLK = 5;
static const int PIN_CMD = 4;
static const int PIN_D0  = 6;
static const int PIN_D1  = 7;
static const int PIN_D2  = 2;   // only used in 4-bit mode
static const int PIN_D3  = 3;   // only used in 4-bit mode (strapping pin — fine post-boot)

static bool s_mounted = false;

// SD_MMC (Arduino FS/SDMMC) is NOT thread-safe. storage_* is called from
// gnss_task, net_task, the LVGL timer, and setup — serialize every driver
// access. First breach observed 2026-08-11: GNSS LOCK log from gnss_task
// racing LAN scan logging -> hang -> watchdog reboot at fix acquisition.
static SemaphoreHandle_t s_mtx = NULL;
#define SD_LOCK()   do { if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY); } while (0)
#define SD_UNLOCK() do { if (s_mtx) xSemaphoreGive(s_mtx); } while (0)

static bool try_mount(bool four_bit) {
  if (four_bit) SD_MMC.setPins(PIN_CLK, PIN_CMD, PIN_D0, PIN_D1, PIN_D2, PIN_D3);
  else          SD_MMC.setPins(PIN_CLK, PIN_CMD, PIN_D0);
  // mode1bit = !four_bit; no auto-format, default frequency.
  return SD_MMC.begin("/sdcard", !four_bit);
}

static const char* card_type_str(uint8_t t) {
  switch (t) {
    case CARD_MMC:  return "MMC";
    case CARD_SD:   return "SDSC";
    case CARD_SDHC: return "SDHC/SDXC";
    default:        return "UNKNOWN";
  }
}

static void ensure_dir(const char* path) {
  if (SD_MMC.exists(path)) {
    Serial.printf("SD: %s exists\n", path);
    return;
  }
  if (SD_MMC.mkdir(path)) Serial.printf("SD: created %s\n", path);
  else                    Serial.printf("SD: FAILED to create %s\n", path);
}

bool storage_init() {
  s_mtx = xSemaphoreCreateMutex();   // before any SD access, incl. boot logs
  Serial.println("SD: mounting internal slot (SDMMC 4-bit)...");
  if (!try_mount(true)) {
    Serial.println("SD: 4-bit mount failed — retrying 1-bit...");
    SD_MMC.end();
    if (!try_mount(false)) {
      Serial.println("SD: mount FAILED (no card, wiring, or unsupported FS).");
      s_mounted = false;
      return false;
    }
    Serial.println("SD: mounted in 1-bit mode.");
  } else {
    Serial.println("SD: mounted in 4-bit mode.");
  }

  uint8_t ct = SD_MMC.cardType();
  if (ct == CARD_NONE) {
    Serial.println("SD: driver up but no card present.");
    s_mounted = false;
    return false;
  }

  uint64_t total = SD_MMC.totalBytes();
  uint64_t used  = SD_MMC.usedBytes();
  Serial.printf("SD: card type %s, size %llu MB, used %llu MB, free %llu MB\n",
                card_type_str(ct),
                total / (1024ULL * 1024ULL),
                used  / (1024ULL * 1024ULL),
                (total - used) / (1024ULL * 1024ULL));

  ensure_dir("/firmware");
  ensure_dir("/logs");

  s_mounted = true;
  char msg[96];
  snprintf(msg, sizeof(msg), "mounted %s %llu MB",
           card_type_str(ct), total / (1024ULL * 1024ULL));
  storage_log("SD", msg);
  return true;
}

void storage_report() {
  if (!s_mounted) {
    Serial.println("SD: not mounted.");
    return;
  }
  SD_LOCK();
  uint64_t total = SD_MMC.totalBytes();
  uint64_t used  = SD_MMC.usedBytes();
  uint8_t ct = SD_MMC.cardType();
  bool fw_ok = SD_MMC.exists("/firmware");
  bool lg_ok = SD_MMC.exists("/logs");
  SD_UNLOCK();
  Serial.printf("SD: card type %s, size %llu MB, used %llu MB, free %llu MB\n",
                card_type_str(ct),
                total / (1024ULL * 1024ULL),
                used  / (1024ULL * 1024ULL),
                (total - used) / (1024ULL * 1024ULL));
  Serial.printf("SD: /firmware %s, /logs %s\n",
                fw_ok ? "ok" : "MISSING", lg_ok ? "ok" : "MISSING");
}

bool storage_mounted() { return s_mounted; }

uint8_t storage_list_hex(char names[][40], uint8_t max) {
  if (!s_mounted) return 0;
  SD_LOCK();
  File dir = SD_MMC.open("/firmware");
  if (!dir || !dir.isDirectory()) { SD_UNLOCK(); return 0; }
  uint8_t n = 0;
  for (File f = dir.openNextFile(); f && n < max; f = dir.openNextFile()) {
    if (f.isDirectory()) continue;
    const char* name = f.name();
    size_t len = strlen(name);
    // .hex suffix, case-insensitive
    if (len > 4 && strcasecmp(name + len - 4, ".hex") == 0) {
      snprintf(names[n], 40, "%s", name);
      n++;
    }
  }
  dir.close();
  SD_UNLOCK();
  return n;
}

void storage_log(const char* tag, const char* msg) {
  if (!s_mounted) return;
  SD_LOCK();
  File f = SD_MMC.open("/logs/cyd.log", FILE_APPEND);
  if (!f) { SD_UNLOCK(); return; }
  f.printf("T+%lu [%s] %s\n", (unsigned long)millis(), tag, msg);
  f.close();
  SD_UNLOCK();
}

// ---- Wi-Fi credentials on SD --------------------------------------------------
static const char* WIFI_CFG = "/wifi.cfg";
static const char* WIFI_TEMPLATE =
  "# GaiaCode CYD Wi-Fi credentials\n"
  "# Fill in SSID and PASS, save, then power-cycle the unit.\n"
  "SSID=YourNetworkName\n"
  "PASS=YourPassword\n";

bool storage_wifi_creds(char* ssid, size_t ssid_len, char* pass, size_t pass_len) {
  ssid[0] = pass[0] = '\0';
  if (!s_mounted) return false;
  SD_LOCK();
  if (!SD_MMC.exists(WIFI_CFG)) {
    File f = SD_MMC.open(WIFI_CFG, FILE_WRITE);
    if (f) { f.print(WIFI_TEMPLATE); f.close(); }
    SD_UNLOCK();   // release BEFORE storage_log (would self-deadlock)
    Serial.println("SD: wrote /wifi.cfg template — fill in and reboot");
    storage_log("SD", "wrote /wifi.cfg template");
    return false;
  }
  File f = SD_MMC.open(WIFI_CFG, FILE_READ);
  if (!f) { SD_UNLOCK(); return false; }
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.startsWith("#")) continue;
    if (line.startsWith("SSID=")) line.substring(5).toCharArray(ssid, ssid_len);
    else if (line.startsWith("PASS=")) line.substring(5).toCharArray(pass, pass_len);
  }
  f.close();
  SD_UNLOCK();
  // Reject empty or untouched template.
  if (ssid[0] == '\0' || strcmp(ssid, "YourNetworkName") == 0) return false;
  return true;
}

uint64_t storage_card_size_mb() {
  if (!s_mounted) return 0;
  SD_LOCK();
  uint64_t v = SD_MMC.cardSize() / (1024ULL * 1024ULL);
  SD_UNLOCK();
  return v;
}

uint64_t storage_used_bytes() {
  if (!s_mounted) return 0;
  SD_LOCK();
  uint64_t v = SD_MMC.usedBytes();
  SD_UNLOCK();
  return v;
}
