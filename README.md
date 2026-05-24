# Pico2W DualSense 5 Bridge — OLED Edition

[中文](./README.CN.md)

> Turn a Raspberry Pi Pico2W into a wireless adapter for the DualSense (DS5) controller — with an optional on-board status display.

> **OLED Edition** is a fork of **[awalol/DS5Dongle](https://github.com/awalol/DS5Dongle)** (upstream) that adds an optional Pico-OLED-1.3 128×64 display add-on with 11 screens (status, 4-slot multi-controller pairing, lightbar color picker with favorites and effect presets, trigger test, gyro tilt, touchpad, diagnostics, CPU/clock, BT signal strength, audio VU meters, and a persistent settings menu), plus a DS5 button-combo soft-reboot. Upstream is the authoritative source for the core bridge firmware; this fork tracks it and layers add-on features on top.

---

## 🛠️ Web Config Tool

**[→ Open the OLED Edition Web Config](https://marcelinevpq.github.io/DS5Dongle-OLED-Config-Web/#config)**

The web tool is a one-stop shop — **no installs, no command line, no `picotool`**. A brand-new Pico 2 W can go from "just out of the box" to fully flashed and configured without ever leaving the browser:

- **Flash Firmware tab** — put the Pico in **BOOTSEL mode**, then click *Connect to Pico* in the browser and *Flash now*. The site bundles the latest release UF2, or you can load a local `.uf2` you've built yourself. Powered by WebUSB.

  > **What is BOOTSEL mode?** It's the Pico's built-in flashing mode. To enter it: press and hold the small white button labeled **BOOTSEL** on the Pico, *then* plug the USB cable in (or, if it's already plugged in, briefly disconnect and reconnect while holding BOOTSEL). The Pico will appear to your computer as a removable drive — that's how you know it's in BOOTSEL mode. After the web tool flashes the firmware, the Pico auto-reboots into normal mode and is ready to use.
- **Config tab** — once the dongle is flashed and reconnected, edit haptics gain, speaker volume, polling rate, audio auto-haptics mode, and the rest of the persistent settings; save to the dongle's flash with one click. Powered by WebHID.
- **OLED Preview tab** — pixel-perfect emulation of all 11 OLED screens. Use the in-page KEY0/KEY1 buttons (or the controller's △ / R1 / D-pad when a DualSense is paired) to navigate. Adaptive triggers actually fire on the controller when you cycle the Trigger Test preset.

Works in any Chromium-based browser (Chrome, Edge, Brave, Opera). Firefox + Safari don't expose WebHID or WebUSB, so flashing and live config aren't available there — the OLED Preview still renders with mock data.

> Source for the web tool: **[MarcelineVPQ/DS5Dongle-OLED-Config-Web](https://github.com/MarcelineVPQ/DS5Dongle-OLED-Config-Web)** (fork of [awalol/ds5dongle-config-web](https://github.com/awalol/ds5dongle-config-web)).

---

## Overview

This project enables the Raspberry Pi Pico2W to function as a Bluetooth bridge for the DualSense controller, allowing wireless connectivity with enhanced haptics support.

## Features

**Core bridge (from upstream):**

- Full DualSense connectivity via Pico2W
- HD haptics (advanced vibration feedback)
- Wireless Bluetooth bridging
- Adjustable haptic gain via microphone volume
- Configurable LED and disconnection behaviors

**OLED Edition additions:**

- Optional Pico-OLED-1.3 status display with **11 screens** (status, slots, lightbar, trigger test, gyro tilt, touchpad, diagnostics, CPU/clock, RSSI, VU meters, settings)
- **4-slot persistent multi-controller pairing** — bond up to four DualSenses, switch between them from the OLED, slot 0 reconnects automatically on boot
- **Lightbar color picker** with 4 user favorite slots + breathing / rainbow / fade effect presets
- **Persistent settings menu** for the 8 firmware config fields (haptics gain, speaker volume, polling rate, etc.) with hold-to-confirm Reset and Wipe-all-slots actions
- **OLED idle power ladder** — manual brightness cycle (KEY1 long-press), automatic deep-dim with a small breathing dot at 2 min idle, full display-off at 15 min idle. Wakes instantly on button, controller pair, or input. Real burn-in protection, not just a contrast tweak.
- **Soft-reboot** without unplugging USB via DS5 `PS + Mute` hold (works headless) or **KEY0 + KEY1 held together for 1 s** on the OLED add-on (replaces the older KEY0 double-click gesture, which was easy to fire by accident while paging quickly)
- **Audit pass on the core bridge** — critical stack-overflow fix in the audio path (resolves long-standing "audio stuttering"), security hardening, watchdog, length validation across HID/L2CAP boundaries (see [CHANGELOG.md](./CHANGELOG.md))

## Hardware

### Required

| Item | Notes | Approx. price |
|---|---|---|
| **Raspberry Pi Pico 2 W** | RP2350 MCU with on-board CYW43 Bluetooth/WiFi. [Official product page](https://www.raspberrypi.com/products/raspberry-pi-pico-2/) | ~$7 USD |
| **Sony DualSense Controller** | Any standard PS5 DualSense (VID `054C:0CE6`). | — |
| **USB-C cable** | Connects the Pico 2 W to the host PC. | — |

### Optional (strongly recommended)

| Item | Notes | Approx. price |
|---|---|---|
| **Waveshare Pico-OLED-1.3** | 128×64 SH1107 OLED add-on board (SKU HIPI1798). Plugs directly onto the Pico 2 W headers. Firmware drives it automatically when present and gracefully no-ops when absent. [Product page](https://www.waveshare.com/pico-oled-1.3.htm) · [Wiki](https://www.waveshare.com/wiki/Pico-OLED-1.3) | ~$6 USD |
| **Small heatsink** for the RP2350 | The firmware overclocks the MCU to 320 MHz at 1.20 V (see [Performance / Overclocking](#performance--overclocking)). A small heatsink or thermal pad helps under sustained gameplay. | $1–3 USD |

### Where to buy

Both the Pico 2 W and the Waveshare Pico-OLED-1.3 are widely available worldwide:

- **Adafruit**, **Pimoroni**, **The Pi Hut**, **DigiKey**, **Mouser** — major electronics distributors (US / EU)
- **Waveshare's own store** for the OLED add-on
- Regional **Amazon** storefronts — search `Raspberry Pi Pico 2 W` and `Waveshare Pico-OLED-1.3` (or the SKU `HIPI1798`)
- **AliExpress** — original Waveshare and Pico stock plus clones; check seller ratings

## Getting Started

### Flashing Firmware

1. Hold the BOOTSEL button on the Pico2W
2. Connect the Pico2W to your computer via USB
3. The device will mount as a USB storage device
4. Drag and drop the .uf2 firmware file onto the device

### Pairing the Controller

1. Put the DualSense controller into Bluetooth pairing mode
2. Wait for the Pico2W to detect and connect
3. Once connected, the device will appear on the host system

## Configuration

There are four ways to configure the firmware:

**Web config (recommended, any Chromium-based browser):** open **[DS5 Bridge Config — OLED Edition](https://marcelinevpq.github.io/DS5Dongle-OLED-Config-Web/)** in Chrome, Edge, Vivaldi, Brave, or Opera (Firefox isn't supported — Mozilla declined WebHID). Click **Connect**, pick the DualSense from the browser dialog, and edit any field with a familiar form UI. The page talks directly to the Pico over WebHID — no driver, no install, no data leaves your machine. Source at [MarcelineVPQ/DS5Dongle-OLED-Config-Web](https://github.com/MarcelineVPQ/DS5Dongle-OLED-Config-Web).

**On-device (OLED add-on present):** use the on-screen **Settings** menu (10th screen). D-pad ▲▼ moves selection, ▶◀ adjusts values, △ saves to flash.

**Terminal CLI (any OS, any browser):** install hidapi, then use `scripts/set_ds5.py`:

```bash
pip install hidapi
scripts/set_ds5.py                            # show current config
scripts/set_ds5.py --auto-haptics fallback    # change a field, persist to flash
scripts/set_ds5.py --speaker-volume -10 --haptics-gain 1.5
scripts/set_ds5.py --slot 2                   # switch active multi-slot pairing
scripts/set_ds5.py --version                  # firmware version
scripts/set_ds5.py --rssi                     # live BT RSSI in dBm
scripts/set_ds5.py --help                     # full flag list
```

The script talks to the firmware over USB HID feature reports `0xF6`/`0xF7`/`0xF8`/`0xF9` — works on Linux, macOS, and Windows in any terminal regardless of which browser you use. Ported from [loteran/DS5Dongle](https://github.com/loteran/DS5Dongle) and extended for this fork's `current_slot` field.

**DualSense controller buttons (legacy fallback, no OLED, no CLI):**

### Microphone volume

Controls haptic gain multiplier. Range: `[1.0 – 2.0]`.

### Speaker mute

Disables LED connection indicator. Takes effect after controller reconnects.

### Microphone mute

Disables silent disconnection behavior.

## Notes

The Pico device will only be visible to the system after the controller is connected

Some behaviors depend on reconnection cycles to take effect

### Low-battery LED indicator

When the connected DualSense reports its battery at or below 10% (and it is not charging), the Pico onboard LED switches from solid-on to a 1 Hz blink so you can see the warning at a glance. The LED returns to solid-on as soon as the controller is plugged in or its reported level rises again. The blink also fires when `disable_pico_led` is set — the warning is treated as critical and overrides the LED-off preference; the LED returns to its disabled (off) state once the battery recovers or the controller starts charging.

To opt out at build time, configure with `-DENABLE_BATT_LED=OFF`. Default is ON.

## DualSense Microphone over Bluetooth

**The DualSense's built-in microphone works over the dongle's Bluetooth pairing** (since v0.6.8). The controller streams its mic as Opus audio; the dongle decodes it and presents it to the host as the standard DualSense USB capture device, so any app (Discord, OBS, in-game voice) can use it like a normal microphone.

This was long believed impossible — earlier versions of this fork documented it as a hard Sony-firmware limitation (and the Linux `hid-playstation` kernel driver still doesn't support it). It turned out to hinge on a single enable bit in the dongle's outbound audio report. **Full credit to [awalol](https://github.com/awalol/DS5Dongle) (upstream) for identifying it.** The corrected investigation log lives in [BLUETOOTH_AUDIO_NOTES.md](./BLUETOOTH_AUDIO_NOTES.md).

**Using it:**

- **On by default.** Pair the controller and the mic begins streaming within a second or two — no game audio required (the dongle keeps the stream alive on its own).
- **Raise the capture volume on the host** — it defaults low. On Linux, find the card with `arecord -l`, then e.g. `amixer -c <DualSense card> sset 'Headset' 90%`. Verify capture with `scripts/mic_diag.sh capture`.
- **Toggle it off to save controller battery** — OLED **Settings → BT Mic**, or the **BT microphone** switch in the [web config tool](#web-config-tool). Always-on mic keeps the DS5's audio subsystem awake, which drains its battery noticeably faster, so disable it if you don't use voice.

**Caveats:**

- Mic audio is **mono** (decoded mono, duplicated across the stereo capture endpoint).
- Toggling off mid-session stops the host feed immediately, but the controller keeps streaming until it next reconnects (there's no known "stop" command); connecting fresh with the toggle off never enables it.
- The OLED **Diagnostics** screen's `Mic in:` counter reads ~100/s while the mic is streaming — a quick way to confirm it's live.

## Known Issues

- Overclocking to 320 MHz @ 1.20 V is **required** for stable BT pairing. Dropping voltage to 1.10 V or clock to stock breaks the CYW43 PIO SPI bus and BT stops working. A small heatsink on the RP2350 is recommended for sustained gameplay.
- HD haptics may not fire in every game on Linux + Steam; this is game-side (some titles only send HD-haptic audio under Windows-specific APIs). Tested working in Spider-Man Remastered; not delivered in Ghost of Tsushima — same firmware, same controller.
- **USB 3.0 ports can disrupt pairing** — the controller may get stuck on a solid amber/yellow lightbar and never connect, while the same dongle works fine on a USB 2.0 port. This is RF interference, not a firmware bug; see [USB 3.0 ports & Bluetooth interference](#usb-30-ports--bluetooth-interference) below. (As of v0.6.8 the firmware auto-retries a stalled connection instead of hanging, which recovers many — but not all — marginal cases.)

## USB 3.0 ports & Bluetooth interference

If the dongle works on one port but not another, **try a USB 2.0 port first.** USB 3.0 ports and (especially) USB 3.0 extension cables emit broadband RF noise centered near 2.4 GHz — the same band the dongle's Bluetooth radio uses to talk to the controller. This is a well-documented industry issue (Intel, *"USB 3.0 Radio Frequency Interference Impact on 2.4 GHz Wireless Devices"*), not specific to this firmware. The noise desensitizes the dongle's BT receiver, so the controller can start connecting (amber lightbar) but the link is too noisy to complete — it hangs on yellow.

Mitigations, roughly in order of effectiveness:

1. **Plug the dongle into a USB 2.0 port** (often the simplest fix — many motherboards/cases have both).
2. **Use a short USB 2.0 extension cable** to get the dongle a few inches away from the USB 3.0 ports / metal chassis, improving line-of-sight to the controller. Avoid USB 3.0 extension cables specifically.
3. **Use a powered USB 2.0 hub** plugged into the USB 3.0 port — the hub downshifts the link and adds distance.
4. **Clip a ferrite bead** onto the cable near the dongle.
5. **Keep the controller closer / in line of sight** of the dongle during pairing.

The firmware will keep retrying a stalled connection on its own, so leaving it plugged in for ~10–20 s after the lightbar goes amber may let it recover without a replug.

## Performance / Overclocking

**You don't need to do anything for this — the overclock is baked into the firmware.** When you flash a UF2 from this repo, the Pico 2 W boots at the settings below automatically. There is no separate tool to run, no config file to edit, no fuses to blow.

Baked-in settings:

- **Voltage: 1.20 V** (`vreg_set_voltage(VREG_VOLTAGE_1_20)`)
- **Clock: 320 MHz** (`set_sys_clock_khz(SYS_CLOCK_KHZ, true)`)

Why it's required: at stock clock/voltage the CYW43 PIO SPI bus (the path the firmware uses to talk to the on-board Bluetooth chip) is unreliable and pairing fails. 320 MHz @ 1.20 V is the lowest combination we've verified to produce a stable BT link on this board.

If your device fails to boot a build you compiled yourself (unusual — only relevant when you've changed source), try increasing voltage slightly or lowering the clock in `src/main.cpp`. End users running official UF2 releases should not need to touch this.

A small heatsink on the RP2350 is **recommended for sustained gameplay** but not required for pairing or short sessions.

## Build Instructions

To build the project from source:

1. Install Pico SDK 2.2.0 (or later). The build uses `pico_sdk_import.cmake`.
2. **Pin TinyUSB to 0.20.0** inside the Pico SDK (`$PICO_SDK_PATH/lib/tinyusb`). This project's `tusb_config.h` uses the 4-argument form of `TUD_AUDIO_EP_SIZE`, which is not present in the 0.18.0 version that ships bundled with Pico SDK 2.2.0:
   ```bash
   cd "$PICO_SDK_PATH/lib/tinyusb"
   git fetch --tags
   git checkout 0.20.0
   ```
3. Configure and build with CMake + Ninja (or Make):
   ```bash
   cd /path/to/DS5Dongle
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_SDK_PATH="$PICO_SDK_PATH"
   cmake --build build --target ds5-bridge
   ```
4. The UF2 lands at `build/ds5-bridge-oled.uf2`. Flash with BOOTSEL as usual.

Build flags worth knowing:

- `-DENABLE_BATT_LED=ON` (default) — blink Pico LED on low DS5 battery.
- `-DENABLE_SERIAL=ON` — route printf to USB CDC for debugging (default OFF; releases UART for production builds).
- `-DPICO_W_BUILD=ON` — build for the original Pico W (drops audio, lowers clock). Default targets Pico 2 W.

## Diagnostics & debug tooling

Two ways to triage bridge issues — on-device via the OLED Diagnostics screen, and host-side via `scripts/mic_diag.sh` (Linux). The host-side path is faster: no screen-switching, no flash cycle, runs while the controller is in active use.

```
# One-shot snapshot — is the dongle on USB? Did ALSA enumerate it? Is the
# capture stream live? Is a controller currently paired?
scripts/mic_diag.sh status

# 3-second arecord on the mic IN endpoint — reports peak / RMS / non-zero
# count so we can tell "stream is silent" from "stream is producing audio".
scripts/mic_diag.sh capture 3

# Same as `status` but in a loop, prints only on state change. Useful for
# catching the exact second pairing completes or audio streams open / close.
scripts/mic_diag.sh watch

# Live read of the firmware's 0xFD vendor feature report (via /dev/hidraw):
# BT input counts + rates, last seen non-0x31 IDs, byte prefixes, AND the
# trigger-flow counters (host 0x02 received / with AllowTriggerFFB set /
# forwarded to BT). bt-trace prints a verdict — "host driver isn't sending
# trigger Allow bits" vs "forwarded but controller didn't actuate" — which
# is what would otherwise need a USB protocol analyzer.
scripts/mic_diag.sh bt-trace
```

Originally written to triage the parked DS5 BT-microphone investigation (see [BLUETOOTH_AUDIO_NOTES.md](./BLUETOOTH_AUDIO_NOTES.md)). The `0xFD` feature report and `bt-trace` decoder now also carry the trigger-flow counters added for [issue #3](https://github.com/MarcelineVPQ/DS5Dongle-OLED-Edition/issues/3) (missing adaptive trigger tension in some games).

## OLED Display Add-on (optional)

If you plug a [Waveshare Pico-OLED-1.3](#hardware) onto the Pico2W's headers, the firmware drives it automatically as a live status display. No configuration needed — the firmware no-ops gracefully when no OLED is present.

### Boot splash (1.5 s on power-on)

Centered firmware version on a blank screen for 1.5 seconds, then jumps to the Status screen.

### Eleven screens, cycled with KEY0 on the add-on

Cycle order: **Status → Slots → Lightbar → Trigger Test → Gyro Tilt → Touchpad → Diagnostics → CPU/Clock → BT Signal → VU Meters → Settings →** wrap. **KEY0 short-press steps forward; KEY1 short-press steps backward** — on *every* screen. Per-screen interactions (cycling the trigger preset, cycling the lightbar mode, navigating the Settings cursor, switching slots) live on the **DualSense controller buttons**, never on KEY0/KEY1, so the two physical buttons on the OLED add-on always mean the same thing.

Every screen also paints **`>`** at the top-left edge (next to KEY0) and **`<`** at the bottom-left edge (next to KEY1) so the on-screen labels physically pair with the buttons.

#### 1. Status

Connection state, paired DualSense BD address, battery % with bar (`+` charging / `*` complete / `!` error), live analog stick positions, D-pad, face buttons (△ ◯ ✕ □), L1/R1, and L2/R2 analog trigger fill bars. The link indicator and battery use small pixel icons.

<img src="./assets/oled/oled_sc01.jpg" alt="Status screen on the OLED" width="420">

#### 2. Slots

Persistent 4-slot multi-controller pairing. Browse stored controllers, switch active slot, or wipe a single slot. `>` is the cursor, `*` marks the currently active slot.

<img src="./assets/oled/oled_sc02.jpg" alt="Slots screen on the OLED" width="420">

- **D-pad ▲▼** — move cursor across slots 0–3
- **△** — switch to the cursor slot (disconnect current, reconnect to slot's stored controller)
- **□ hold 1.5 s** — wipe the cursor slot (drops bd_addr + BTstack link key)
- Active slot is persisted; the dongle reconnects to it on next boot

#### 3. Lightbar Color Picker

Tilt the controller on each axis to dial in R / G / B; the firmware sends the resulting color to the DualSense's actual lightbar at 10 Hz, so the lightbar IS the visual preview (the OLED is monochrome).

<img src="./assets/oled/oled_sc03.jpg" alt="Lightbar color picker on the OLED" width="420">

- Press **△ ◯ ✕ □** on the controller to **save** the current color into favorite slot 0 / 1 / 2 / 3
- Press **R1** on the controller to cycle the mode tag: `[LIVE]` → `[FAV0]` → `[FAV1]` → `[FAV2]` → `[FAV3]` → effects (Breathing / Rainbow / Fade) → back to `[LIVE]`
- Default favorites: Red, Green, Blue, White

#### 4. Trigger Test

Press **△** on the controller to cycle seven adaptive trigger effects applied to both L2 and R2. Pull each trigger to feel the effect.

<img src="./assets/oled/oled_sc04.jpg" alt="Trigger Test screen on the OLED" width="420">

Cycle order: **Off → Feedback → Weapon → Vibration → Bow → Gallop → Machine Gun → Off …** Effect parameters bitpacked per [dualsensectl](https://github.com/nowrep/dualsensectl)'s reverse-engineering, all at max strength.

#### 5. Gyro Tilt

Live X/Y/Z accelerometer values with a 40×40 crosshair box. Tilt the controller and the dot tracks in real time.

<img src="./assets/oled/oled_sc05.jpg" alt="Gyro Tilt screen on the OLED" width="420">

#### 6. Touchpad

Live render of the touchpad surface. Dots appear at current finger positions; the count updates as fingers touch / leave.

<img src="./assets/oled/oled_sc06.jpg" alt="Touchpad screen on the OLED" width="420">

#### 7. Diagnostics

Scrollable list of live counters — uptime, BT state, host → BT trigger flow (`host02` / `trig` / `tx`), BT 0x31 input rate, USB audio frames/sec, BT 0x32 packets/sec, and parked mic-investigation counters at the bottom. Controller D-pad ▲/▼ scrolls; tiny `^` / `v` glyphs at the right edge mark "more above/below." Read-only, so no cursor. Useful for verifying the bridge is moving bytes without needing a UART cable.

The same counters are also exported on HID feature report `0xFD` for host-side tooling — see `scripts/mic_diag.sh bt-trace` below.

<img src="./assets/oled/oled_sc07.jpg" alt="Diagnostics screen on the OLED" width="420">

#### 8. CPU / Clock

Live RP2350 vitals: configured (`Set`) and actually-running (`Real`) system clock measured against the crystal reference, core voltage read back from the regulator, and on-die temperature (256-sample average + slow EMA so the value tracks the real die temp rather than ADC noise).

<img src="./assets/oled/oled_sc08.jpg" alt="CPU / Clock diagnostics screen on the OLED" width="420">

The same telemetry is also exported on HID feature report `0xFC` for tooling.

#### 9. BT Signal

Live Bluetooth signal strength of the active link, in dBm with a bar. Closer to 0 dBm is stronger; −90 dBm is weak. Includes a qualitative label (Poor / Fair / Good / Excellent).

<img src="./assets/oled/oled_sc09.jpg" alt="BT Signal screen on the OLED" width="420">

#### 10. VU Meters

Live peak meters for the speaker and haptic audio paths. Useful for verifying audio routing without the controller being plugged in to a host.

<img src="./assets/oled/oled_sc10.jpg" alt="VU Meters screen on the OLED" width="420">

#### 11. Settings

Persistent config editor. D-pad ▲▼ moves the selection, ▶◀ adjusts values, △ saves to flash. Includes the firmware-config fields (haptics gain, speaker volume, inactive timeout, polling rate), the Audio Auto Haptics controls, and two hold-to-confirm actions:

<img src="./assets/oled/oled_sc11.jpg" alt="Settings screen on the OLED" width="420">

- **AutoHap Off / Fallback / Mix / Replace** — selects the Audio Auto Haptics mode. Default `Fallback` fires derived rumble only when the game sends no native haptic data (e.g. Ghost of Tsushima on Linux); games that *do* send native haptics (Spider-Man Remastered) pass through unchanged. `Mix` adds derived on top of native, `Replace` ignores native entirely, `Off` disables.
- **AH Gain N%** — derived-signal gain, 0–200 % in 10 % steps. Default 100 %.
- **AH LP 80/160/250/400 Hz** — low-pass cutoff applied to the speaker audio before envelope follow. Lower = more sub-bass, higher = more presence. Default 160 Hz.
- **Reset to defaults** — hold △ for 2 s to revert all config fields
- **Wipe all slots** — hold △ for 2 s to drop all 4 paired controllers + all BTstack link keys

### Button reference

The two physical buttons on the OLED add-on are **strictly navigation**:

| Button | Action |
|---|---|
| **KEY0** short-press | Next screen (forward) |
| **KEY1** short-press | Previous screen (backward) |
| **KEY1** long-press (≥ 1.5 s) | Cycle OLED brightness level |
| **KEY0 + KEY1** held together ≥ 1 s | `watchdog_reboot` — soft-reboot without unplugging USB |

Per-screen state changes (cycling the trigger preset, cycling the lightbar mode, navigating Settings, switching slots, saving colors to favorite slots) all happen on the **DualSense controller buttons** — never on KEY0 / KEY1 — so the two physical buttons always mean the same thing across every screen. See each screen's section above for which controller button does what.

### Pinout (standard Waveshare Pico HAT layout)

| Function | GPIO |
|---|---|
| MOSI | 11 |
| SCK  | 10 |
| CS   | 9  |
| DC   | 8  |
| RST  | 12 |
| KEY0 | 15 |
| KEY1 | 17 |

### Soft-reboot recovery

Two ways to reboot the dongle without unplugging USB — handy if pairing gets stuck or you want a clean state:

- **OLED KEY0 + KEY1 held simultaneously for ≥ 1 s** → `watchdog_reboot`. Replaces the older "KEY0 double-click" gesture from earlier versions, since rapid forward-navigation kept tripping the double-click timer by accident.
- **DualSense `PS + Mute` held for 2 seconds** → `watchdog_reboot` (works whether or not the OLED is attached — headless backup).

## Acknowledgements

Some features and design ideas in this fork are borrowed from other forks of upstream, with credit:

- **[zurce/DS5Dongle-OLED](https://github.com/zurce/DS5Dongle-OLED)** — pixel-art icons in the OLED status header (visual approach), the "hold for factory reset" UX pattern used by the Settings screen's "Reset to defaults" item (hold △ for 2 s to confirm), and the multi-slot persistent BT pairing system on the new Slots screen (4 bonded controllers, D-pad to navigate, △ to switch slots, □ hold to wipe a slot, plus "Wipe all slots" in the Settings menu).
- **[loteran/DS5Dongle](https://github.com/loteran/DS5Dongle)** — independent rediscovery of the upstream `3a31bd7` regression that broke speaker / HD haptic output (commit `c7a8d3c`); the fix in our `src/audio.cpp` restores the same SetStateData sub-report. Also the source of the Audio Auto Haptics DSP (1-pole LP + envelope follower, see Settings → Auto Haptics) and the "don't sync USB-side UAC1 volume to the persistent config" fix.
- **[awalol/ds5dongle-config-web](https://github.com/awalol/ds5dongle-config-web)** — base for our forked web config app at [MarcelineVPQ/DS5Dongle-OLED-Config-Web](https://github.com/MarcelineVPQ/DS5Dongle-OLED-Config-Web). The fork adapts upstream's 13-byte Config_body layout to our 19-byte one and adds UI for our additions (multi-slot pairing, Auto Haptics).

## Roadmap
- Please check out [DS5Dongle plan](https://github.com/users/awalol/projects/5)

## Community
- Join the Discord server: [Discord Server](https://discord.gg/hM4ntchGCa)
- If you have a bug, please open an issue instead.

## References

- [rafaelvaloto/Pico_W-Dualsense](https://github.com/rafaelvaloto/Pico_W-Dualsense) — Project inspiration
- [egormanga/SAxense](https://github.com/egormanga/SAxense) — Bluetooth Haptics POC
- [https://controllers.fandom.com/wiki/Sony_DualSense](https://controllers.fandom.com/wiki/Sony_DualSense) - DualSense data report structure documentation
- [Paliverse/DualSenseX](https://github.com/Paliverse/DualSenseX) — Speaker report packet
