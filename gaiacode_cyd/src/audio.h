#pragma once
#include <Arduino.h>

// GaiaCode CYD — I2S audio pipeline (ES8311 codec + SC8002B amp).
// Pins per Freenove FNK0104N examples (Sketch_07.1/07.2/18.1 — three-way
// consensus; supersedes the older pinout note):
#define I2S_MCK   17   // codec master clock (256 x Fs from the I2S driver)
#define I2S_BCLK  18   // bit clock
#define I2S_LRCK  21   // word select / LRCK
#define I2S_DOUT  15   // ESP32 -> codec DIN
#define I2S_DIN   16   // codec DOUT -> ESP32 (unused, wired for completeness)
#define PA_EN      1   // amplifier shutdown: LOW = amp enabled (active-low SD)

#define AUDIO_SAMPLE_RATE 44100   // 44.1 kHz, 16-bit stereo frames
#define AUDIO_I2S_PORT    I2S_NUM_0

// Procedural sound profiles (no .wav — pure math into the DMA buffer).
enum SoundId : uint8_t {
  SOUND_NAV_ZAP,      // DarkPsy zap  — navigation / tab switching
  SOUND_ACTION_KICK,  // HardTek kick — primary/destructive actions
  SOUND_PING_FAULT,   // NeuroFunk growl — sensor ping-fault alert
};

bool audio_init();                      // I2S driver + ES8311 codec + amp on
void audio_set_volume(uint8_t pct);     // codec DAC volume 0..100
void audio_click(uint8_t volume_pct);   // legacy short tick (sensor matrix)
void audio_play(SoundId id);            // queue a synth profile (non-blocking)
