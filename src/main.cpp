//
// Created by awalol on 2026/3/4.
//

#include <cstdio>
#include "bsp/board_api.h"
#include "bt.h"
#include "utils.h"
#include "resample.h"
#include "audio.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "state_mgr.h"
#if ENABLE_SERIAL
#include "pico/stdio_usb.h"
#endif
#include "config.h"
#include "cmd.h"
#if ENABLE_BATT_LED
#include "battery_led.h"
#endif
#include "oled.h"

// Pico SDK speciifically for waiting on conditions
#include "pico/critical_section.h"

int reportSeqCounter = 0;
uint8_t packetCounter = 0;
bool spk_active = false;

// Mic-debug instrumentation: count every 0x31 BT input report regardless
// of mic-tag bit, accumulate OR-mask of every byte-2 value seen (tells us
// which bits ever fire) and remember the last byte-2 value. Also track
// observed frame-length range. Surfaced on the OLED Diagnostics screen.
volatile uint32_t g_bt_31_packets = 0;
volatile uint32_t g_bt_other_packets = 0;
volatile uint8_t  g_last_other_id = 0;
volatile uint8_t  g_other_id_or = 0;
volatile uint8_t  g_last_31_b2 = 0;
volatile uint8_t  g_31_b2_or = 0;
volatile uint16_t g_31_len_min = 0xFFFF;
volatile uint16_t g_31_len_max = 0;
volatile uint8_t  g_mic_prefix[6] = {0};
volatile uint8_t  g_last_other_prefix[8] = {0};
volatile uint8_t  g_last_any_prefix[16] = {0};
volatile uint16_t g_longest_len = 0;
volatile uint8_t  g_longest_frame[80] = {0};
uint32_t bt_31_packet_count() { return g_bt_31_packets; }
uint8_t  bt_31_last_byte2()  { return g_last_31_b2; }
uint8_t  bt_31_b2_or_mask()  { return g_31_b2_or; }
uint16_t bt_31_len_min()     { return g_31_len_min == 0xFFFF ? 0 : g_31_len_min; }
uint16_t bt_31_len_max()     { return g_31_len_max; }
void bt_31_mic_prefix(uint8_t out[6]) {
    for (int i = 0; i < 6; i++) out[i] = g_mic_prefix[i];
}

// Trigger-flow diagnostics. Counts host → dongle → BT path for adaptive
// trigger effects. Lets us tell which link in the chain breaks when games
// like Death Stranding 2 don't produce trigger tension via the dongle:
//   out02_total     - every 0x02 HID OUT report received from host
//   out02_trig_allow - of those, how many set AllowRight/LeftTriggerFFB
//                     (valid_flag0 bits 2 & 3) — i.e. the host actually
//                     told us "apply trigger FFB"
//   out02_to_bt     - 0x02 reports that we forwarded to the controller as
//                     a BT 0x31 sub-0x10 packet (gated off when speaker is
//                     active; audio.cpp's 0x36 path carries state then)
//   out02_trig_folded - of the trig_allow reports, how many arrived while the
//                     speaker stream was active and were therefore NOT sent as
//                     a standalone 0x31 — their trigger FFB was folded into the
//                     0x36 audio frames via state[]. So trig_allow == to_bt's
//                     trigger share + this, proving the "missing" forwards
//                     (issue #6) aren't drops. Surfaced on the Diag screen.
// Surfaced on the OLED Diagnostics screen.
volatile uint32_t g_host_out02_total = 0;
volatile uint32_t g_host_out02_trig_allow = 0;
volatile uint32_t g_host_out02_to_bt = 0;
volatile uint32_t g_host_out02_trig_folded = 0;
uint32_t host_out02_total()       { return g_host_out02_total; }
uint32_t host_out02_trig_allow()  { return g_host_out02_trig_allow; }
uint32_t host_out02_to_bt()       { return g_host_out02_to_bt; }
uint32_t host_out02_trig_folded() { return g_host_out02_trig_folded; }

uint8_t interrupt_in_data[63] = {
    0x7f, 0x7d, 0x7f, 0x7e, 0x00, 0x00, 0xa7,
    0x08, 0x00, 0x00, 0x00, 0x52, 0x43, 0x30, 0x41,
    0x01, 0x00, 0x0e, 0x00, 0xef, 0xff, 0x03, 0x03,
    0x7b, 0x1b, 0x18, 0xf0, 0xcc, 0x9c, 0x60, 0x00,
    0xfc, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x09, 0x09, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xa7, 0xad, 0x60, 0x00, 0x29, 0x18, 0x00,
    0x53, 0x9f, 0x28, 0x35, 0xa5, 0xa8, 0x0c, 0x8b
};

critical_section_t report_cs;
volatile bool report_dirty = false;

void interrupt_loop() {
    // OLED Edition: hold PS + Mute for 2 seconds to soft-reboot the dongle.
    // Works whether or not the OLED add-on is present. PS+Mute is uncommon
    // during gameplay and the long hold avoids accidental triggers.
    {
        static uint32_t combo_first_us = 0;
        constexpr uint8_t kComboBits = 0x05;       // byte 9: PS (bit 0) + Mute (bit 2)
        constexpr uint32_t kComboHoldUs = 2000000; // 2 seconds
        const bool held = (interrupt_in_data[9] & kComboBits) == kComboBits;
        if (held) {
            const uint32_t now = time_us_32();
            if (combo_first_us == 0) combo_first_us = now;
            else if ((now - combo_first_us) > kComboHoldUs) watchdog_reboot(0, 0, 0);
        } else {
            combo_first_us = 0;
        }
    }

    if (!tud_hid_ready()) return;

    // TODO: Refactor for better code reuse
    if (get_config().polling_rate_mode != 2) {
        if (!tud_hid_report(0x01, interrupt_in_data, 63)) {
            printf("[USBHID] tud_hid_report error\n");
        }
        return;
    }

    bool should_send = false;
    // Local buffer to hold the report data while we prepare it to send. 
    uint8_t safe_report[63];


    critical_section_enter_blocking(&report_cs);
    if (report_dirty) {
        memcpy(safe_report, interrupt_in_data, 63);
        report_dirty = false;
        should_send = true;
    }
    critical_section_exit(&report_cs);

    // Only send to TinyUSB if we actually grabbed fresh data
    if (should_send) {
        if (!tud_hid_report(0x01, safe_report, 63)) {
            printf("[USBHID] tud_hid_report error\n");

            // If the report failed to queue, restore the dirty flag 
            // so we try again on the next loop iteration.
            critical_section_enter_blocking(&report_cs);
            report_dirty = true;
            critical_section_exit(&report_cs);
        }
    }
}

void on_bt_data(CHANNEL_TYPE channel, uint8_t *data, uint16_t len) {
    // printf("[Main] BT data callback: channel=%u len=%u\n", channel, len);
    // Track ALL INTERRUPT input reports, not just 0x31. The mic stream
    // may live on a different report ID — confirmed 2026-05-19 that data[2]
    // bit 0 (and bit 1) is NOT a mic flag, just the report-type indicator;
    // every "mic-tagged" frame turned out to be standard input.
    if (channel == INTERRUPT && len > 1) {
        if (data[1] == 0x31) g_bt_31_packets++;
        else {
            g_bt_other_packets++;
            g_last_other_id = data[1];
            g_other_id_or = (uint8_t)(g_other_id_or | data[1]);
            for (uint16_t i = 0; i < 8 && i < len; i++) {
                g_last_other_prefix[i] = data[i];
            }
        }
        if (len > 2) {
            g_last_31_b2 = data[2];
            g_31_b2_or = (uint8_t)(g_31_b2_or | data[2]);
        }
        if (len < g_31_len_min) g_31_len_min = len;
        if (len > g_31_len_max) g_31_len_max = len;
        for (uint16_t i = 0; i < 16 && i < len; i++) {
            g_last_any_prefix[i] = data[i];
        }

        // Capture the entire content of the longest 0x31 frame we've
        // seen. Long frames almost certainly carry the mic audio appended
        // after the standard 63-byte input report — this lets us look
        // at the trailing bytes directly via 0xFD diagnostic.
        if (data[1] == 0x31 && len > g_longest_len) {
            g_longest_len = len;
            for (uint16_t i = 0; i < 80 && i < len; i++) {
                g_longest_frame[i] = data[i];
            }
        }
    }

    // Mic-in tap (TEST): once the dongle asserts the mic-enable bit in the
    // outgoing 0x36 audio report (pkt[4] bit 0, see audio.cpp — awalol
    // confirmed this is the key), the DS5 streams its mic as a 71-byte Opus
    // packet at data+4 of a 0x31 report with bit 1 of data[2] set. Route those
    // to the mic decoder instead of treating them as a standard input report.
    // The length guard (4-byte header + 71-byte Opus) keeps a stray short
    // frame from over-reading. The diagnostic counters above still observe
    // these frames, so the Diag screen's data[2] OR-mask will show bit 1 set
    // once the enable bit takes effect.
    // A mic-tagged 0x31 frame carries Opus audio at data+4, NOT a standard input
    // report — so it must ALWAYS be diverted here (decoded when mic is on, dropped
    // when off), never fall through to the input handler below. Letting it through
    // would copy Opus bytes into interrupt_in_data and corrupt sticks/buttons.
    if (channel == INTERRUPT && data[1] == 0x31 && ((data[2] >> 1) & 1)
        && len >= 75) {
        if (get_config().bt_mic_enable) mic_add_queue(data + 4);
        return;
    }

    if (channel == INTERRUPT && data[1] == 0x31) {
        if ((data[56] & 1) != (interrupt_in_data[53] & 1)) {
            set_headset(data[56] & 1);
        }

        if (get_config().polling_rate_mode != 2) {
            memcpy(interrupt_in_data, data + 3, 63);
#if ENABLE_BATT_LED
            battery_led_note_report();
#endif
            return;
        }

        critical_section_enter_blocking(&report_cs);
        memcpy(interrupt_in_data, data + 3, 63);
        report_dirty = true;
        critical_section_exit(&report_cs);
#if ENABLE_BATT_LED
        battery_led_note_report();
#endif
    }
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
    (void) itf;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) reqlen;

    if (is_pico_cmd(report_id)) {
        return pico_cmd_get(report_id, buffer, reqlen);
    }

    std::vector<uint8_t> feature_data = get_feature_data(report_id, reqlen);
    if (!feature_data.empty()) {
        memcpy(buffer, feature_data.data() + 1, feature_data.size() - 1);
    }

    return feature_data.empty() ? 0 : feature_data.size() - 1;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void) rhport;
    uint8_t const itf = tu_u16_low(p_request->wIndex); // wInterface
    uint8_t const alt = tu_u16_low(p_request->wValue); // bAlternateSetting

    if (itf == 1) {
        printf("[AUDIO] Set interface Speaker to alternate setting %d\n", alt);
        spk_active = alt;
    }

    return true;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize) {
    (void) itf;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) bufsize;

    if (is_pico_cmd(report_id)) {
        printf("[HID] Receive 0xf6 setting config, funcid:0x%02X\n", buffer[0]);
        pico_cmd_set(report_id, buffer, bufsize);
        return;
    }

    // INTERRUPT OUT
    if (report_id == 0) {
        switch (buffer[0]) {
            case 0x02: {
                g_host_out02_total++;
                // valid_flag0 lives at buffer[1] (right after the 0x02 report id).
                // Bits 2 & 3 are AllowRight/LeftTriggerFFB.
                if (bufsize > 1 && (buffer[1] & 0x0C)) {
                    g_host_out02_trig_allow++;
                }
                state_update(buffer + 1, bufsize - 1);
                if (spk_active) {
                    // Not forwarded as a standalone 0x31 — the trigger FFB just
                    // written into state[] rides the 0x36 audio frames instead.
                    // Count the trigger-bearing ones so the Diag screen shows
                    // trig_allow == to_bt(trig) + folded (issue #6: not drops).
                    if (bufsize > 1 && (buffer[1] & 0x0C)) g_host_out02_trig_folded++;
                    break;
                }
                uint8_t outputData[78]{};
                outputData[0] = 0x31;
                outputData[1] = reportSeqCounter << 4;
                if (++reportSeqCounter == 256) {
                    reportSeqCounter = 0;
                }
                outputData[2] = 0x10;
                // memcpy(outputData + 3, buffer + 1, bufsize - 1);
                state_set(outputData + 3,sizeof(SetStateData));
                bt_write(outputData, sizeof(outputData));
                g_host_out02_to_bt++;
                break;
            }
        }
    }
    if (report_id == 0x80 ||
        // DSE: Write Profile Block
        report_id == 0x60 ||
        report_id == 0x62 ||
        report_id == 0x61) {
        set_feature_data(report_id, const_cast<uint8_t *>(buffer), bufsize);
        return;
    }
}

int main() {
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(1000);
    set_sys_clock_khz(SYS_CLOCK_KHZ, true);

    board_init();
    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL
    };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);
#if !ENABLE_SERIAL
    tud_disconnect();
#endif
    board_init_after_tusb();
#if ENABLE_SERIAL
    stdio_usb_init();
#endif

    if (cyw43_arch_init()) {
        printf("Failed to initialize CYW43\n");
        return 1;
    }
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);

#if ENABLE_BATT_LED
    battery_led_init();
#endif

#if !ENABLE_SERIAL
    if (watchdog_caused_reboot()) {
        printf("Rebooted by Watchdog!\n");
        // 当崩溃重启以后，闪三下灯
        for (int i = 0; i < 6; i++) {
            if (i % 2 == 0) {
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
            } else {
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
            }
            sleep_ms(500);
        }
    } else {
        printf("Clean boot\n");
    }
#endif

    // Initialize the critical section for the report buffer
    critical_section_init(&report_cs);

    config_load();

    bt_init();
    bt_register_data_callback(on_bt_data);

    audio_init();
    state_init();
    oled_init();

#if !ENABLE_SERIAL
    watchdog_enable(1000, true);
#endif

    while (1) {
#if !ENABLE_SERIAL
        watchdog_update();
#endif
        cyw43_arch_poll();
        bt_connection_watchdog_tick();
        tud_task();
        audio_loop();
        interrupt_loop();
        oled_loop();
#if ENABLE_BATT_LED
        battery_led_tick();
#endif
    }
}
