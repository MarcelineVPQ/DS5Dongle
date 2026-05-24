//
// Created by awalol on 2026/5/4.
//

#ifndef DS5_BRIDGE_CONFIG_H
#define DS5_BRIDGE_CONFIG_H

#include <cstdint>

struct __attribute__((packed)) Config_body {
    uint8_t config_version; // Config Version
    float haptics_gain; // [1.0,2.0]
    float speaker_volume; // [-100,0]
    uint8_t inactive_time; // [5,60] min
    uint8_t disable_inactive_disconnect; // bool: 0 disable,1 enable
    uint8_t disable_pico_led; // bool
    uint8_t polling_rate_mode; // 0: 250Hz, 1: 500Hz, 2: real-time
    uint8_t audio_buffer_length; // [16,128]
    uint8_t controller_mode; // 0: DS5, 1: DSE, 2: Auto
    uint8_t current_slot;    // [0..3] active multi-pairing slot (OLED Edition Phase G)
    // Audio Auto Haptics — derive haptic feedback from speaker audio for games that
    // send no per-frame native haptic data. DSP borrowed from loteran/DS5Dongle 5d6bc2f.
    uint8_t auto_haptics_enable;  // 0=Off, 1=Fallback (default), 2=Mix, 3=Replace
    uint8_t auto_haptics_gain;    // [0,200] percent, default 100
    uint8_t auto_haptics_lowpass; // 0=80Hz, 1=160Hz (default), 2=250Hz, 3=400Hz
    // Lightbar (OLED Edition Phase H): persisted so the chosen mode/colors
    // survive reboot and stick across all screens. lightbar_mode indexes the
    // OLED Lightbar screen's mode list — 0=LIVE, 1..4=FAV0..3, 5=BREATHING,
    // 6=RAINBOW, 7=FADE, 8=HOST (passthrough, the safe default that lets the
    // host/game own the LED). Keep this numbering in sync with kNumLbModes /
    // kLbModeHost in src/oled.cpp. Erased flash (0xFF) → HOST + white favorites.
    uint8_t lightbar_mode;
    uint8_t lb_fav_r[4];
    uint8_t lb_fav_g[4];
    uint8_t lb_fav_b[4];
    // OLED idle power-ladder thresholds, in minutes. 0 = that tier disabled.
    // Defaults preserve the original hardcoded ladder (2 min dim, 15 min off).
    // Range [0,250] (0xFF erased flash → default via config_valid clamp). The
    // idle timer is 64-bit µs so the full range is representable. Issue #5.
    uint8_t screen_dim_timeout;
    uint8_t screen_off_timeout;
    // DualSense mic over Bluetooth (Phase I). 0 = off, 1 = on (default). When on,
    // the dongle asserts the DS5 mic-enable bit so the controller streams its mic
    // over BT and the dongle decodes it to the USB capture endpoint. Costs extra
    // DS5 battery (keeps its audio subsystem awake), hence the toggle.
    uint8_t bt_mic_enable;
};

struct __attribute__((packed)) Config {
    uint32_t magic;
    uint16_t version;
    uint32_t crc32; // Config_body crc32, only calc and verify when save
    uint16_t size;  // Config_body size
    Config_body body;
};

void config_default();
void config_load();
bool config_save();
const Config_body& get_config();
void set_config(const uint8_t *new_config, const uint16_t len);
void config_valid();
void set_config(const Config_body &new_config);
extern bool is_dse;

#endif //DS5_BRIDGE_CONFIG_H
