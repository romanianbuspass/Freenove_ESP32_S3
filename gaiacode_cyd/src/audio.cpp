#include "audio.h"
#include "driver/i2s.h"
#include "es8311.h"
#include <math.h>

static es8311_handle_t s_es = NULL;
static QueueHandle_t   s_sound_q = NULL;

void audio_synth_task(void* arg);   // defined below (task entry)

// ES8311 clock plan: 44.1 kHz, 16-bit. The IDF legacy driver emits
// MCLK = 256 x Fs = 11.2896 MHz — coeff-table row {11289600, 44100, ...}.

bool audio_init() {
  // Amplifier: active-low shutdown — drive LOW to enable.
  pinMode(PA_EN, OUTPUT);
  digitalWrite(PA_EN, LOW);

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = AUDIO_SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;      // stereo frames
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S; // Philips
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 2;          // per brief: 2 x 1024-frame DMA
  cfg.dma_buf_len = 1024;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;  // silence on underrun, no stale looping
  cfg.fixed_mclk = 0;

  if (i2s_driver_install(AUDIO_I2S_PORT, &cfg, 0, NULL) != ESP_OK) {
    Serial.println("AUDIO: i2s_driver_install failed");
    return false;
  }
  i2s_pin_config_t pins = {};
  pins.mck_io_num   = I2S_MCK;
  pins.bck_io_num   = I2S_BCLK;
  pins.ws_io_num    = I2S_LRCK;
  pins.data_out_num = I2S_DOUT;
  pins.data_in_num  = I2S_DIN;
  if (i2s_set_pin(AUDIO_I2S_PORT, &pins) != ESP_OK) {
    Serial.println("AUDIO: i2s_set_pin failed");
    return false;
  }

  // Codec over the shared touch/audio I2C bus (Wire already up on I2C_NUM_0).
  es8311_handle_t es = es8311_create(I2C_NUM_0, ES8311_ADDRESS_0);
  if (!es) {
    Serial.println("AUDIO: es8311_create failed");
    return false;
  }
  s_es = es;
  const es8311_clock_config_t clk = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = AUDIO_SAMPLE_RATE * 256,
    .sample_frequency = AUDIO_SAMPLE_RATE,
  };
  esp_err_t err = es8311_init(es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
  if (err != ESP_OK) {
    Serial.printf("AUDIO: es8311_init failed (%d)\n", err);
    return false;
  }
  es8311_voice_volume_set(es, 85, NULL);

  s_sound_q = xQueueCreate(4, sizeof(SoundId));
  xTaskCreatePinnedToCore(audio_synth_task, "synth", 4096, NULL, 2, NULL, 0);

  Serial.printf("AUDIO: I2S %d Hz 16-bit + ES8311 up (MCLK=%d Hz)\n",
                AUDIO_SAMPLE_RATE, AUDIO_SAMPLE_RATE * 256);
  return true;
}

void audio_set_volume(uint8_t pct) {
  if (s_es) es8311_voice_volume_set(s_es, pct, NULL);
}

// ============================================================================
// FM/PM synthesis engine — discrete-time phase accumulators, chunk rendering.
// ============================================================================

struct Synth {
  SoundId  id;
  uint32_t n;          // sample index
  uint32_t total;      // total samples in the profile
  float    ph_c;       // carrier phase accumulator (cycles, 0..1)
  float    ph_c2;      // second carrier (growl)
  float    ph_m;       // modulator phase accumulator
};

static void synth_begin(Synth* s, SoundId id) {
  memset(s, 0, sizeof(*s));
  s->id = id;
  switch (id) {
    case SOUND_NAV_ZAP:     s->total = AUDIO_SAMPLE_RATE * 0.12f; break;
    case SOUND_ACTION_KICK: s->total = AUDIO_SAMPLE_RATE * 0.25f; break;
    case SOUND_PING_FAULT:  s->total = AUDIO_SAMPLE_RATE * 0.45f; break;
  }
}

// 1. DarkPsy Zap — FM sweep; exponential envelopes on amplitude AND on the
//    modulation index itself.
//    y[n] = A e^{-n/tau} sin(2π fc/Fs n + I e^{-n/tau_i} sin(2π fm/Fs n))
static float synth_zap(Synth* s) {
  const float A = 0.85f, fc = 3500.0f, fm = 1250.0f, I = 8.0f;
  const float tau = 0.050f, tau_i = 0.015f;
  float t  = (float)s->n / AUDIO_SAMPLE_RATE;
  float mod = sinf(2.0f * (float)M_PI * s->ph_m);
  float env = expf(-t / tau);
  float idx = I * expf(-t / tau_i);
  float y = A * env * sinf(2.0f * (float)M_PI * s->ph_c + idx * mod);
  s->ph_c += fc / AUDIO_SAMPLE_RATE;  if (s->ph_c >= 1.0f) s->ph_c -= 1.0f;
  s->ph_m += fm / AUDIO_SAMPLE_RATE;  if (s->ph_m >= 1.0f) s->ph_m -= 1.0f;
  return y;
}

// 2. HardTek Kick — exponential pitch-drop on the carrier, hard tanh drive.
//    The brief's formula writes f_start e^{-n/tau_p} multiplied by n; the
//    discrete-time correct form integrates the instantaneous frequency into
//    the phase accumulator (which is what we do here).
static float synth_kick(Synth* s) {
  const float A = 0.95f, f_start = 160.0f, D = 4.0f;
  const float tau_a = 0.13f, tau_p = 0.025f;
  float t   = (float)s->n / AUDIO_SAMPLE_RATE;
  float f   = f_start * expf(-t / tau_p);
  float env = expf(-t / tau_a);
  float y   = A * env * tanhf(D * sinf(2.0f * (float)M_PI * s->ph_c));
  s->ph_c += f / AUDIO_SAMPLE_RATE;   if (s->ph_c >= 1.0f) s->ph_c -= 1.0f;
  return y;
}

// 3. NeuroFunk Growl — two detuned carriers (phase-cancellation beating),
//    heavy high-index low-frequency modulation.
//    y[n] = e^{-n/tau} [ sin(th1 + I sin thm) + sin(th2 + I sin thm) ]
static float synth_growl(Synth* s) {
  const float f1 = 55.0f, f2 = 56.1f, fm = 27.0f, I = 12.0f;
  const float tau = 0.28f;
  float t   = (float)s->n / AUDIO_SAMPLE_RATE;
  float mod = I * sinf(2.0f * (float)M_PI * s->ph_m);
  float s1  = sinf(2.0f * (float)M_PI * s->ph_c  + mod);
  float s2  = sinf(2.0f * (float)M_PI * s->ph_c2 + mod);
  float y   = 0.45f * expf(-t / tau) * (s1 + s2);
  s->ph_c  += f1 / AUDIO_SAMPLE_RATE; if (s->ph_c  >= 1.0f) s->ph_c  -= 1.0f;
  s->ph_c2 += f2 / AUDIO_SAMPLE_RATE; if (s->ph_c2 >= 1.0f) s->ph_c2 -= 1.0f;
  s->ph_m  += fm / AUDIO_SAMPLE_RATE; if (s->ph_m  >= 1.0f) s->ph_m  -= 1.0f;
  return y;
}

static float synth_sample(Synth* s) {
  switch (s->id) {
    case SOUND_NAV_ZAP:     return synth_zap(s);
    case SOUND_ACTION_KICK: return synth_kick(s);
    default:                return synth_growl(s);
  }
}

void audio_synth_task(void* arg) {
  (void)arg;
  enum { CHUNK = 256 };                     // frames per render/write block
  static int16_t buf[CHUNK * 2];            // stereo
  SoundId id;
  for (;;) {
    if (xQueueReceive(s_sound_q, &id, portMAX_DELAY) != pdTRUE) continue;
    Synth s;
    synth_begin(&s, id);
    while (s.n < s.total) {
      uint32_t frames = s.total - s.n;
      if (frames > CHUNK) frames = CHUNK;
      for (uint32_t i = 0; i < frames; i++, s.n++) {
        int16_t v = (int16_t)(synth_sample(&s) * 32767.0f);
        buf[i * 2] = v;
        buf[i * 2 + 1] = v;
      }
      size_t written = 0;
      i2s_write(AUDIO_I2S_PORT, buf, frames * 4, &written, portMAX_DELAY);
    }
    i2s_zero_dma_buffer(AUDIO_I2S_PORT);    // clean tail
  }
}

void audio_play(SoundId id) {
  if (s_sound_q) xQueueSend(s_sound_q, &id, 0);   // drop if queue full
}

// ---- Legacy UI tick: 8 ms percussive square burst (sensor matrix) ----------
void audio_click(uint8_t volume_pct) {
  enum { FRAMES = AUDIO_SAMPLE_RATE / 125 };   // 8 ms of stereo frames
  static int16_t buf[FRAMES * 2];

  const float f0 = 1800.0f, f1 = 700.0f;
  float phase = 0.0f;
  int peak = (int)(32767.0f * 0.5f * (volume_pct / 100.0f));

  for (int i = 0; i < FRAMES; i++) {
    float t = (float)i / FRAMES;
    float freq = f0 + (f1 - f0) * t;
    phase += freq / AUDIO_SAMPLE_RATE;
    if (phase >= 1.0f) phase -= 1.0f;
    float env = expf(-6.0f * t);
    int16_t v = (int16_t)((phase < 0.5f ? peak : -peak) * env);
    buf[i * 2] = v;
    buf[i * 2 + 1] = v;
  }

  size_t written = 0;
  i2s_write(AUDIO_I2S_PORT, buf, sizeof(buf), &written, 0);
}
