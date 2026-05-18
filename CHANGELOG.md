# Changelog

All notable changes to **Pico2W DualSense 5 Bridge — OLED Edition** are documented here. This fork tracks [awalol/DS5Dongle](https://github.com/awalol/DS5Dongle) (upstream) and adds an optional OLED status display plus a security/correctness audit pass on the core bridge.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versioning approximates [SemVer](https://semver.org/) with the upstream version stream — the fork shares a major.minor with whatever upstream tag it is rebased on.

---

## [Unreleased]

### Added

- **CPU / Clock diagnostics screen** (`kScreenCpu`, inserted between Diagnostics and BT Signal in the K0 cycle). Shows the configured system clock (`SYS_CLOCK_KHZ` — the overclock target), the *actually running* `clk_sys` measured live by the RP2350 on-chip frequency counter against the crystal reference, the core voltage read back from the regulator (`vreg_get_voltage()`, not the compile-time constant), and the RP2350 on-die temperature (ADC input 4). Pure read-only instrumentation; one-time ADC bring-up and no other code path uses the ADC, so it is conflict-free. `render_screen_cpu()` is `noinline` like the other render functions (Thumb literal-pool reach). Adds `hardware_adc` to `target_link_libraries`. The frequency-counter measurement (a multi-ms busy-wait) runs **once on screen entry** and is cached — `clk_sys` is fixed at boot, so only the temperature refreshes per frame, avoiding a per-frame BT/audio hitch while the screen is visible. `oled_loop()` gained a generic `screen_entered` flag for this. Hardware-verified on Pico 2 W + OLED. Also exported over a new HID feature report **`0xfc`** (`src/cmd.cpp`) — 11 bytes: set_khz, cached real_khz, vreg code, ADC ch4 raw — so the web config emulator can show live CPU telemetry (volts/temp math done web-side to keep the firmware HID path float-free).

### Fixed (web telemetry — latent since the slots/diag reports landed)

- **CPU/Clock temperature was a single noisy ADC sample.** The RP2350 temp sensor has a shallow slope (−1.721 mV/°C, ~1 LSB ≈ 0.47 °C) so a lone 12-bit reading swings several tenths of a degree per frame — the displayed value just mirrored the latest noisy sample instead of the true die temperature. New `cpu_temp_raw_smoothed()` in `src/cmd.cpp` averages a 256-sample block then runs a slow EMA (α=0.15, seeded on first call). It is the **single source of truth**: both `render_screen_cpu()` and the `0xfc` web telemetry call it, and the duplicated per-site ADC bring-up was removed (ADC now initialised in exactly one place). `oled.cpp` no longer touches the ADC directly (drops `hardware/adc.h`, adds `cmd.h`).
- **Live web telemetry over WebHID: not feasible on the target setup; abandoned.** A browser-side read-only diagnostic proved Chrome WebHID returns `NotAllowedError` for any report ID **not declared** in the parsed HID report descriptor (declared `0xF7`/`0xF8`/`0xF9` read fine; undeclared `0xFA`/`0xFB`/`0xFC` fail). Declaring them is therefore mandatory for the web read — but doing so (even applied atomically with a matching `wDescriptorLength`, correct bytes identical in shape to the working `0xF6`–`0xF9`, and a `bcdDevice` cache-bust bump) made the device fail to enumerate as a usable HID device on the user's real Windows machine in **two** independent attempts (Device Manager showed it; WebHID and the PlayStation Accessories app did not). The cloned DualSense HID report descriptor cannot be safely extended on this environment. Reverted to the original descriptor (`0x0141`/`0x01B5`, no vendor feature reports, `bcdDevice 0x0100`). Retained with **no USB impact**: the `0xfc` firmware handler and the temperature smoothing/`cpu_temp_raw_smoothed()`. The on-device CPU/Clock OLED screen is fully working and hardware-verified; the web preview's CPU screen stays on representative mock values (the slots/diagnostics web screens were never readable for the same root cause and are likewise mock-only when connected).

### Fixed

- **Low-battery LED keeps blinking after controller disconnect** (`fb68ea5`). When the DualSense's battery dropped low enough to trigger `battery_led_tick`'s blink and the controller subsequently disconnected (typically: battery fully depletes and the BT link drops), the Pico's onboard LED stayed frozen in whichever half-cycle it was in at the moment of disconnect; reconnect-retry windows could even briefly resume blinking. New `battery_led_on_disconnect()` clears blink state, forces LED off, and zeros `last_report_us` so the stale-check early-return blocks any new blink until a fresh 0x31 report arrives on the next connection. Stale-check in the tick also now forces LED off when it fires mid-blink (defense in depth for ungraceful disconnects). Reported by Sura Academy on Discord. Same bug present in upstream — sent back as [awalol/DS5Dongle#101](https://github.com/awalol/DS5Dongle/pull/101).

---

## [0.6.0-oled-edition] — 2026-05-17

Tagged release of the rebase below. UF2s attached to [the GitHub release](https://github.com/MarcelineVPQ/DS5Dongle-OLED-Edition/releases/tag/v0.6.0-oled-edition) (built by `.github/workflows/release.yml`). No code changes vs the rebase; tag exists so users can install from a stable artifact.

---

## [0.6.0-rebase] — 2026-05-17

Rebased onto upstream `awalol/DS5Dongle` `v0.6.0-hotfix`. All OLED Edition features preserved with no user-visible regression.

### Changed

- **Adopted upstream's `state_mgr` refactor** (`awalol/DS5Dongle#93`) as the new base for controller-state and audio-packet construction. The local speaker / HD-haptic regression fix we shipped in `bff65d6` (re-introducing a 63-byte `state_data` block into every audio packet) is **dropped** — upstream's `state_mgr.cpp` is the proper architectural fix and supersedes the hack. Speaker, HD haptics, and basic rumble all verified working on the rebased firmware. Same fix loteran/DS5Dongle shipped as `c7a8d3c` ~6 h before ours.
- All upstream commits between our prior base (`a2e3a33`) and `v0.6.0-hotfix` are now in our tree: `0a33aae` PR#93 merge, `b1019fb` README update, `54a4b69` config struct comment typo, `f882ff1` adjust default state-init volume, `77bbee5` rumble transfer fix, `c2e0d84` state init after connected, `7bbe37b` state_mgr refactor itself, `9a2c2b4` discoverable / connectable off when connected, `508a841` DISABLE_SPEAKER_PROC option, `b308545` audio.cpp comment, `0ed05d3` rumble hotfix.
- Our `update_discoverable()` helper (gates `gap_discoverable_control` on `slots_any_empty()`) replaces upstream's stricter `gap_*_control(false)` pair on L2CAP connect. Effect: dongle stays discoverable while at least one slot is empty (needed for slot-1/2/3 pairing); only goes dark when all 4 are full. Strictly looser than upstream's rule, but correct for the multi-slot use case.

### Fixed

- **`awalol/DS5Dongle#100` "(v0.6.0) Dongle is detected as a new device on Windows when using different USB ports"** — upstream's `e79c762 remove usb serialnumber #32` zeroed the device descriptor's `iSerialNumber` to work around a SpecialK compat issue, but that broke Windows device identity: users lost per-device volume / app settings every time they moved the dongle to a different USB port. The OLED Edition restores `iSerialNumber = STRID_SERIAL` (per-board unique serial from flash chip ID via `board_usb_get_serial`). Trade-off documented in `src/usb_descriptors.cpp`: re-introduces SpecialK incompatibility (`#32`) for the narrower set of Windows users with that specific tool, in exchange for stable device identity for the broader Windows population.

### Verification

End-to-end on user's hardware after the rebase:
- DS5 pairs cleanly.
- Speaker audio via `scripts/test_speaker.sh --tone 440 3` — audible (load-bearing check that upstream's state_mgr does what we expect).
- Basic rumble works in games (validates we didn't disturb upstream's `0ed05d3` hotfix).
- All 10 OLED screens render correctly with live data.
- Multi-slot pairing: switch between slots, slot_assign on empty, wipe-from-Settings all work.
- PS + Mute hold-2s combo reboot triggers `watchdog_reboot` (validates our `interrupt_loop` addition coexists with upstream's `state_update` flow).

### Unchanged

The full prior CHANGELOG history for `[0.5.4]` and earlier sessions remains accurate and below.

## [Unreleased]

### Fixed

- **DualSense speaker + HD haptic actuator regression** — upstream commit `3a31bd7` (2026-05-12, "refactor: add SetStateData and audio send priority") moved the `0x10` SetStateData sub-report out of every `0x36` audio packet and into a one-time L2CAP-open setup. The DS5 hardware requires that sub-report to be re-asserted on every audio frame (the `0x7f 0x7f` Headphones+Speaker volume bytes specifically) or the speaker and HD haptic actuators silently stop producing output. Restored in `src/audio.cpp` with the pre-3a31bd7 packet layout (state_data at `pkt[11..75]`, haptic at `pkt[76..141]`, speaker at `pkt[142..343]`). Same fix shipped independently by loteran/DS5Dongle as commit `c7a8d3c` ~6 h before ours; credit to loteran for the clearer hardware-side explanation in their commit message.
- **USB UAC1 SET_CUR Volume request no longer overrides flash-persisted speaker_volume.** PipeWire / PulseAudio re-apply their last-known UAC1 volume on every device reconnect, which had silently overridden whatever the user had saved in the OLED Settings menu. The in-memory `volume[]` array still tracks live host volume; only the flash sync was removed. Fix borrowed from loteran/DS5Dongle commit `03fa1e4`.

### Added

- **Audio Auto Haptics** — derive haptic feedback from the speaker audio (UAC channels 0/1) for games that send sound but no per-frame haptic data, e.g. Ghost of Tsushima on Linux + Steam. DSP is a 1-pole low-pass + envelope follower + modulation + soft-clip (`x / (1 + |x|)`, avoids `tanhf` on Cortex-M33), borrowed from loteran/DS5Dongle commit `5d6bc2f`. Four modes selectable from the OLED Settings menu: **Off** / **Fallback** / **Mix** / **Replace**. Default is **Fallback** — derived rumble fires only after the game has been silent on the native haptic path (channels 2/3) for ~1 s, so games that send native HD haptics (Spider-Man Remastered) are not overridden, while games that don't (Ghost of Tsushima) get derived haptics out of the box. Gain (0–200 %) and LP cutoff (80 / 160 / 250 / 400 Hz) are also tunable from the Settings menu.
- **Audio Diagnostics counters** on the OLED Diagnostics screen: `USB aud N/s` (UAC1 frames per second arriving from the host) and `BT 0x32 N/s` (audio packets emitted to the DS5 per second). Lets the user verify the speaker/haptic path is actually moving bytes without needing a UART cable. Used to triage the speaker regression above.

---

## [0.5.4] — 2026-05-16

First full OLED Edition release. Includes upstream's v0.5.4 base plus the audit pass and the entire OLED add-on feature set.

### Added — OLED display add-on

Requires a Waveshare [Pico-OLED-1.3](https://www.waveshare.com/wiki/Pico-OLED-1.3) (128×64 SH1107). Firmware drives it automatically when present and no-ops gracefully when absent.

- **Boot splash** (1.5 s) showing firmware version on power-on.
- **10 screens**, cycled with KEY0 (forward) / KEY1 (back, except where contextual):
  1. **Status** — connection state, paired BD address, battery % with pixel-icon battery, live stick / D-pad / face-button / L1-R1 / L2-R2 trigger visualization.
  2. **Slots** — persistent 4-slot multi-controller pairing (see below).
  3. **Lightbar Color Picker** — tilt-to-RGB live preview on the controller's lightbar, with 4 user-savable favorite slots (△ ○ ✕ □) and three effect presets (Breathing, Rainbow, Fade).
  4. **Trigger Test** — cycles 7 DS5 adaptive trigger effects (Off / Feedback / Weapon / Vibration / Bow / Galloping / Machine Gun) on both L2 and R2, bitpacked per [dualsensectl](https://github.com/nowrep/dualsensectl)'s reverse-engineering.
  5. **Gyro Tilt** — live X/Y/Z accelerometer values + 40×40 crosshair box that tracks tilt in real time.
  6. **Touchpad** — live render of finger positions on the touchpad surface, with a finger count.
  7. **Diagnostics** — uptime, BT state, HCI / audio-FIFO / opus-FIFO counter stubs (kept for future wiring).
  8. **RSSI** — live BT signal strength of the active link, dBm + bar.
  9. **VU Meters** — live peak meters for the speaker + haptic audio paths.
  10. **Settings** — persistent on-device editor for the 8 firmware config fields, plus "Reset to defaults" (hold △ 2 s) and "Wipe all slots" (hold △ 2 s).
- **OLED brightness control** — KEY1 long-press cycles brightness levels.
- **Auto-dim** after 5 minutes of input idle (lifespan / burn-in protection).
- **Soft-reboot recovery** without unplugging USB:
  - OLED KEY0 **double-click** (~400 ms window) → `watchdog_reboot`.
  - DS5 `PS + Mute` held for 2 seconds → `watchdog_reboot` (works without the OLED).
- **Pixel-art icons** in the Status screen header (link indicator + battery icon).

### Added — 4-slot persistent multi-controller pairing

- Bond up to 4 DualSenses; switch between them from the **Slots** OLED screen.
- Active slot persisted in the existing flash-backed config; dongle reconnects to the last-used controller on boot.
- D-pad ▲▼ to move cursor, △ to switch to the cursor slot (disconnect current ACL, restart inquiry filtered to the new slot's bd_addr), □ hold 1.5 s to wipe a single slot.
- "Wipe all slots" item in the Settings menu drops all 4 stored bd_addrs + all BTstack link keys in one shot.
- Inquiry filter on `HCI_EVENT_INQUIRY_RESULT` enforces slot ownership: devices stored in other slots are skipped; empty slots auto-assign on the first L2CAP HID_CONTROL channel open.
- Dongle stops being BT-discoverable once all 4 slots are full (security tightening; was permanently discoverable in upstream).
- Storage: BTstack's TLV link-key DB holds the keys (`NVM_NUM_LINK_KEYS=4`, unchanged from upstream); a new dedicated flash sector holds the 4 bd_addrs + occupancy bits + magic word.
- Multi-slot UX modeled on [zurce/DS5Dongle-OLED](https://github.com/zurce/DS5Dongle-OLED). Credit to zurce.

### Added — security / correctness audit pass

Critical and high-severity fixes on the core bridge code. Many of these have since landed upstream independently; this changelog captures what this fork shipped.

- **C1**: 4.8× stack-overflow in `core1_entry`'s `out_buf` (200 floats vs. 960 floats the resampler/encoder actually writes/reads). Root cause of the long-standing "audio may experience slight stuttering" known issue. Buffer resized and Opus return value now checked.
- **C2 + H5**: Variable-length stack array in `set_feature_data` sized by host-controlled length (potential stack blowup). Replaced with a bounded fixed buffer + length validation; CRC bounds tightened so `len < 4` no longer wraps to a huge `size_t`.
- **C3**: Unbounded `memcpy` into a 78-byte stack buffer in `tud_hid_set_report_cb` for HID report 0x02 (overflow if `bufsize > 76`). Bounded.
- **H1**: `tud_hid_get_report_cb` wrote `feature_data.size() - 1` bytes into the host-supplied buffer without clamping to `reqlen` (host-buffer overflow). Clamped.
- **H2**: OOB read in `on_bt_data` (`data[56]` / `memcpy(.., data+3, 63)` with no length check on the incoming BT frame). Bounded.
- **H3**: Same pattern in `l2cap_packet_handler` (`packet[3]` read with no size check). Bounded.
- **H4**: UAC1 volume parsed as `uint16` instead of signed `int16` — non-conformant hosts could crank haptic gain to 256× and saturate everything. Fixed sign extension.
- **H7**: BT report sequence counter in `main.cpp` cycled with period 16 (incremented as `int`, written as `(c<<4)`). Replaced with the correct `(c+1) & 0x0F` wrap used in `audio.cpp`.
- **N1**: Pairing-failure recovery: three `gap_inquiry_start(30)` calls were commented out in `bt.cpp`'s error paths (HCI command status, connection complete, authentication complete). Dongle would soft-brick after any pairing failure that didn't end in a disconnect. Uncommented.
- **N2**: Removed printf of the bonding link-key to UART on every reconnect.
- **N3**: CRC-helper bounds: `fill_output_report_checksum` / `fill_feature_report_checksum` underflowed `size_t` and wrote at negative offsets on `len < 4`. Guarded.
- **N4**: HCI command-send return values logged so a future stuck-dongle report has observability.
- **N5**: Watchdog enabled (8 s). Hangs now recover automatically instead of needing a power-cycle.

### Added — pairing posture hardening

- Tightened `gap_discoverable_control(1)` — the dongle now only advertises as pairable when at least one slot is empty. Once all 4 slots are full, it goes non-discoverable (still connectable to bonded controllers).
- (Carried over from upstream's own hardening; documented here for completeness.)

### Changed

- Project rebranded as **OLED Edition** to differentiate from upstream. UF2 artifact renamed: `ds5-bridge.uf2` → `ds5-bridge-oled.uf2`.
- README rewritten with full hardware section (Pico 2 W + Pico-OLED-1.3 SKUs, vendor links, prices), all 10 OLED screen mockups in the new cycle order, and explicit `TINYUSB 0.20.0` pin requirement (the 0.18.0 bundled with Pico SDK 2.2.0 lacks the 4-arg form of `TUD_AUDIO_EP_SIZE` used by this project's `tusb_config.h`).
- KEY1 short-press: was a 250 ms test-rumble burst on most screens; now cycles **backward** through screens (mirror of KEY0). Still cycles trigger preset on Trigger Test and lightbar mode on Lightbar (their primary in-screen interactions).
- Screen cycle order reorganized: **Status → Slots → Lightbar → Trigger Test → Gyro Tilt → Touchpad → Diagnostics → RSSI → VU Meters → Settings**. Settings last; the three "test" stimulus screens grouped together; diagnostic screens grouped together. Screen indices are symbolic constants (`kScreenStatus`, `kScreenSlots`, …) so future reorders are a one-block edit.

### Fixed

- `config_default()` was declared in `config.h` but never defined upstream (latent "undefined symbol" → linker "dangerous relocation" if anything called it). Implemented (fills body with `0xFF`, runs `config_valid()` — same path as a freshly-erased flash sector).
- GitHub Actions workflow `cp` paths corrected to match the `ds5-bridge-oled.uf2` produced after the CMake `OUTPUT_NAME` rename. (Workflows had failed on every push since the rebrand commit.)

### Build

- TinyUSB pinned to `0.20.0` (required; see Build Instructions in README).
- Pico SDK 2.2.0 + toolchain `14_2_Rel1` validated. Pico 2 W (RP2350) is the primary target.
- `CMakeLists.txt` adds `src/oled.cpp` + `src/slots.cpp` to the executable target, links `hardware_spi`.
- `OUTPUT_NAME` set to `ds5-bridge-oled` so the UF2 reflects the project name.

### Acknowledgements

- **[awalol/DS5Dongle](https://github.com/awalol/DS5Dongle)** — upstream base. This fork is a strict superset that tracks upstream and layers add-on features.
- **[zurce/DS5Dongle-OLED](https://github.com/zurce/DS5Dongle-OLED)** — pixel-art icon approach, the "hold for factory reset" UX pattern, and the multi-slot persistent pairing model.

---

[0.5.4]: https://github.com/MarcelineVPQ/DS5Dongle-OLED-Edition/releases/tag/v0.5.4
