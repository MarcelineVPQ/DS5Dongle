# Bluetooth microphone — SOLVED

**TL;DR:** The DualSense's built-in microphone **works over this dongle's Bluetooth pairing** as of v0.6.8. It is decoded and presented to the host as the standard DualSense USB capture device. Earlier versions of this document concluded the opposite — that it was a Sony-side limitation, probably encrypted, a dead end. **That conclusion was wrong.** The whole thing hinged on a single enable bit. Full credit to **[awalol](https://github.com/awalol/DS5Dongle)** (upstream `mic` branch) for identifying it.

## How it works

1. **Enable bit (dongle → DS5).** In the outbound `0x36` BT audio report, the audio-control sub-report's first flag byte (`pkt[4]`) has **bit 0 = mic-enable**. Setting it (`0b11111110` → `0b11111111`; bits 1–7 were already the speaker/haptic enables) tells the DS5 to start streaming its mic. See `src/audio.cpp`.
2. **Mic frames (DS5 → dongle).** Once enabled, the DS5 tags certain `0x31` BT input reports as mic frames by setting **bit 1 of byte 2** (`(data[2] >> 1) & 1`); the payload is a **71-byte Opus packet at offset +4**. `src/main.cpp:on_bt_data()` routes those to `mic_add_queue()`.
3. **Decode + present.** `src/audio.cpp` Opus-decodes each frame to mono 48 kHz (480-sample / 10 ms frames), duplicates mono → stereo, and `tud_audio_write`s it to the UAC1 capture endpoint. The host sees a normal DualSense mic.
4. **Sticky.** Once the DS5 starts streaming it **keeps going for the rest of the session** even after the enable bit / audio output stops (verified: mic stays live with no further `0x36` frames).
5. **Always-on.** Because the enable normally only rides the audio-gated `0x36` frames, the dongle sends a **control-only `0x36` keep-alive** (enable bit + `SetStateData` + silent haptic, no speaker payload) at ~4 Hz *only until mic frames start arriving*, then stops (sticky takes over). So the mic works with no game audio playing, at minimal extra BT traffic. See `mic_enable_keepalive()` in `src/audio.cpp`.
6. **Toggle.** Gated by `Config_body.bt_mic_enable` (default on) — OLED **Settings → BT Mic** and the web config tool. Off = no enable sent, no keep-alive, and inbound mic frames are not routed (so it's off host-side even if a previously-enabled DS5 is still streaming). Off by toggle saves DS5 battery (always-on keeps its audio subsystem awake).

## Why the original conclusion was wrong (the lesson)

The earlier investigation ported awalol's *receive* side (the `(data[2]>>1)&1` trigger + Opus decode) and then watched for that bit — but **never sent the enable bit**, because this fork's `audio.cpp` had diverged (the pre-`3a31bd7` SetStateData revert) and sat at `pkt[4] = 0b11111110`. With nothing telling the DS5 to start, it never streamed, so bit 1 of byte 2 never set — which got misread as "the trigger never fires → the channel must be encrypted / it's a dead end."

It was never encrypted. It was one un-set bit on the *transmit* side. Lesson: when porting a two-sided protocol, confirm **both** halves (enable *and* receive) before concluding the device "can't" do something.

## What we use (host-side)

- **`scripts/mic_diag.sh`** — `status` / `capture [secs]` / `watch` / `bt-trace`. `capture` arecords the DualSense mic card and reports peak/RMS/non-zero, the fastest way to confirm real audio.
- The DualSense capture card's **`Headset` capture control defaults low** — raise it (`amixer -c <card> sset 'Headset' 90%`) or captures look silent.
- **OLED Diagnostics** `Mic in:` (~100/s when streaming) + `Mic dec=` (480 = good Opus decode) are the on-device confirmation.
- Vendor HID feature reports `0xFD` / `0xFE` and the BT counters remain useful general audio-debug infra.

## Open follow-ups

- **No documented "stop" command.** Disabling mid-session relies on gating the receive side; the DS5 keeps streaming until reconnect. If a real stop/disable bit is found, wire it into the toggle to stop the DS5-side battery drain immediately.
- **Mono only.** Decoded mono is duplicated to the stereo endpoint; the DS5 mic is mono so this is fine, but the descriptor could be made truly mono (as awalol's branch does) to halve endpoint bandwidth.
- **Name the `pkt[4]` bits precisely.** `daidr/dualsense-tester`'s `outputStruct.ts` documents the *standard* output report flags but not the `0x36` BT-audio sub-report; the bit meanings beyond bit 0 are inferred from the working speaker/haptic path.

## References

- Upstream `awalol/DS5Dongle` branch `mic` (commits `9c197fc`, `3829163`) — the source of the enable bit (`pkt[4]` bit 0) and the receive-side decode. awalol confirmed bit 0 is the key.
- Linux kernel `drivers/hid/hid-playstation.c` (~line 1509, *"Bluetooth audio is currently not supported"*) — still true for the kernel driver; not for this dongle.
- `daidr/dualsense-tester` — `src/router/DualSense/views/_OutputPanel/outputStruct.ts` for the standard output-report flag layout.
