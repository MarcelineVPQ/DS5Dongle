#include "oled.h"
#include "oled_font.h"
#include "bt.h"
#include "slots.h"
#include "audio.h"
#include "config.h"
#include "state_mgr.h"

#include <cstdio>
#include <cstring>
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "cmd.h"
#include "pico/time.h"

extern uint8_t interrupt_in_data[63]; // defined in main.cpp

// Mic diagnostic counters (defined in main.cpp).
extern uint32_t bt_31_packet_count();
extern uint32_t host_out02_total();
extern uint32_t host_out02_trig_allow();
extern uint32_t host_out02_to_bt();
extern uint32_t host_out02_trig_folded();
extern uint8_t  bt_31_last_byte2();
extern uint8_t  bt_31_b2_or_mask();
extern uint16_t bt_31_len_min();
extern uint16_t bt_31_len_max();
extern void     bt_31_mic_prefix(uint8_t out[6]);
extern bool     spk_active; // main.cpp: true while host USB speaker stream is open

// Global (not in the anon namespace below) so state_mgr.cpp can extern it:
// true while an OLED lightbar mode or the charging pulse owns the LED, which
// tells state_update() to ignore the host's AllowLedColor writes.
bool g_lightbar_override = false;

namespace {

constexpr uint kPinDC = 8;
constexpr uint kPinCS = 9;
constexpr uint kPinCLK = 10;
constexpr uint kPinMOSI = 11;
constexpr uint kPinRST = 12;
constexpr uint kPinKey0 = 15;
constexpr uint kPinKey1 = 17;

constexpr int kW = 128;
constexpr int kH = 64;
constexpr int kRowBytes = kW / 8;
constexpr int kFbBytes = kRowBytes * kH;

uint8_t fb[kFbBytes];

uint32_t last_render_us = 0;
constexpr uint32_t kFrameUs = 100000;
bool key0_prev = true;
bool key1_prev = true;
uint32_t key0_t_us = 0;
uint32_t key1_t_us = 0;
constexpr uint32_t kDebounceUs = 20000;

// Single-press latch — armed on rising edge, fired on release. KEY0 was
// previously a double-click reboot trigger; that gesture moved to the
// KEY0+KEY1 chord below because rapid forward-navigation kept tripping it.
bool key0_armed = false;

// KEY1 long-press detection (for brightness cycling)
uint32_t key1_press_us = 0;
bool key1_was_pressed = false;
constexpr uint32_t kLongPressUs = 1500000;

// KEY0 + KEY1 simultaneous hold → watchdog_reboot. 1 s hold is long enough
// to filter accidental two-button taps but short enough to feel responsive.
uint32_t chord_held_since_us = 0;
constexpr uint32_t kChordHoldUs = 1000000;

// Brightness levels (SH1107 contrast register 0x81). User cycles via KEY1 long-press.
constexpr uint8_t kBrightLevels[] = {0xFF, 0x7F, 0x3F, 0x10};
constexpr int kNumBrightLevels = sizeof(kBrightLevels) / sizeof(kBrightLevels[0]);
int bright_idx = 0;
uint8_t current_contrast = 0xFF;

// Auto-dim / auto-off after idle. Tracks last button/input activity.
// Tier 1: Active → full brightness (bright_idx).
// Tier 2: idle > dim threshold → contrast drops to kDimContrast (deep dim).
// Tier 3: idle > off threshold → SH1107 panel turned fully off (cmd 0xAE)
//         to prevent OLED burn-in on long unattended sits.
// The two thresholds are user-configurable (Config_body.screen_dim_timeout /
// screen_off_timeout, minutes; 0 = tier disabled) — issue #5. last_activity_us
// is 64-bit µs so the full 0..250 min range is representable without the ~71 min
// wrap of time_us_32().
// kDimContrast tuned by eye: 0x10 looked like only ~10% reduction on this
// panel (contrast-vs-brightness is heavily non-linear near the bottom of
// the register range). 0x02 is visibly dim while still legible up close.
uint64_t last_activity_us = 0;
uint32_t last_input_hash = 0;
constexpr uint8_t kDimContrast = 0x01;
enum OledPowerState { OLED_ACTIVE, OLED_DIM, OLED_OFF };
OledPowerState oled_power_state = OLED_ACTIVE;
bool prev_bt_connected = false;

// Screen ordering — single source of truth. Reorder by editing this block;
// oled_loop's switch and handle_buttons' KEY1 contextual checks use these
// names, so the indices can move without touching that code.
constexpr int kScreenStatus    = 0;
constexpr int kScreenSlots     = 1;
constexpr int kScreenLightbar  = 2;
constexpr int kScreenTriggers  = 3;
constexpr int kScreenGyro      = 4;
constexpr int kScreenTouchpad  = 5;
constexpr int kScreenDiag      = 6;
constexpr int kScreenCpu       = 7;
constexpr int kScreenRssi      = 8;
constexpr int kScreenVU        = 9;
constexpr int kScreenSettings  = 10;
constexpr int kNumScreens      = 11;
int current_screen = 0;

// Lightbar mode cycle: 0=LIVE, 1-4=FAV0-3, 5=BREATHING, 6=RAINBOW, 7=FADE,
// 8=HOST (passthrough — let the host/game own the LED). HOST is the default so
// the dongle doesn't hijack game player-indicator LEDs out of the box. Keep
// this numbering in sync with Config_body::lightbar_mode (src/config.h).
constexpr int kLbModeHost = 8;
constexpr int kNumLbModes = 9;

// Settings screen state
constexpr int kNumSettingsItems = 16; // 8 fields + 3 auto-haptic + 2 screen-timeout + BT mic + Reset + Wipe
constexpr int kSettingsAutoHapEnaIdx  = 8;
constexpr int kSettingsAutoHapGainIdx = 9;
constexpr int kSettingsAutoHapLpIdx   = 10;
constexpr int kSettingsScrDimIdx      = 11;
constexpr int kSettingsScrOffIdx      = 12;
constexpr int kSettingsBtMicIdx       = 13;
constexpr int kSettingsResetIdx       = 14;
constexpr int kSettingsWipeSlotsIdx   = 15;
Config_body settings_local{};
int settings_sel = 0;
bool settings_dirty = false;
bool settings_init_done = false;
uint8_t settings_last_dpad = 8;  // 8 = released
uint8_t settings_last_face = 0;
const char* settings_save_status = "";  // shown briefly after Triangle press

// Factory-reset hold-Triangle-2s state. Borrowed from zurce/DS5Dongle-OLED's
// "hold to wipe" UX pattern (https://github.com/zurce/DS5Dongle-OLED).
uint32_t settings_tri_press_us = 0;
bool settings_reset_triggered = false;
constexpr uint32_t kResetHoldUs = 2000000;

uint8_t lb_r = 0, lb_g = 0, lb_b = 0;

// Lightbar mode + favorite slots: 0 = LIVE tilt preview; 1..4 = saved slots F0..F3.
// These are seeded from flash (lightbar_load_config) at boot; the defaults here
// only apply before that runs. lb_dirty tracks an unsaved mode/favorite change
// so we persist once on leaving the Lightbar screen instead of per button press.
int lb_mode = kLbModeHost;
uint8_t lb_fav_r[4] = {255, 0,   0,   255}; // Red, Green, Blue, White defaults
uint8_t lb_fav_g[4] = {0,   255, 0,   255};
uint8_t lb_fav_b[4] = {0,   0,   255, 255};
uint8_t lb_last_face = 0;
bool lb_dirty = false;

uint32_t rumble_off_at_us = 0;
bool rumble_active = false;
constexpr uint32_t kRumbleBurstUs = 250000;

int trigger_preset = 0;
const char* const kTrigPresetNames[] = {"Off", "Feedback", "Weapon", "Vibration", "Bow", "Gallop", "Machine"};

// Rising-edge trackers for the screens whose K1=cycle action moved to a
// controller button. Trigger Test uses △ (byte 7 bit 7); Lightbar uses R1
// (byte 8 bit 1) because △ is already taken on Lightbar for "save current
// RGB to favorite slot 0".
uint8_t triggers_last_face = 0;
uint8_t lb_last_buttons = 0;
constexpr int kNumTrigPresets = 7;

void cmd(uint8_t c) {
    gpio_put(kPinDC, 0);
    gpio_put(kPinCS, 0);
    spi_write_blocking(spi1, &c, 1);
    gpio_put(kPinCS, 1);
}

void data_byte(uint8_t d) {
    gpio_put(kPinDC, 1);
    gpio_put(kPinCS, 0);
    spi_write_blocking(spi1, &d, 1);
    gpio_put(kPinCS, 1);
}

uint8_t reverse_byte(uint8_t b) {
    b = ((b & 0x55) << 1) | ((b & 0xAA) >> 1);
    b = ((b & 0x33) << 2) | ((b & 0xCC) >> 2);
    b = ((b & 0x0F) << 4) | ((b & 0xF0) >> 4);
    return b;
}

void hw_reset() {
    gpio_put(kPinRST, 1); sleep_ms(100);
    gpio_put(kPinRST, 0); sleep_ms(100);
    gpio_put(kPinRST, 1); sleep_ms(100);
}

void sh1107_set_contrast(uint8_t value) {
    if (value == current_contrast) return;
    current_contrast = value;
    cmd(0x81); cmd(value);
}

void sh1107_init() {
    cmd(0xAE);
    cmd(0x00); cmd(0x10);
    cmd(0xB0);
    cmd(0xDC); cmd(0x00);
    cmd(0x81); cmd(0x6F);
    cmd(0x21);
    cmd(0xA0);
    cmd(0xC0);
    cmd(0xA4);
    cmd(0xA6);
    cmd(0xA8); cmd(0x3F);
    cmd(0xD3); cmd(0x60);
    cmd(0xD5); cmd(0x41);
    cmd(0xD9); cmd(0x22);
    cmd(0xDB); cmd(0x35);
    cmd(0xAD); cmd(0x8A);
    sleep_ms(50);
    cmd(0xAF);
}

// Forward-declared so flush_fb can paint the per-button arrows on top of
// the rendered framebuffer just before SPI sends it to the OLED. Body
// lives near the other text-drawing helpers below.
void draw_button_chrome();

void flush_fb_raw() {
    cmd(0xB0);
    for (int j = 0; j < kH; j++) {
        const uint8_t col = kH - 1 - j;
        cmd(0x00 + (col & 0x0F));
        cmd(0x10 + (col >> 4));
        for (int i = 0; i < kRowBytes; i++) {
            data_byte(reverse_byte(fb[j * kRowBytes + i]));
        }
    }
}

void flush_fb() {
    draw_button_chrome();
    flush_fb_raw();
}

void fb_clear() { memset(fb, 0, sizeof(fb)); }

void px(int x, int y, bool on) {
    if (x < 0 || x >= kW || y < 0 || y >= kH) return;
    uint8_t *p = &fb[y * kRowBytes + (x / 8)];
    uint8_t m = 1 << (7 - (x % 8));
    if (on) *p |= m; else *p &= ~m;
}

void rect_outline(int x, int y, int w, int h) {
    for (int i = 0; i < w; i++) { px(x + i, y, true); px(x + i, y + h - 1, true); }
    for (int i = 0; i < h; i++) { px(x, y + i, true); px(x + w - 1, y + i, true); }
}

void rect_filled(int x, int y, int w, int h) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            px(x + i, y + j, true);
}

void draw_char(int x, int y, char c) {
    if (c < 0x20 || c > 0x7E) return;
    const uint8_t *g = kFont5x7[c - 0x20];
    for (int col = 0; col < kFontW; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < kFontH; row++) {
            if (bits & (1 << row)) px(x + col, y + row, true);
        }
    }
}

void draw_text(int x, int y, const char *s) {
    while (*s) {
        draw_char(x, y, *s++);
        x += 6;
    }
}

// Button-chrome strip on the left edge of every screen. KEY0 (top button)
// shows '>' at y=8; KEY1 (bottom button) shows '<' at y=49. Painted by
// flush_fb() on top of the rendered framebuffer so it never gets clobbered.
// Per-screen renderers reserve x ∈ [0..5] (5-wide glyph + 1 padding) and
// start main content at kContentX.
constexpr int kContentX = 6;
void draw_button_chrome() {
    draw_char(0, 8,  '>');
    draw_char(0, 49, '<');
}

// Pixel-art icon support. Visual approach inspired by zurce/DS5Dongle-OLED
// (https://github.com/zurce/DS5Dongle-OLED) — credit to zurce for the idea
// of decorating the OLED with small bitmaps instead of bare text/shapes.
// Bitmap layout: row-major, MSB = leftmost pixel, ceil(w/8) bytes per row.
void draw_icon(int x, int y, const uint8_t *bitmap, int w, int h) {
    const int row_bytes = (w + 7) / 8;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            const uint8_t byte = bitmap[row * row_bytes + (col / 8)];
            const uint8_t mask = (uint8_t)(1u << (7 - (col % 8)));
            if (byte & mask) px(x + col, y + row, true);
        }
    }
}

// 8x8 "link active" filled circle (drawn when DS5 is paired)
static const uint8_t kIconLinkOn[8] = {
    0b00111100,
    0b01111110,
    0b11111111,
    0b11111111,
    0b11111111,
    0b11111111,
    0b01111110,
    0b00111100,
};
// 8x8 "link inactive" hollow circle (drawn when waiting for DS5)
static const uint8_t kIconLinkOff[8] = {
    0b00111100,
    0b01000010,
    0b10000001,
    0b10000001,
    0b10000001,
    0b10000001,
    0b01000010,
    0b00111100,
};

// Battery icon — body 52x8 + small nub on the right. Inside fill scales with pct.
void draw_battery_icon(int x, int y, int pct) {
    rect_outline(x, y, 52, 8);
    rect_filled(x + 52, y + 2, 3, 4);
    int fill = (pct * 48) / 100;
    if (fill < 0) fill = 0;
    if (fill > 48) fill = 48;
    if (fill > 0) rect_filled(x + 2, y + 2, fill, 4);
}

void send_rumble(uint8_t amplitude) {
    uint8_t pkt[78] = {};
    pkt[0] = 0x31;
    pkt[1] = 0x00;
    pkt[2] = 0x10;
    pkt[3] = 0x03;
    pkt[5] = amplitude;
    pkt[6] = amplitude;
    bt_write(pkt, sizeof(pkt));
}

void rumble_burst_tick(uint32_t now) {
    if (rumble_active && (int32_t)(now - rumble_off_at_us) >= 0) {
        send_rumble(0);
        rumble_active = false;
    }
}

// Trigger effect param format follows dualsensectl's reverse-engineering.
// Modes 0x21/0x25/0x26 use bitpacked 10-zone arrays, not raw position bytes.
void send_trigger_effect(int preset) {
    uint8_t pkt[78] = {};
    pkt[0] = 0x31;
    pkt[2] = 0x10;
    pkt[3] = 0x0C; // valid_flag0: RIGHT_TRIGGER_MOTOR_ENABLE | LEFT_TRIGGER_MOTOR_ENABLE

    uint8_t mode = 0x05; // OFF
    uint8_t p[9] = {0};

    switch (preset) {
        case 0: // Off
            mode = 0x05;
            break;
        case 1: { // Feedback — all 10 zones at max strength 8
            mode = 0x21;
            const uint16_t active = 0x03FF;
            uint32_t strength = 0;
            for (int i = 0; i < 10; i++) strength |= (uint32_t)(7u << (3 * i));
            p[0] = active & 0xFF;
            p[1] = (active >> 8) & 0xFF;
            p[2] = strength & 0xFF;
            p[3] = (strength >> 8) & 0xFF;
            p[4] = (strength >> 16) & 0xFF;
            p[5] = (strength >> 24) & 0xFF;
            break;
        }
        case 2: { // Weapon — snap between positions 3 and 5, force 8
            mode = 0x25;
            const uint16_t start_stop = (1u << 3) | (1u << 5);
            p[0] = start_stop & 0xFF;
            p[1] = (start_stop >> 8) & 0xFF;
            p[2] = 7; // force = strength - 1
            break;
        }
        case 3: { // Vibration — all 10 zones at amplitude 8, frequency 30 Hz
            mode = 0x26;
            const uint16_t active = 0x03FF;
            uint32_t strength = 0;
            for (int i = 0; i < 10; i++) strength |= (uint32_t)(7u << (3 * i));
            p[0] = active & 0xFF;
            p[1] = (active >> 8) & 0xFF;
            p[2] = strength & 0xFF;
            p[3] = (strength >> 8) & 0xFF;
            p[4] = (strength >> 16) & 0xFF;
            p[5] = (strength >> 24) & 0xFF;
            p[8] = 30;
            break;
        }
        case 4: { // Bow — drawing resistance + snap at position 6
            mode = 0x22;
            const uint16_t start_stop = (1u << 2) | (1u << 6);
            const uint8_t force_pair = 7u | (7u << 3); // strength=8, snap=8
            p[0] = start_stop & 0xFF;
            p[1] = (start_stop >> 8) & 0xFF;
            p[2] = force_pair;
            break;
        }
        case 5: { // Galloping
            mode = 0x23;
            const uint16_t start_stop = (1u << 0) | (1u << 9);
            const uint8_t ratio = (5u & 0x07) | ((1u & 0x07) << 3);
            p[0] = start_stop & 0xFF;
            p[1] = (start_stop >> 8) & 0xFF;
            p[2] = ratio;
            p[3] = 5; // frequency
            break;
        }
        case 6: { // Machine gun
            mode = 0x27;
            const uint16_t start_stop = (1u << 1) | (1u << 8);
            const uint8_t force_pair = 7u | (7u << 3);
            p[0] = start_stop & 0xFF;
            p[1] = (start_stop >> 8) & 0xFF;
            p[2] = force_pair;
            p[3] = 20; // frequency
            p[4] = 0;  // period
            break;
        }
    }

    pkt[13] = mode;
    for (int i = 0; i < 9; i++) pkt[14 + i] = p[i];
    pkt[24] = mode;
    for (int i = 0; i < 9; i++) pkt[25 + i] = p[i];

    bt_write(pkt, sizeof(pkt));
}

void send_lightbar_color(uint8_t r, uint8_t g, uint8_t b);

void handle_buttons() {
    const uint32_t now = time_us_32();
    const bool k0 = gpio_get(kPinKey0);
    const bool k1 = gpio_get(kPinKey1);

    // KEY0 + KEY1 chord — both held >= kChordHoldUs triggers watchdog_reboot.
    // Pre-empts the per-key handlers so a chord cancels any armed single
    // press (whichever key gets released first won't also navigate).
    const bool chord = !k0 && !k1;
    if (chord) {
        if (chord_held_since_us == 0) chord_held_since_us = now;
        key0_armed = false;
        key1_was_pressed = false;
        if ((now - chord_held_since_us) >= kChordHoldUs) {
            watchdog_reboot(0, 0, 0);
        }
    } else {
        chord_held_since_us = 0;
    }

    // KEY0: arm on debounced rising edge, fire "next screen" on release.
    // Releasing without a chord during the hold = pure forward-nav.
    if (!k0 && key0_prev && (now - key0_t_us) > kDebounceUs) {
        key0_t_us = now;
        key0_armed = true;
        last_activity_us = time_us_64();
    }
    if (k0 && !key0_prev && key0_armed) {
        key0_armed = false;
        current_screen = (current_screen + 1) % kNumScreens;
        last_render_us = 0;
        last_activity_us = time_us_64();
    }

    // KEY1: arm on press, fire on release. Short press = back; long press
    // = brightness cycle (unchanged). Trigger-preset / lightbar-mode cycle
    // moved to the DualSense △ button — see triggers_handle_input() and
    // lightbar_handle_input(). The chord above clears key1_was_pressed so
    // a released-after-chord K1 doesn't navigate back.
    if (!k1 && key1_prev && (now - key1_t_us) > kDebounceUs) {
        key1_t_us = now;
        key1_press_us = now;
        key1_was_pressed = true;
        last_activity_us = time_us_64();
    }
    if (k1 && !key1_prev && key1_was_pressed) {
        key1_was_pressed = false;
        const uint32_t held = now - key1_press_us;
        last_activity_us = time_us_64();
        if (held > kLongPressUs) {
            bright_idx = (bright_idx + 1) % kNumBrightLevels;
        } else {
            current_screen = (current_screen - 1 + kNumScreens) % kNumScreens;
            last_render_us = 0;
        }
    }

    key0_prev = k0;
    key1_prev = k1;
}

// --- Charge ETA tracker --------------------------------------------------
// The DS5 only reports battery in 10% steps (interrupt_in_data[52] low
// nibble, 0..10; high nibble is power-state, 1 == charging). We can't read a
// finer percentage over BT, so a smooth countdown is impossible. Instead we
// time how long each 10% step takes while charging and extrapolate the
// remaining steps. Sampled once per frame from oled_loop (continuously, so
// the estimate stays current even while the panel is dimmed/off and even when
// the user is on another screen); render_screen reads g_charge_eta.
//
// Taper correction: Li-ion CC/CV charging slows sharply near the top, so a
// flat "time per step × steps left" runs optimistic in the last ~20%. Each
// measured step is normalised to a bulk-equivalent duration (divide out the
// step's taper weight); the remaining steps are then re-weighted. This makes
// the estimate consistent whether the user plugged in near-empty or near-full.
struct ChargeEta {
    bool charging;    // pstate == 1 (so the token shows only while charging)
    bool valid;       // minutes is meaningful (provisional or measured)
    bool provisional; // true until a full step is timed — using the default rate
    int  minutes;     // estimated minutes to 100%
};
ChargeEta g_charge_eta{};

// Default bulk-step duration used for a provisional estimate before any real
// step has been timed, so the token shows "~Nm?" immediately on plug-in instead
// of sitting on "~--m" for ~15-20 min. Tuned to an observed ~15 min per 10% on
// this dongle's charge current; it self-corrects to the measured rate (and drops
// the "?") as soon as the first clean step completes.
constexpr float kDefaultStepUs = 15.0f * 60.0f * 1000000.0f;

// Relative time the step *ending* at `to_level` (10% units, 1..10) takes vs a
// bulk step. Tuned to the Li-ion CV taper: ~80% onward stretches out.
static float charge_step_weight(int to_level) {
    if (to_level >= 10) return 2.2f;  // 90→100% (constant-voltage tail)
    if (to_level == 9)  return 1.5f;  // 80→90%  (taper begins)
    return 1.0f;                      // bulk constant-current region
}

void sample_charge_eta() {
    constexpr int kRing = 3;                 // average the last few steps
    static float    ring[kRing] = {0};       // bulk-equivalent step durations (us)
    static int      ring_count = 0;
    static int      ring_head = 0;
    static int      cur_step = -1;           // last observed 10% step
    static uint64_t step_start_us = 0;
    static bool     was_charging = false;
    static bool     first_step_pending = false;  // discard the partial step at plug-in

    const uint8_t pwr   = interrupt_in_data[52];
    int           step  = pwr & 0x0F;
    if (step > 10) step = 10;
    const uint8_t pstate = pwr >> 4;
    const bool charging = bt_is_connected() && (pstate == 1);

    if (!charging) {
        g_charge_eta = ChargeEta{};          // clears charging/valid/minutes
        ring_count = ring_head = 0;
        cur_step = -1;
        was_charging = false;
        return;
    }

    const uint64_t now = time_us_64();
    if (!was_charging) {
        // Just plugged in: start timing from here. The step in progress is
        // partial, so its duration gets discarded when it completes.
        cur_step = step;
        step_start_us = now;
        ring_count = ring_head = 0;
        first_step_pending = true;
        was_charging = true;
    } else if (step == cur_step + 1) {
        // One clean step completed. Skip the first (partial) one; otherwise
        // record its bulk-equivalent duration.
        const float dur = (float)(now - step_start_us);
        if (first_step_pending) {
            first_step_pending = false;
        } else {
            ring[ring_head] = dur / charge_step_weight(step);
            ring_head = (ring_head + 1) % kRing;
            if (ring_count < kRing) ring_count++;
        }
        cur_step = step;
        step_start_us = now;
    } else if (step != cur_step) {
        // Multi-step jump (e.g. woke from sleep across several steps) or a
        // small dip under heavy use — can't attribute timing cleanly, so just
        // resync without polluting the ring.
        cur_step = step;
        step_start_us = now;
        first_step_pending = false;
    }

    g_charge_eta.charging = true;
    if (cur_step < 10) {
        // Use the measured rate once we have a timed step; until then fall back
        // to the default rate and flag the estimate provisional (renders "?").
        const bool measured = (ring_count > 0);
        float bulk;
        if (measured) {
            bulk = 0.0f;
            for (int i = 0; i < ring_count; i++) bulk += ring[i];
            bulk /= (float)ring_count;
        } else {
            bulk = kDefaultStepUs;
        }
        float rem_us = 0.0f;
        for (int L = cur_step + 1; L <= 10; L++) rem_us += bulk * charge_step_weight(L);
        int mins = (int)(rem_us / 60000000.0f + 0.5f);
        if (mins < 0)   mins = 0;
        if (mins > 999) mins = 999;
        g_charge_eta.valid = true;
        g_charge_eta.provisional = !measured;
        g_charge_eta.minutes = mins;
    } else {
        // cur_step == 10 → essentially full; nothing meaningful to count down.
        g_charge_eta.valid = true;
        g_charge_eta.provisional = false;
        g_charge_eta.minutes = 0;
    }
}

__attribute__((noinline)) void render_screen() {
    fb_clear();

    const bool connected = bt_is_connected();

    // FIRMWARE_VERSION is set via CMake from -DVERSION=... on the build
    // command line (release.yml passes the tag name). Local builds get
    // "dev" so a non-tagged build is visible at a glance.
    draw_text(kContentX, 0, "DS5 Bridge " FIRMWARE_VERSION);
    draw_icon(120, 0, connected ? kIconLinkOn : kIconLinkOff, 8, 8);

    if (connected) {
        uint8_t a[6];
        bt_get_addr(a);
        char buf[24];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                 a[0], a[1], a[2], a[3], a[4], a[5]);
        draw_text(kContentX, 9, buf);

        const uint8_t pwr = interrupt_in_data[52];
        int pct = (pwr & 0x0F) * 10;
        if (pct > 100) pct = 100;
        const uint8_t pstate = pwr >> 4;
        char marker = ' ';
        if (pstate == 1) marker = '+';      // Charging
        else if (pstate == 2) marker = '*'; // Complete
        else if (pstate >= 0xA) marker = '!'; // Error
        char bbuf[16];
        snprintf(bbuf, sizeof(bbuf), "%3d%%%c", pct, marker);
        draw_text(kContentX, 18, bbuf);
        draw_battery_icon(36, 18, pct);

        // Charge ETA, right of the battery icon (icon ends at x≈90). Shown only
        // while charging: "~43m?" is the provisional default-rate estimate shown
        // immediately on plug-in; the "?" drops to "~43m" once a real 10% step
        // has been timed and the measured rate takes over. See sample_charge_eta().
        if (g_charge_eta.charging) {
            char ebuf[8];
            if (g_charge_eta.valid)
                snprintf(ebuf, sizeof(ebuf), "~%dm%s", g_charge_eta.minutes,
                         g_charge_eta.provisional ? "?" : "");
            else
                snprintf(ebuf, sizeof(ebuf), "~--m");
            draw_text(94, 18, ebuf);
        }

        // Left-half visuals are shifted right by kContentX so the < button
        // chrome at (x=0, y=49) doesn't paint over the live stick dot.
        rect_outline(kContentX, 30, 32, 32);
        int lx = (kContentX + 2) + (interrupt_in_data[0] * 27) / 255;
        int ly = 32 + (interrupt_in_data[1] * 27) / 255;
        rect_filled(lx - 1, ly - 1, 3, 3);

        rect_outline(96, 30, 32, 32);
        int rx = 98 + (interrupt_in_data[2] * 27) / 255;
        int ry = 32 + (interrupt_in_data[3] * 27) / 255;
        rect_filled(rx - 1, ry - 1, 3, 3);

        // L2/R2 analog trigger bars (vertical, fill from bottom). L2 sits
        // just right of the shifted left stick box.
        rect_outline(kContentX + 32, 33, 4, 29);
        const int l2_fill = (interrupt_in_data[4] * 27) / 255;
        if (l2_fill > 0) rect_filled(kContentX + 33, 61 - l2_fill, 2, l2_fill);
        rect_outline(92, 33, 4, 29);
        const int r2_fill = (interrupt_in_data[5] * 27) / 255;
        if (r2_fill > 0) rect_filled(93, 61 - r2_fill, 2, r2_fill);

        const uint8_t b7 = interrupt_in_data[7];
        const uint8_t b8 = interrupt_in_data[8];

        // D-pad indicator (4 directions; lit for primary + diagonals).
        // Centered between the left stick column and the face-button cluster.
        const int dp = b7 & 0x0F;
        const bool dp_n = (dp == 7 || dp == 0 || dp == 1);
        const bool dp_e = (dp == 1 || dp == 2 || dp == 3);
        const bool dp_s = (dp == 3 || dp == 4 || dp == 5);
        const bool dp_w = (dp == 5 || dp == 6 || dp == 7);
        const int dcx = 52, dcy = 46;
        auto dot = [&](int dx, int dy, bool on) {
            if (on) rect_filled(dcx + dx - 2, dcy + dy - 2, 5, 5);
            else    rect_outline(dcx + dx - 2, dcy + dy - 2, 5, 5);
        };
        dot(0,  -7, dp_n);
        dot(7,   0, dp_e);
        dot(0,   7, dp_s);
        dot(-7,  0, dp_w);

        const int fcx = 64, fcy = 46;
        auto sq = [&](int dx, int dy, bool on) {
            if (on) rect_filled(fcx + dx - 2, fcy + dy - 2, 5, 5);
            else    rect_outline(fcx + dx - 2, fcy + dy - 2, 5, 5);
        };
        // shift face buttons right so they don't collide with d-pad
        const int fcx_off = 18;
        sq(fcx_off + 0,  -8, b7 & 0x80); // Triangle
        sq(fcx_off + 8,   0, b7 & 0x40); // Circle
        sq(fcx_off + 0,   8, b7 & 0x20); // Cross
        sq(fcx_off - 8,   0, b7 & 0x10); // Square

        // L1 bar shifted to sit between the L2 trigger column and the d-pad.
        if (b8 & 0x01) rect_filled(42, 30, 8, 3);  else rect_outline(42, 30, 8, 3);  // L1
        if (b8 & 0x02) rect_filled(80, 30, 12, 3); else rect_outline(80, 30, 12, 3); // R1
    } else {
        draw_text(kContentX, 14, "Pair your DualSense:");
        draw_text(kContentX, 26, "1. Hold Create + PS");
        draw_text(kContentX, 36, "2. Wait for light bar");
        draw_text(kContentX, 46, "   to flash blue");
    }

    flush_fb();
}

__attribute__((noinline)) void render_screen_rssi() {
    fb_clear();
    draw_text(kContentX, 0, "BT Signal");
    if (bt_is_connected()) {
        int8_t rssi = 0;
        bt_get_signal_strength(&rssi);
        char buf[24];
        snprintf(buf, sizeof(buf), "RSSI: %d dBm", (int)rssi);
        draw_text(kContentX, 12, buf);

        // Map RSSI range -90..-40 dBm to 0..100% bar
        int pct = ((int)rssi + 90) * 100 / 50;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        snprintf(buf, sizeof(buf), "Quality: %d%%", pct);
        draw_text(kContentX, 22, buf);
        rect_outline(kContentX, 34, 122, 10);
        int fill = (pct * 118) / 100;
        if (fill > 0) rect_filled(kContentX + 2, 36, fill, 6);

        const char *label = "Poor";
        if (rssi > -55) label = "Excellent";
        else if (rssi > -65) label = "Good";
        else if (rssi > -75) label = "Fair";
        snprintf(buf, sizeof(buf), "Link: %s", label);
        draw_text(kContentX, 48, buf);
    } else {
        draw_text(kContentX, 30, "(no controller)");
    }
    flush_fb();
}

// Diagnostics screen state. Read-only viewport that scrolls with controller
// D-pad up/down. No cursor — there's nothing to select.
int   diag_scroll = 0;
uint8_t diag_last_dpad = 8; // edge-trigger N/E/S/W like settings_handle_input

// Per-second rates sampled once per render, shared across format_diag_row's
// rate-based rows so they stay in sync.
struct DiagRates {
    uint32_t usb_rate;
    uint32_t bt_rate;
    uint32_t mic_rate;
    uint32_t bt31_rate;
};
DiagRates g_diag_rates{};

void sample_diag_rates() {
    static uint32_t prev_us_frames = 0, prev_bt_packets = 0, prev_mic_frames = 0, prev_bt31 = 0;
    static uint32_t prev_sample_us = 0;
    const uint32_t now_us = time_us_32();
    const uint32_t cur_us_frames  = audio_usb_frames();
    const uint32_t cur_bt_packets = audio_bt_packets();
    const uint32_t cur_mic_frames = audio_mic_frames();
    const uint32_t cur_bt31       = bt_31_packet_count();
    if (prev_sample_us != 0 && now_us > prev_sample_us) {
        const uint32_t dt_us = now_us - prev_sample_us;
        if (dt_us > 0) {
            g_diag_rates.usb_rate  = (uint32_t)(((uint64_t)(cur_us_frames  - prev_us_frames)  * 1000000u) / dt_us);
            g_diag_rates.bt_rate   = (uint32_t)(((uint64_t)(cur_bt_packets - prev_bt_packets) * 1000000u) / dt_us);
            g_diag_rates.mic_rate  = (uint32_t)(((uint64_t)(cur_mic_frames - prev_mic_frames) * 1000000u) / dt_us);
            g_diag_rates.bt31_rate = (uint32_t)(((uint64_t)(cur_bt31       - prev_bt31)       * 1000000u) / dt_us);
        }
    }
    prev_us_frames  = cur_us_frames;
    prev_bt_packets = cur_bt_packets;
    prev_mic_frames = cur_mic_frames;
    prev_bt31       = cur_bt31;
    prev_sample_us  = now_us;
}

// Row list ordered by relevance: always-useful at top, parked-mic-investigation
// data at bottom. To add a row, bump kNumDiagRows and add a case.
constexpr int kNumDiagRows = 11;
__attribute__((noinline))
void format_diag_row(int idx, char* line, size_t n) {
    switch (idx) {
        case 0: {
            const uint32_t s = time_us_32() / 1000000u;
            snprintf(line, n, "Up:%luh %02lum %02lus",
                     (unsigned long)(s / 3600u),
                     (unsigned long)((s / 60u) % 60u),
                     (unsigned long)(s % 60u));
            break;
        }
        case 1:
            snprintf(line, n, "BT: %s", bt_is_connected() ? "connected" : "waiting");
            break;
        case 2:
            snprintf(line, n, "host02: %lu", (unsigned long)host_out02_total());
            break;
        case 3:
            snprintf(line, n, "trig %lu / tx %lu",
                     (unsigned long)host_out02_trig_allow(),
                     (unsigned long)host_out02_to_bt());
            break;
        case 4:
            // Trigger reports folded into the 0x36 audio path (speaker active),
            // not sent as 0x31. trig == tx-trig-share + this → no drops (#6).
            snprintf(line, n, "trig fold: %lu", (unsigned long)host_out02_trig_folded());
            break;
        case 5:
            snprintf(line, n, "BT31 in: %lu/s", (unsigned long)g_diag_rates.bt31_rate);
            break;
        case 6:
            snprintf(line, n, "USB aud: %lu/s", (unsigned long)g_diag_rates.usb_rate);
            break;
        case 7:
            snprintf(line, n, "BT32 out: %lu/s", (unsigned long)g_diag_rates.bt_rate);
            break;
        case 8:
            snprintf(line, n, "Mic in: %lu/s", (unsigned long)g_diag_rates.mic_rate);
            break;
        case 9:
            snprintf(line, n, "Mic dec=%ld w=%u",
                     (long)audio_mic_last_decoded(),
                     (unsigned)audio_mic_last_wrote());
            break;
        case 10: {
            uint8_t pfx[6]; bt_31_mic_prefix(pfx);
            snprintf(line, n, "%02X %02X %02X %02X %02X %02X",
                     pfx[0], pfx[1], pfx[2], pfx[3], pfx[4], pfx[5]);
            break;
        }
        default:
            line[0] = '\0';
            break;
    }
}

void diag_handle_input(int visible) {
    if (!bt_is_connected()) return;
    const uint8_t dpad = (uint8_t)(interrupt_in_data[7] & 0x0F);
    if (dpad != diag_last_dpad && dpad != 8) {
        if      (dpad == 0) diag_scroll--; // up
        else if (dpad == 4) diag_scroll++; // down
    }
    diag_last_dpad = dpad;
    const int max_top = (kNumDiagRows > visible) ? (kNumDiagRows - visible) : 0;
    if (diag_scroll < 0) diag_scroll = 0;
    if (diag_scroll > max_top) diag_scroll = max_top;
}

__attribute__((noinline)) void render_screen_diag() {
    fb_clear();
    draw_text(kContentX, 0, "Diagnostics");

    sample_diag_rates();
    constexpr int kVisible = 5;
    diag_handle_input(kVisible);

    char line[28];
    for (int i = 0; i < kVisible && diag_scroll + i < kNumDiagRows; i++) {
        format_diag_row(diag_scroll + i, line, sizeof(line));
        draw_text(kContentX, 9 + i * 9, line);
    }

    // Scroll indicators along the right edge, adjacent to the visible content
    // rather than in a footer — keeps the bottom row available for content.
    if (diag_scroll > 0)                       draw_text(120, 9,  "^");
    if (diag_scroll + kVisible < kNumDiagRows) draw_text(120, 45, "v");

    flush_fb();
}

__attribute__((noinline)) void render_screen_cpu(bool entered) {
    fb_clear();
    draw_text(kContentX, 0, "CPU / Clock");

    char buf[24];

    // Configured system clock — compile-time SYS_CLOCK_KHZ, set in main()
    // via set_sys_clock_khz(). This is the *target*.
    const uint32_t set_khz = (uint32_t)SYS_CLOCK_KHZ;
    snprintf(buf, sizeof(buf), "Set : %lu MHz", (unsigned long)(set_khz / 1000u));
    draw_text(kContentX, 12, buf);

    // Actually running clk_sys, measured by the on-chip frequency counter
    // against the crystal reference (not just what we asked for). The counter
    // busy-waits a few ms per call, so measure ONCE on screen entry and cache
    // it — clk_sys is fixed at boot and never changes, so the temperature
    // (which legitimately drifts) is the only thing worth refreshing per
    // frame. cached_real_khz==0 also forces a (re)measure as a safety net.
    static uint32_t cached_real_khz = 0;
    if (entered || cached_real_khz == 0) {
        cached_real_khz = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS);
    }
    const uint32_t real_khz = cached_real_khz;
    snprintf(buf, sizeof(buf), "Real: %lu.%01lu MHz",
             (unsigned long)(real_khz / 1000u),
             (unsigned long)((real_khz % 1000u) / 100u));
    draw_text(kContentX, 22, buf);

    // Core voltage actually programmed into the regulator, read back (not the
    // compile-time constant). Codes 0..15 are linear 0.05 V steps from 0.55 V.
    const int vcode = (int)vreg_get_voltage();
    if (vcode >= 0 && vcode <= 0b01111) {
        const unsigned mv = 550u + 50u * (unsigned)vcode;
        snprintf(buf, sizeof(buf), "Vcore: %u.%02u V", mv / 1000u, (mv % 1000u) / 10u);
    } else {
        snprintf(buf, sizeof(buf), "Vcore: code %d", vcode);
    }
    draw_text(kContentX, 32, buf);

    // RP2350 on-die temperature sensor. Smoothed + averaged in cmd.cpp
    // (single source of truth shared with the 0xfc web telemetry) so the
    // reading converges to the true die temp instead of chasing ADC noise.
    const uint16_t raw = cpu_temp_raw_smoothed();
    const float volts = (float)raw * 3.3f / 4096.0f;
    const float temp_c = 27.0f - (volts - 0.706f) / 0.001721f;
    const int t10 = (int)(temp_c * 10.0f + (temp_c >= 0 ? 0.5f : -0.5f));
    snprintf(buf, sizeof(buf), "Temp : %d.%d C", t10 / 10,
             (t10 < 0 ? -t10 : t10) % 10);
    draw_text(kContentX, 42, buf);

    flush_fb();
}

// △ rising edge on the Trigger Test screen cycles trigger_preset and
// re-applies the new effect to the paired controller. KEY1 used to do
// this; moving it to the controller frees K0/K1 for navigation only.
void triggers_handle_input() {
    if (!bt_is_connected()) { triggers_last_face = 0; return; }
    const uint8_t face = interrupt_in_data[7] & 0xF0;
    const bool tri_now  = (face & 0x80) != 0;
    const bool tri_prev = (triggers_last_face & 0x80) != 0;
    if (tri_now && !tri_prev) {
        trigger_preset = (trigger_preset + 1) % kNumTrigPresets;
        send_trigger_effect(trigger_preset);
    }
    triggers_last_face = face;
}

__attribute__((noinline)) void render_screen_triggers() {
    triggers_handle_input();
    fb_clear();
    draw_text(kContentX, 0, "Trigger Test");

    char buf[24];
    snprintf(buf, sizeof(buf), "Mode: %s", kTrigPresetNames[trigger_preset]);
    draw_text(kContentX, 12, buf);

    if (bt_is_connected()) {
        const uint8_t l2 = interrupt_in_data[4];
        const uint8_t r2 = interrupt_in_data[5];
        snprintf(buf, sizeof(buf), "L2:%3d  R2:%3d", l2, r2);
        draw_text(kContentX, 24, buf);

        rect_outline(kContentX, 35, 56, 9);
        int lfill = (l2 * 52) / 255;
        if (lfill > 0) rect_filled(kContentX + 2, 37, lfill, 5);
        rect_outline(72, 35, 56, 9);
        int rfill = (r2 * 52) / 255;
        if (rfill > 0) rect_filled(74, 37, rfill, 5);
    } else {
        draw_text(kContentX, 24, "(no controller)");
    }

    draw_text(kContentX, 56, "Tri=cycle");
    flush_fb();
}

__attribute__((noinline)) void render_screen_gyro() {
    fb_clear();
    draw_text(kContentX, 0, "Gyro Tilt");
    if (bt_is_connected()) {
        int16_t ax, ay, az;
        memcpy(&ax, &interrupt_in_data[21], 2);
        memcpy(&ay, &interrupt_in_data[23], 2);
        memcpy(&az, &interrupt_in_data[25], 2);
        char buf[16];
        snprintf(buf, sizeof(buf), "X%+5d", ax); draw_text(kContentX, 10, buf);
        snprintf(buf, sizeof(buf), "Y%+5d", ay); draw_text(50, 10, buf);
        snprintf(buf, sizeof(buf), "Z%+5d", az); draw_text(94, 10, buf);

        const int bx = 44, by = 22, bw = 40, bh = 40;
        rect_outline(bx, by, bw, bh);
        for (int x = bx + 1; x < bx + bw - 1; x++) px(x, by + bh / 2, true);
        for (int y = by + 1; y < by + bh - 1; y++) px(bx + bw / 2, y, true);
        int dx = ((int)ax * (bw / 2 - 3)) / 8192;
        int dy = ((int)ay * (bh / 2 - 3)) / 8192;
        int cx = bx + bw / 2 + dx;
        int cy = by + bh / 2 + dy;
        if (cx < bx + 2) cx = bx + 2;
        if (cx > bx + bw - 3) cx = bx + bw - 3;
        if (cy < by + 2) cy = by + 2;
        if (cy > by + bh - 3) cy = by + bh - 3;
        rect_filled(cx - 1, cy - 1, 3, 3);
    } else {
        draw_text(kContentX, 30, "(no controller)");
    }
    flush_fb();
}

__attribute__((noinline)) void render_screen_touchpad() {
    fb_clear();
    draw_text(kContentX, 0, "Touchpad");
    if (bt_is_connected()) {
        rect_outline(kContentX + 2, 12, 116, 30);
        int active = 0;
        for (int finger = 0; finger < 2; finger++) {
            const int off = 32 + finger * 4;
            const uint32_t f = (uint32_t)interrupt_in_data[off] |
                               ((uint32_t)interrupt_in_data[off + 1] << 8) |
                               ((uint32_t)interrupt_in_data[off + 2] << 16) |
                               ((uint32_t)interrupt_in_data[off + 3] << 24);
            const bool not_touching = (f >> 7) & 1u;
            if (not_touching) continue;
            const uint16_t fx = (f >> 8) & 0xFFFu;
            const uint16_t fy = (f >> 20) & 0xFFFu;
            int sx = (kContentX + 3) + ((int)fx * 110) / 1919;
            int sy = 13 + ((int)fy * 26) / 1079;
            if (sx < kContentX + 3)   sx = kContentX + 3;
            if (sx > 122) sx = 122;
            if (sy < 13)  sy = 13;
            if (sy > 40)  sy = 40;
            rect_filled(sx - 1, sy - 1, 3, 3);
            active++;
        }
        char buf[20];
        snprintf(buf, sizeof(buf), "Fingers: %d", active);
        draw_text(kContentX, 46, buf);
    } else {
        draw_text(kContentX, 30, "(no controller)");
    }
    flush_fb();
}

void send_lightbar_color(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t pkt[78] = {};
    pkt[0] = 0x31;
    pkt[2] = 0x10;
    pkt[4] = 0x04; // valid_flag1: LIGHTBAR_CONTROL_ENABLE (bit 2)
    pkt[47] = r;   // lightbar_red
    pkt[48] = g;   // lightbar_green
    pkt[49] = b;   // lightbar_blue
    bt_write(pkt, sizeof(pkt));
}

// Tiny 32-step sine LUT (no <cmath>). angle 0..255 → amplitude -127..127.
static const int8_t kSine32[32] = {
    0,   24,   49,   70,   90,  106,  117,  125,  127,  125,  117,  106,   90,   70,   49,   24,
    0,  -24,  -49,  -70,  -90, -106, -117, -125, -127, -125, -117, -106,  -90,  -70,  -49,  -24,
};
int sin_lut(uint8_t a) { return kSine32[(a >> 3) & 0x1F]; }

void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (h >= 360) h %= 360;
    const uint8_t region = (uint8_t)(h / 60);
    const uint16_t remainder = (uint16_t)((h - region * 60u) * 256u / 60u);
    const uint8_t p = (uint8_t)(((uint16_t)v * (255u - s)) >> 8);
    const uint8_t q = (uint8_t)(((uint16_t)v * (255u - (((uint16_t)s * remainder) >> 8))) >> 8);
    const uint8_t t = (uint8_t)(((uint16_t)v * (255u - (((uint16_t)s * (255u - remainder)) >> 8))) >> 8);
    switch (region) {
        case 0:  *r = v; *g = t; *b = p; break;
        case 1:  *r = q; *g = v; *b = p; break;
        case 2:  *r = p; *g = v; *b = t; break;
        case 3:  *r = p; *g = q; *b = v; break;
        case 4:  *r = t; *g = p; *b = v; break;
        default: *r = v; *g = p; *b = q; break;
    }
}

const char* lb_mode_tag(int mode) {
    switch (mode) {
        case 0: return "[LIVE]";
        case 1: return "[FAV0]";
        case 2: return "[FAV1]";
        case 3: return "[FAV2]";
        case 4: return "[FAV3]";
        case 5: return "[BREA]";
        case 6: return "[RAIN]";
        case 7: return "[FADE]";
        case 8: return "[HOST]";
        default: return "[????]";
    }
}

// R1 rising edge on Lightbar cycles lb_mode. Used to be KEY1; that moved
// to back-nav. Triangle on this screen stays as "save current RGB to
// favorite slot 0" (the existing favorite-save UX), so R1 is the next
// free button that doesn't break a mental model.
void lightbar_handle_input() {
    if (!bt_is_connected()) { lb_last_buttons = 0; return; }
    const uint8_t btns   = interrupt_in_data[8];
    const bool r1_now    = (btns & 0x02) != 0;
    const bool r1_prev   = (lb_last_buttons & 0x02) != 0;
    if (r1_now && !r1_prev) {
        lb_mode = (lb_mode + 1) % kNumLbModes;
        lb_dirty = true; // persisted on leaving the Lightbar screen
    }
    lb_last_buttons = btns;
}

__attribute__((noinline)) void render_screen_lightbar() {
    lightbar_handle_input();
    fb_clear();
    draw_text(kContentX, 0, "Lightbar");
    draw_text(86, 0, lb_mode_tag(lb_mode));

    if (bt_is_connected()) {
        // lb_r/lb_g/lb_b are computed every frame by lightbar_service() (which
        // runs ahead of this render in oled_loop), so here we only display them.
        char buf[16];
        snprintf(buf, sizeof(buf), "R:%3u", lb_r); draw_text(kContentX, 12, buf);
        snprintf(buf, sizeof(buf), "G:%3u", lb_g); draw_text(48, 12, buf);
        snprintf(buf, sizeof(buf), "B:%3u", lb_b); draw_text(90, 12, buf);

        const int by = 22, bh = 8;
        rect_outline(kContentX,  by, 38, bh); int rf = (lb_r * 34) / 255; if (rf > 0) rect_filled(kContentX + 2,  by + 2, rf, bh - 4);
        rect_outline(48, by, 38, bh); int gf = (lb_g * 34) / 255; if (gf > 0) rect_filled(50, by + 2, gf, bh - 4);
        rect_outline(90, by, 38, bh); int bf = (lb_b * 34) / 255; if (bf > 0) rect_filled(92, by + 2, bf, bh - 4);

        // Face button rising-edge -> save current color to slot 0..3
        const uint8_t face = interrupt_in_data[7] & 0xF0;
        const uint8_t pressed = face & ~lb_last_face;
        lb_last_face = face;
        int save_slot = -1;
        if      (pressed & 0x80) save_slot = 0; // Triangle
        else if (pressed & 0x40) save_slot = 1; // Circle
        else if (pressed & 0x20) save_slot = 2; // Cross
        else if (pressed & 0x10) save_slot = 3; // Square
        if (save_slot >= 0) {
            lb_fav_r[save_slot] = lb_r;
            lb_fav_g[save_slot] = lb_g;
            lb_fav_b[save_slot] = lb_b;
            lb_dirty = true; // persisted on leaving the Lightbar screen
        }

        draw_text(kContentX, 38, "Sv:T=0 C=1 X=2 S=3");
        const char* hint =
            (lb_mode == 0)           ? "Tilt = R/G/B"   :
            (lb_mode == 5)           ? "Breathing FAV0" :
            (lb_mode == 6)           ? "Rainbow sweep"  :
            (lb_mode == 7)           ? "Fade thru FAVs" :
            (lb_mode == kLbModeHost) ? "Host controls"  :
                                       "Locked to fav";
        draw_text(kContentX, 48, hint);
        // No send here: lightbar_service() owns pushing the color to the
        // controller every frame, on this screen and every other.
    } else {
        draw_text(kContentX, 30, "(no controller)");
    }
    draw_text(kContentX, 56, "R1=mode");
    flush_fb();
}

// Compute lb_r/lb_g/lb_b for an OLED lightbar mode (0..7). HOST (8) is handled
// by the caller (no firmware color). noinline keeps the float/HSV literals out
// of lightbar_service's / oled_loop's literal pool (same Thumb reach constraint
// the render_screen_* functions hit).
__attribute__((noinline))
void lightbar_compute_mode(int mode, uint32_t now_ms) {
    if (mode == 0) {
        // LIVE: tilt -> RGB
        int16_t ax, ay, az;
        memcpy(&ax, &interrupt_in_data[21], 2);
        memcpy(&ay, &interrupt_in_data[23], 2);
        memcpy(&az, &interrupt_in_data[25], 2);
        const int rr = ((int)ax + 8192) * 255 / 16384;
        const int gg = ((int)ay + 8192) * 255 / 16384;
        const int bb = ((int)az + 8192) * 255 / 16384;
        lb_r = (uint8_t)(rr < 0 ? 0 : rr > 255 ? 255 : rr);
        lb_g = (uint8_t)(gg < 0 ? 0 : gg > 255 ? 255 : gg);
        lb_b = (uint8_t)(bb < 0 ? 0 : bb > 255 ? 255 : bb);
    } else if (mode <= 4) {
        // FAV slot: fixed color
        const int slot = mode - 1;
        lb_r = lb_fav_r[slot];
        lb_g = lb_fav_g[slot];
        lb_b = lb_fav_b[slot];
    } else if (mode == 5) {
        // BREATHING: modulate FAV0 brightness with a sine wave (~3 s cycle)
        const uint8_t phase = (uint8_t)(now_ms / 12);
        const int s = sin_lut(phase); // -127..127
        const uint16_t scale = (uint16_t)(32 + (s + 127) / 2); // 32..191
        lb_r = (uint8_t)((lb_fav_r[0] * scale) / 255);
        lb_g = (uint8_t)((lb_fav_g[0] * scale) / 255);
        lb_b = (uint8_t)((lb_fav_b[0] * scale) / 255);
    } else if (mode == 6) {
        // RAINBOW: hue sweep over ~6 s
        const uint16_t hue = (uint16_t)((now_ms / 17) % 360);
        hsv_to_rgb(hue, 255, 255, &lb_r, &lb_g, &lb_b);
    } else {
        // FADE between FAV slots, 2 s per slot
        const uint32_t kSlotMs = 2000;
        const uint32_t total = now_ms % (4 * kSlotMs);
        const int slot = (int)(total / kSlotMs);
        const int next = (slot + 1) & 3;
        const uint16_t blend = (uint16_t)(((total - slot * kSlotMs) * 256u) / kSlotMs);
        lb_r = (uint8_t)((lb_fav_r[slot] * (255 - blend) + lb_fav_r[next] * blend) / 255);
        lb_g = (uint8_t)((lb_fav_g[slot] * (255 - blend) + lb_fav_g[next] * blend) / 255);
        lb_b = (uint8_t)((lb_fav_b[slot] * (255 - blend) + lb_fav_b[next] * blend) / 255);
    }
}

// The single owner of the controller LED. Runs every frame (~10 Hz) from
// oled_loop, on every screen, so a chosen mode "sticks" everywhere instead of
// only while the Lightbar screen renders. Priority:
//   1. Charging  -> amber-orange breathing pulse (status indicator).
//   2. lb_mode != HOST -> the selected OLED mode/color.
//   3. HOST (or disconnected) -> hand the LED back to the host/game.
// When the firmware owns the LED it (a) writes state[] so the color rides every
// host/audio packet and (b) actively pushes it via send_lightbar_color so it
// updates even when the host is idle and animations keep moving. g_lightbar_
// override gates state_update() so host AllowLedColor writes can't stomp us.
__attribute__((noinline))
void lightbar_service() {
    if (!bt_is_connected()) { g_lightbar_override = false; return; }
    const uint32_t now_ms = time_us_32() / 1000;

    if (g_charge_eta.charging) {
        // ~4.6 s breathing cycle (256 phase steps × 18 ms). Base amber
        // (255,100,0) sine-enveloped from dim (24) to bright (240).
        const uint8_t  phase = (uint8_t)(now_ms / 18);
        const int      s     = sin_lut(phase);                          // -127..127
        const uint16_t scale = (uint16_t)(24 + ((s + 127) * 216) / 254); // 24..240
        lb_r = (uint8_t)((255u * scale) / 255u);
        lb_g = (uint8_t)((100u * scale) / 255u);
        lb_b = 0;
    } else if (lb_mode == kLbModeHost) {
        // Reflect the host's current LED on the OLED bars, then stand down.
        state_get_led(&lb_r, &lb_g, &lb_b);
        g_lightbar_override = false;
        return;
    } else {
        lightbar_compute_mode(lb_mode, now_ms);
    }

    g_lightbar_override = true;
    state_set_led(lb_r, lb_g, lb_b);  // ride every host/audio frame
    if (!spk_active) {
        // Active push so the LED updates when the host is idle and animations
        // keep moving. Skipped during audio: the 0x36 frames already carry
        // state[]'s LED at audio rate, and slipping a 0x31 between them would
        // intrude on the load-bearing audio/haptic packet cadence.
        send_lightbar_color(lb_r, lb_g, lb_b);
    }
}

void lightbar_load_config() {
    const Config_body& c = get_config();
    lb_mode = c.lightbar_mode;
    if (lb_mode < 0 || lb_mode >= kNumLbModes) lb_mode = kLbModeHost;
    for (int i = 0; i < 4; i++) {
        lb_fav_r[i] = c.lb_fav_r[i];
        lb_fav_g[i] = c.lb_fav_g[i];
        lb_fav_b[i] = c.lb_fav_b[i];
    }
    lb_dirty = false;
}

void lightbar_save_config() {
    Config_body b = get_config();
    b.lightbar_mode = (uint8_t)lb_mode;
    for (int i = 0; i < 4; i++) {
        b.lb_fav_r[i] = lb_fav_r[i];
        b.lb_fav_g[i] = lb_fav_g[i];
        b.lb_fav_b[i] = lb_fav_b[i];
    }
    set_config(b);
    config_save();
    lb_dirty = false;
}

__attribute__((noinline)) void render_screen_vu() {
    fb_clear();
    draw_text(kContentX, 0, "Audio Meters");
    if (bt_is_connected()) {
        const uint8_t spk = audio_peak_speaker();
        const uint8_t hap = audio_peak_haptic();
        char buf[16];
        snprintf(buf, sizeof(buf), "SPK %3u", spk);
        draw_text(kContentX, 14, buf);
        rect_outline(48, 14, 80, 8);
        int sfill = (spk * 76) / 255;
        if (sfill > 0) rect_filled(50, 16, sfill, 4);

        snprintf(buf, sizeof(buf), "HAP %3u", hap);
        draw_text(kContentX, 28, buf);
        rect_outline(48, 28, 80, 8);
        int hfill = (hap * 76) / 255;
        if (hfill > 0) rect_filled(50, 30, hfill, 4);

        draw_text(kContentX, 42, "Live USB audio peaks");
    } else {
        draw_text(kContentX, 30, "(no controller)");
    }
    flush_fb();
}

void settings_adjust(int delta) {
    Config_body &c = settings_local;
    settings_dirty = true;
    switch (settings_sel) {
        case 0: { // haptics_gain  [1.0, 2.0] step 0.1
            int v = (int)(c.haptics_gain * 10.0f + 0.5f) + delta;
            if (v < 10) v = 10; if (v > 20) v = 20;
            c.haptics_gain = v / 10.0f;
            break;
        }
        case 1: { // speaker_volume  [-100, 0] step 5
            int v = (int)c.speaker_volume + delta * 5;
            if (v < -100) v = -100; if (v > 0) v = 0;
            c.speaker_volume = (float)v;
            break;
        }
        case 2: { // inactive_time  [10, 60] step 5
            int v = (int)c.inactive_time + delta * 5;
            if (v < 10) v = 10; if (v > 60) v = 60;
            c.inactive_time = (uint8_t)v;
            break;
        }
        case 3: c.disable_inactive_disconnect ^= 1; break;
        case 4: c.disable_pico_led ^= 1; break;
        case 5: { // polling_rate_mode  0..2
            int v = (int)c.polling_rate_mode + delta;
            if (v < 0) v = 2; if (v > 2) v = 0;
            c.polling_rate_mode = (uint8_t)v;
            break;
        }
        case 6: { // audio_buffer_length  [16, 128] step 4
            int v = (int)c.audio_buffer_length + delta * 4;
            if (v < 16) v = 16; if (v > 128) v = 128;
            c.audio_buffer_length = (uint8_t)v;
            break;
        }
        case 7: { // controller_mode  0..2
            int v = (int)c.controller_mode + delta;
            if (v < 0) v = 2; if (v > 2) v = 0;
            c.controller_mode = (uint8_t)v;
            break;
        }
        case 8: { // auto_haptics_enable  0..3
            int v = (int)c.auto_haptics_enable + delta;
            if (v < 0) v = 3; if (v > 3) v = 0;
            c.auto_haptics_enable = (uint8_t)v;
            break;
        }
        case 9: { // auto_haptics_gain  [0, 200] step 10
            int v = (int)c.auto_haptics_gain + delta * 10;
            if (v < 0) v = 0; if (v > 200) v = 200;
            c.auto_haptics_gain = (uint8_t)v;
            break;
        }
        case 10: { // auto_haptics_lowpass  0..3
            int v = (int)c.auto_haptics_lowpass + delta;
            if (v < 0) v = 3; if (v > 3) v = 0;
            c.auto_haptics_lowpass = (uint8_t)v;
            break;
        }
        case 11: { // screen_dim_timeout  [0,250] min, 0 = disabled
            int v = (int)c.screen_dim_timeout + delta;
            if (v < 0) v = 0; if (v > 250) v = 250;
            c.screen_dim_timeout = (uint8_t)v;
            break;
        }
        case 12: { // screen_off_timeout  [0,250] min, 0 = disabled
            int v = (int)c.screen_off_timeout + delta;
            if (v < 0) v = 0; if (v > 250) v = 250;
            c.screen_off_timeout = (uint8_t)v;
            break;
        }
        case 13: c.bt_mic_enable ^= 1; break; // BT mic on/off
    }
}

void settings_handle_input() {
    if (!bt_is_connected()) return;
    const uint8_t dpad = (uint8_t)(interrupt_in_data[7] & 0x0F);
    const uint8_t face = (uint8_t)(interrupt_in_data[7] & 0xF0);

    // Edge-trigger on D-pad direction CHANGE; only pure N/E/S/W to avoid diagonals
    if (dpad != settings_last_dpad && dpad != 8) {
        if      (dpad == 0) settings_sel = (settings_sel - 1 + kNumSettingsItems) % kNumSettingsItems;
        else if (dpad == 4) settings_sel = (settings_sel + 1) % kNumSettingsItems;
        else if (dpad == 6) settings_adjust(-1);
        else if (dpad == 2) settings_adjust(+1);
    }
    settings_last_dpad = dpad;

    // Triangle handling — Reset and Wipe-slots items both require a 2 s hold;
    // every other item saves edits on a normal short press.
    const bool tri_now = (face & 0x80) != 0;
    const bool tri_prev = (settings_last_face & 0x80) != 0;
    const bool is_hold_item = (settings_sel == kSettingsResetIdx
                               || settings_sel == kSettingsWipeSlotsIdx);
    if (tri_now && !tri_prev) {
        settings_tri_press_us = (uint32_t)time_us_32();
        settings_reset_triggered = false;
    }
    if (is_hold_item && tri_now && !settings_reset_triggered
        && ((uint32_t)time_us_32() - settings_tri_press_us) >= kResetHoldUs) {
        settings_reset_triggered = true;
        if (settings_sel == kSettingsResetIdx) {
            config_default();
            if (config_save()) {
                settings_local = get_config();
                lightbar_load_config(); // refresh RAM lightbar state (no reboot here)
                settings_dirty = false;
                settings_save_status = "Reset!";
            } else {
                settings_save_status = "Reset FAIL";
            }
        } else {
            bt_wipe_all_slots();
            settings_save_status = "Slots wiped!";
        }
    }
    if (!tri_now && tri_prev) {
        if (!is_hold_item && !settings_reset_triggered) {
            set_config(settings_local);
            settings_save_status = config_save() ? "Saved!" : "Save FAIL";
            if (settings_save_status[0] == 'S' && settings_save_status[1] == 'a') {
                settings_dirty = false;
            }
        }
        settings_reset_triggered = false;
    }
    settings_last_face = face;
}

__attribute__((noinline)) void format_settings_item(int idx, char* line, size_t n) {
    const Config_body &c = settings_local;
    const char *cur = (idx == settings_sel) ? ">" : " ";
    switch (idx) {
        case 0: {
            int g = (int)(c.haptics_gain * 10.0f + 0.5f);
            snprintf(line, n, "%s Hap Gain %d.%dx", cur, g / 10, g % 10);
            break;
        }
        case 1: snprintf(line, n, "%s Spk Vol %ddB", cur, (int)c.speaker_volume); break;
        case 2: snprintf(line, n, "%s Inact %umin", cur, c.inactive_time); break;
        case 3: snprintf(line, n, "%s InactDC %s", cur, c.disable_inactive_disconnect ? "off" : "on"); break;
        case 4: snprintf(line, n, "%s Pico LED %s", cur, c.disable_pico_led ? "off" : "on"); break;
        case 5: {
            const char* names[3] = {"250Hz", "500Hz", "RT"};
            snprintf(line, n, "%s Poll %s", cur, names[c.polling_rate_mode % 3]);
            break;
        }
        case 6: snprintf(line, n, "%s AudBuf %u", cur, c.audio_buffer_length); break;
        case 7: {
            const char* names[3] = {"DS5", "DSE", "Auto"};
            snprintf(line, n, "%s Ctrl %s", cur, names[c.controller_mode % 3]);
            break;
        }
        case 8: {
            const char* names[4] = {"Off", "Fallback", "Mix", "Replace"};
            snprintf(line, n, "%s AutoHap %s", cur, names[c.auto_haptics_enable & 3]);
            break;
        }
        case 9: snprintf(line, n, "%s AH Gain %u%%", cur, c.auto_haptics_gain); break;
        case 10: {
            const char* names[4] = {"80Hz", "160Hz", "250Hz", "400Hz"};
            snprintf(line, n, "%s AH LP %s", cur, names[c.auto_haptics_lowpass & 3]);
            break;
        }
        case 11:
            if (c.screen_dim_timeout == 0) snprintf(line, n, "%s ScrDim off", cur);
            else snprintf(line, n, "%s ScrDim %umin", cur, c.screen_dim_timeout);
            break;
        case 12:
            if (c.screen_off_timeout == 0) snprintf(line, n, "%s ScrOff off", cur);
            else snprintf(line, n, "%s ScrOff %umin", cur, c.screen_off_timeout);
            break;
        case 13: snprintf(line, n, "%s BT Mic %s", cur, c.bt_mic_enable ? "on" : "off"); break;
        case 14: snprintf(line, n, "%s Reset to defaults", cur); break;
        case 15: snprintf(line, n, "%s Wipe all slots", cur); break;
    }
}

__attribute__((noinline)) void render_screen_settings() {
    if (!settings_init_done) {
        settings_local = get_config();
        settings_init_done = true;
    }
    settings_handle_input();

    fb_clear();
    char buf[24];
    snprintf(buf, sizeof(buf), "Settings %s", settings_dirty ? "(*)" : "   ");
    draw_text(kContentX, 0, buf);
    if (settings_save_status[0]) {
        draw_text(86, 0, settings_save_status);
    }

    constexpr int kVisible = 5;
    int top = 0;
    if (settings_sel >= kVisible) top = settings_sel - kVisible + 1;
    char line[28];
    for (int i = 0; i < kVisible && top + i < kNumSettingsItems; i++) {
        format_settings_item(top + i, line, sizeof(line));
        draw_text(kContentX, 9 + i * 9, line);
    }

    if (settings_sel == kSettingsResetIdx) {
        draw_text(kContentX, 56, "Hold Tri 2s = RESET");
    } else if (settings_sel == kSettingsWipeSlotsIdx) {
        draw_text(kContentX, 56, "Hold Tri 2s = WIPE");
    } else {
        draw_text(kContentX, 56, "DP nav/adj  Tri=save");
    }
    flush_fb();
}

// ---- Slots screen (Phase G) ----------------------------------------------
// Multi-slot persistent pairing UI. Modeled on zurce/DS5Dongle-OLED.
// Credit to zurce.

int slots_cursor = -1;             // initialized to active slot on first entry
uint8_t slots_last_dpad = 8;
uint8_t slots_last_face = 0;
uint32_t slots_sq_press_us = 0;
bool slots_wipe_triggered = false;
const char* slots_status = "";
uint32_t slots_status_until_us = 0;
constexpr uint32_t kSlotsWipeHoldUs = 1500000;  // 1.5 s

void slots_handle_input() {
    if (slots_cursor < 0) slots_cursor = bt_get_slot();
    if (!bt_is_connected()) {
        // Even without a DS5 connected we still want to navigate / wipe;
        // we just can't read the controller's D-pad / face inputs. Return
        // here and require KEY0/KEY1 for screen switching.
        slots_last_dpad = 8;
        slots_last_face = 0;
        return;
    }
    const uint8_t dpad = (uint8_t)(interrupt_in_data[7] & 0x0F);
    const uint8_t face = (uint8_t)(interrupt_in_data[7] & 0xF0);

    if (dpad != slots_last_dpad && dpad != 8) {
        if      (dpad == 0) slots_cursor = (slots_cursor - 1 + kNumSlots) % kNumSlots;
        else if (dpad == 4) slots_cursor = (slots_cursor + 1) % kNumSlots;
    }
    slots_last_dpad = dpad;

    // Triangle rising edge: switch to cursor slot if different from active
    const bool tri_now = (face & 0x80) != 0;
    const bool tri_prev = (slots_last_face & 0x80) != 0;
    if (tri_now && !tri_prev) {
        if (slots_cursor != bt_get_slot()) {
            bt_set_slot(slots_cursor);
            slots_status = "Switched!";
            slots_status_until_us = (uint32_t)time_us_32() + 1500000;
        }
    }

    // Square hold 1.5 s: wipe cursor slot
    const bool sq_now = (face & 0x10) != 0;
    const bool sq_prev = (slots_last_face & 0x10) != 0;
    if (sq_now && !sq_prev) {
        slots_sq_press_us = (uint32_t)time_us_32();
        slots_wipe_triggered = false;
    }
    if (sq_now && !slots_wipe_triggered
        && ((uint32_t)time_us_32() - slots_sq_press_us) >= kSlotsWipeHoldUs) {
        slots_wipe_triggered = true;
        bt_forget_slot(slots_cursor);
        slots_status = "Wiped!";
        slots_status_until_us = (uint32_t)time_us_32() + 1500000;
    }
    if (!sq_now && sq_prev) slots_wipe_triggered = false;

    slots_last_face = face;
}

__attribute__((noinline)) void render_screen_slots() {
    slots_handle_input();
    if (slots_cursor < 0) slots_cursor = bt_get_slot();

    fb_clear();
    char hdr[24];
    const int active = bt_get_slot();
    const bool conn = bt_is_connected();
    snprintf(hdr, sizeof(hdr), "Slots         [s%d %s]", active, conn ? "ON" : "--");
    draw_text(kContentX, 0, hdr);

    if (slots_status[0] && (uint32_t)time_us_32() < slots_status_until_us) {
        draw_text(80, 0, slots_status);
    }

    for (int i = 0; i < kNumSlots; i++) {
        char line[28];
        const char *cursor_mark = (i == slots_cursor) ? ">" : " ";
        const char *active_mark = (i == active) ? "*" : " ";
        if (slot_occupied(i)) {
            uint8_t a[6];
            slot_get_addr(i, a);
            snprintf(line, sizeof(line), "%s%d%s %02X:%02X:%02X:%02X:%02X:%02X",
                     cursor_mark, i, active_mark, a[0], a[1], a[2], a[3], a[4], a[5]);
        } else {
            snprintf(line, sizeof(line), "%s%d%s (empty)", cursor_mark, i, active_mark);
        }
        draw_text(kContentX, 9 + i * 9, line);
    }

    draw_text(kContentX, 56, "Tri=switch Sq hold=wipe");
    flush_fb();
}

void boot_splash() {
    fb_clear();
    auto cx_for = [](const char* s) {
        int n = 0; while (s[n]) n++;
        return (128 - (n * 6 - 1)) / 2;
    };
    const char* l1 = "DS5 Bridge";
    const char* l2 = "v0.6.0";
    const char* l3 = "Pico2W + OLED";
    draw_text(cx_for(l1), 16, l1);
    draw_text(cx_for(l2), 30, l2);
    draw_text(cx_for(l3), 44, l3);
    flush_fb();
    sleep_ms(1500);
}

} // namespace

void oled_init() {
    spi_init(spi1, 10 * 1000 * 1000);
    gpio_set_function(kPinCLK, GPIO_FUNC_SPI);
    gpio_set_function(kPinMOSI, GPIO_FUNC_SPI);

    gpio_init(kPinCS);   gpio_set_dir(kPinCS, GPIO_OUT);  gpio_put(kPinCS, 1);
    gpio_init(kPinDC);   gpio_set_dir(kPinDC, GPIO_OUT);  gpio_put(kPinDC, 0);
    gpio_init(kPinRST);  gpio_set_dir(kPinRST, GPIO_OUT); gpio_put(kPinRST, 1);

    gpio_init(kPinKey0); gpio_set_dir(kPinKey0, GPIO_IN); gpio_pull_up(kPinKey0);
    gpio_init(kPinKey1); gpio_set_dir(kPinKey1, GPIO_IN); gpio_pull_up(kPinKey1);

    hw_reset();
    sh1107_init();
    fb_clear();
    boot_splash();

    // Restore the persisted lightbar mode + favorites (config_load() already ran
    // in main() before this). Defaults to HOST passthrough on a fresh flash.
    lightbar_load_config();
}

// Dim-tier renderer: blank the panel and draw a tiny "I'm alive" dot that
// breathes (1s on / 1s off) and walks through 8 evenly-spaced positions every
// ~30 s. Two goals: (1) reduce total pixel-on-time to a tiny fraction so the
// panel barely glows even with the contrast register pinned, (2) prevent any
// single pixel from accumulating wear. noinline keeps oled_loop's literal pool
// in Thumb's 4 KB reach (same constraint the other render_screen_* hit).
__attribute__((noinline))
void render_dim_pulse(uint32_t dim_elapsed_us) {
    fb_clear();
    constexpr uint32_t kPulsePeriodUs = 2UL * 1000000UL; // 2 s blink cycle
    constexpr uint32_t kPulseOnUs     = 1UL * 1000000UL; // 1 s on, 1 s off
    constexpr uint32_t kPosStepUs     = 30UL * 1000000UL; // 30 s per position
    constexpr int kPositions[][2] = {
        { 16,  8}, { 64,  8}, {112,  8},
        {112, 32},
        {112, 56}, { 64, 56}, { 16, 56},
        { 16, 32},
    };
    constexpr int kNumPositions = sizeof(kPositions) / sizeof(kPositions[0]);
    const bool dot_on = (dim_elapsed_us % kPulsePeriodUs) < kPulseOnUs;
    if (dot_on) {
        const int idx = (int)((dim_elapsed_us / kPosStepUs) % (uint32_t)kNumPositions);
        const int cx = kPositions[idx][0];
        const int cy = kPositions[idx][1];
        // 2x2 dot — small enough to barely register, big enough to see across a desk.
        rect_filled(cx, cy, 2, 2);
    }
    flush_fb_raw(); // skip chrome arrows; nothing to navigate to from sleep
}

void oled_loop() {
    handle_buttons();
    const uint32_t now = time_us_32();
    rumble_burst_tick(now);
    if ((now - last_render_us) < kFrameUs) return;
    last_render_us = now;
    // Track charge progress every frame — before the power-ladder early-returns
    // below, so step timing stays correct even while the panel is dimmed/off.
    sample_charge_eta();
    // Drive the controller LED every frame (any screen / power state): charging
    // pulse, selected OLED mode, or hand-off to the host. See lightbar_service().
    lightbar_service();
    // Bump activity on controller input changes (cheap rolling hash over input
    // bytes). Mirror bt.cpp's inactivity heuristic so resting-controller noise
    // doesn't read as activity: the analog sticks (idata[0..3]) jitter by ±1 LSB
    // at rest, so collapse their rest band [120,140] to a constant, and skip
    // idata[6] (the volatile counter byte bt.cpp's idle check also ignores).
    // Without this the dot/dim tier never engages while a controller is
    // connected, because a stick flicker resets the idle timer every few frames.
    uint32_t hash = 0;
    for (int i = 0; i < 10; i++) {
        if (i == 6) continue;
        uint8_t b = interrupt_in_data[i];
        if (i < 4 && b >= 120 && b <= 140) b = 128; // stick deadzone
        hash = hash * 31u + b;
    }
    if (hash != last_input_hash) {
        last_input_hash = hash;
        last_activity_us = time_us_64();
    }
    // Rising-edge: BT-connect itself counts as activity, so the screen wakes
    // the moment a controller pairs rather than waiting for the first input.
    const bool bt_connected_now = bt_is_connected();
    if (bt_connected_now && !prev_bt_connected) last_activity_us = time_us_64();
    prev_bt_connected = bt_connected_now;

    // Power-state ladder: Active → Dim (breathing dot) → Off based on idle time.
    // Thresholds are user-configurable (minutes; 0 = that tier disabled) — #5.
    // While charging we cap the ladder at Dim — the panel keeps doing the
    // low-power breathing dot but never fully sleeps. This stops the user from
    // unplugging the controller just to "wake" the dongle (which would reset the
    // charge-ETA calibration). The dot tier already draws ~no current, so this
    // costs little; sample_charge_eta() runs before this block regardless.
    const uint64_t idle   = time_us_64() - last_activity_us;
    const uint64_t dim_us = (uint64_t)get_config().screen_dim_timeout * 60ULL * 1000000ULL;
    const uint64_t off_us = (uint64_t)get_config().screen_off_timeout * 60ULL * 1000000ULL;
    const bool off_enabled = get_config().screen_off_timeout != 0;
    const bool dim_enabled = get_config().screen_dim_timeout != 0;
    if (off_enabled && idle > off_us && !g_charge_eta.charging) {
        if (oled_power_state != OLED_OFF) {
            cmd(0xAE);
            oled_power_state = OLED_OFF;
        }
        return; // panel is off, nothing to draw
    }
    if (oled_power_state == OLED_OFF) cmd(0xAF); // wake panel before drawing
    if (dim_enabled && idle > dim_us) {
        sh1107_set_contrast(kDimContrast);
        oled_power_state = OLED_DIM;
        render_dim_pulse((uint32_t)(idle - dim_us));
        return; // skip the regular per-screen render path
    }
    sh1107_set_contrast(kBrightLevels[bright_idx]);
    oled_power_state = OLED_ACTIVE;

    // True on the first render after navigating to a different screen.
    // Lets a screen do expensive one-shot work on entry (the CPU screen
    // caches its frequency-counter measurement here).
    static int last_rendered_screen = -1;
    const bool screen_entered = (current_screen != last_rendered_screen);

    // Leaving Trigger Test in either direction → reset the adaptive
    // trigger preset to OFF and push it to the controller. Otherwise
    // the last-cycled effect (Weapon snap, Galloping pulse, etc.)
    // stays active on the DS5 indefinitely, which surprised users
    // who'd just navigated away expecting a clean slate.
    if (last_rendered_screen == kScreenTriggers
        && current_screen != kScreenTriggers) {
        trigger_preset = 0;
        send_trigger_effect(0);
    }

    // Leaving the Lightbar screen → persist mode/favorite changes made there,
    // batched into a single flash write instead of one per button press.
    if (last_rendered_screen == kScreenLightbar
        && current_screen != kScreenLightbar
        && lb_dirty) {
        lightbar_save_config();
    }

    last_rendered_screen = current_screen;

    switch (current_screen) {
        case kScreenStatus:   render_screen();           break;
        case kScreenSlots:    render_screen_slots();     break;
        case kScreenLightbar: render_screen_lightbar();  break;
        case kScreenTriggers: render_screen_triggers();  break;
        case kScreenGyro:     render_screen_gyro();      break;
        case kScreenTouchpad: render_screen_touchpad();  break;
        case kScreenDiag:     render_screen_diag();      break;
        case kScreenCpu:      render_screen_cpu(screen_entered); break;
        case kScreenRssi:     render_screen_rssi();      break;
        case kScreenVU:       render_screen_vu();        break;
        case kScreenSettings: render_screen_settings();  break;
    }
}
