//
// Created by awalol on 2026/5/4.
//

#ifndef DS5_BRIDGE_CMD_H
#define DS5_BRIDGE_CMD_H

#include <stdint.h>

bool is_pico_cmd(uint8_t report_id);
uint16_t pico_cmd_get(uint8_t report_id, uint8_t *buffer,uint16_t reqlen);
void pico_cmd_set(uint8_t report_id, uint8_t const *buffer,uint16_t bufsize);

// Smoothed RP2350 on-die temperature sensor reading (ADC input 4, 12-bit
// raw). A single sample is very noisy; this averages a large block and runs
// a slow EMA so the value converges to the true die temperature instead of
// chasing per-sample noise. Single source of truth — the OLED CPU screen and
// the 0xfc web telemetry both call this so device and web always agree.
uint16_t cpu_temp_raw_smoothed();

#endif //DS5_BRIDGE_CMD_H
