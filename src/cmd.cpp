//
// Created by awalol on 2026/5/4.
//

#include "cmd.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "audio.h"
#include "bt.h"
#include "config.h"
#include "device/usbd.h"
#include "pico/time.h"
#include "slots.h"
#include "hardware/clocks.h"
#include "hardware/adc.h"
#include "hardware/vreg.h"

uint16_t cpu_temp_raw_smoothed() {
    // One-time ADC bring-up. This is the only place the ADC is initialised
    // now (oled.cpp's CPU screen calls through here too). Runs on core0
    // under the cooperative main loop; adc_select_input(4) is set before
    // every read, so the shared ADC needs no locking.
    static bool adc_ready = false;
    if (!adc_ready) {
        adc_init();
        adc_set_temp_sensor_enabled(true);
        adc_ready = true;
    }
    adc_select_input(4);

    // The temp sensor has a shallow slope (-1.721 mV/C) and ~1 LSB ≈ 0.47 C,
    // so a lone 12-bit sample swings several tenths of a degree frame to
    // frame. Average a big block to kill that...
    constexpr int kSamples = 256;
    uint32_t acc = 0;
    for (int i = 0; i < kSamples; i++) acc += adc_read();
    const float mean = (float)acc / (float)kSamples;

    // ...then a slow EMA so the displayed value glides to the true die
    // temperature rather than mirroring the latest block. Seeded on the
    // first call so it doesn't ramp up from zero.
    static float ema = -1.0f;
    if (ema < 0.0f) ema = mean;
    else            ema += (mean - ema) * 0.15f;
    return (uint16_t)(ema + 0.5f);
}

bool is_pico_cmd(uint8_t report_id) {
    if (report_id == 0xf6 ||
        report_id == 0xf7 ||
        report_id == 0xf8 ||
        report_id == 0xf9 ||
        report_id == 0xfa ||
        report_id == 0xfb ||
        report_id == 0xfc
    ) {
        return true;
    }
    return false;
}

uint16_t pico_cmd_get(uint8_t report_id, uint8_t *buffer, uint16_t reqlen) {
    if (report_id == 0xf7) {
        printf("[HID] Receive 0xf7 getting config\n");
        if (sizeof(Config_body) > reqlen) {
            printf("[Config] Warning: Config_body overflow\n");
        }
        const auto len = std::min(sizeof(Config_body),static_cast<size_t>(reqlen));
        memcpy(buffer,&get_config(),len);
        return len;
    }
    if (report_id == 0xf8) {
        printf("[HID] Receive 0xf8 getting firmware version\n");
        const auto len = std::min(strlen(PICO_PROGRAM_VERSION_STRING), static_cast<size_t>(reqlen));
        memcpy(buffer, PICO_PROGRAM_VERSION_STRING, len);
        return len;
    }
    if (report_id == 0xf9) {
        // [-128,0]
        int8_t rssi = 0;
        bt_get_signal_strength(&rssi);
        if (reqlen == 0) {
            return 0;
        }
        buffer[0] = rssi;
#if ENABLE_VERBOSE
        printf("[HID] 0xf9 RSSI=%d raw=0x%02X\n", rssi, buffer[0]);
#endif
        return 1;
    }
    if (report_id == 0xfa) {
        // OLED Edition: 4 x bd_addr (6 bytes each) + 4 x occupied flag = 28 bytes.
        constexpr uint16_t want = 28;
        if (reqlen < want) {
            printf("[HID] 0xfa reqlen=%u too small for slots payload (%u)\n", reqlen, want);
            return 0;
        }
        for (int i = 0; i < 4; i++) {
            uint8_t addr[6];
            bt_slot_get_addr(i, addr);
            memcpy(buffer + i * 6, addr, 6);
        }
        for (int i = 0; i < 4; i++) {
            buffer[24 + i] = bt_slot_occupied(i) ? 1 : 0;
        }
        return want;
    }
    if (report_id == 0xfb) {
        // OLED Edition: diagnostics + audio meters for the web emulator.
        constexpr uint16_t want = 18;
        if (reqlen < want) {
            printf("[HID] 0xfb reqlen=%u too small for diag payload (%u)\n", reqlen, want);
            return 0;
        }
        const uint32_t uptime_s   = time_us_32() / 1000000u;
        const uint32_t usb_frames = audio_usb_frames();
        const uint32_t bt_packets = audio_bt_packets();
        const uint32_t hci_errs   = bt_hci_err_count();
        memcpy(buffer + 0,  &uptime_s,   4);
        memcpy(buffer + 4,  &usb_frames, 4);
        memcpy(buffer + 8,  &bt_packets, 4);
        buffer[12] = audio_peak_speaker();
        buffer[13] = audio_peak_haptic();
        memcpy(buffer + 14, &hci_errs,   4);
        return want;
    }
    if (report_id == 0xfc) {
        // OLED Edition: CPU / Clock telemetry for the web emulator. 11 bytes:
        //   [0..3]  set_khz  uint32  configured clk_sys (SYS_CLOCK_KHZ)
        //   [4..7]  real_khz uint32  measured clk_sys (cached, see below)
        //   [8]     vcode    uint8   vreg_get_voltage() raw enum code
        //   [9..10] temp_raw uint16  ADC ch4 12-bit reading
        // The web side does the volts/temperature math (same formulas as
        // render_screen_cpu) so the firmware HID path stays float-free.
        constexpr uint16_t want = 11;
        if (reqlen < want) {
            printf("[HID] 0xfc reqlen=%u too small for cpu payload (%u)\n", reqlen, want);
            return 0;
        }
        const uint32_t set_khz = (uint32_t)SYS_CLOCK_KHZ;

        // clk_sys is fixed at boot and frequency_count_khz() busy-waits a few
        // ms — measure exactly once (lazily) and cache. Doing it here on the
        // first poll keeps it off the boot path; one ~ms stall in a single
        // GET_REPORT is acceptable.
        static uint32_t cached_real_khz = 0;
        if (cached_real_khz == 0) {
            cached_real_khz = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS);
        }

        const uint16_t temp_raw = cpu_temp_raw_smoothed();

        const uint8_t vcode = (uint8_t)vreg_get_voltage();

        memcpy(buffer + 0, &set_khz,         4);
        memcpy(buffer + 4, &cached_real_khz, 4);
        buffer[8] = vcode;
        memcpy(buffer + 9, &temp_raw,        2);
        return want;
    }
    return 0;
}

void pico_cmd_set(uint8_t report_id, uint8_t const *buffer, uint16_t bufsize) {
    (void) report_id;
    if (bufsize == 0) {
        return;
    }

    // 0x01 update config in variable
    // 0x02 write config to flash
    // 0x03 reconnect tinyusb device;
    if (buffer[0] == 0x01) {
        printf("[CMD] Enter config set func\n");
        set_config(buffer + 1, bufsize - 1);
    }
    if (buffer[0] == 0x02) {
        printf("[CMD] Enter config save func\n");
        config_save();
    }
    if (buffer[0] == 0x03) {
        printf("[CMD] Enter tud reconnect func\n");
        tud_disconnect();
        sleep_ms(150);
        tud_connect();
    }
}
