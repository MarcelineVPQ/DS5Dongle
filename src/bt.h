//
// Created by awalol on 2026/3/4.
//

#ifndef DS5_BRIDGE_BT_H
#define DS5_BRIDGE_BT_H

#include <cstdint>
#include <vector>

enum CHANNEL_TYPE {
    INTERRUPT,
    CONTROL
};

typedef void (*bt_data_callback_t)(CHANNEL_TYPE channel, uint8_t *data, uint16_t len);

int bt_init();
void bt_register_data_callback(bt_data_callback_t callback);
void bt_send_packet(uint8_t *data, uint16_t len);
void bt_send_control(uint8_t *data, uint16_t len);
void bt_write(const uint8_t *data, uint16_t len);
void bt_get_signal_strength(int8_t *rssi);
std::vector<uint8_t> get_feature_data(uint8_t reportId,uint16_t len);
void init_feature();
void set_feature_data(uint8_t reportId, uint8_t* data,uint16_t len);

// Connection-attempt watchdog: call once per main-loop iteration. Recovers a
// stalled connection (auto re-inquiry) so a transient RF glitch — e.g. USB 3.0
// 2.4 GHz interference — doesn't hang the dongle on the amber lightbar.
void bt_connection_watchdog_tick();

// OLED add-on accessors.
bool bt_is_connected();
void bt_get_addr(uint8_t out[6]);
uint32_t bt_hci_err_count();

// Multi-slot persistent pairing (Phase G). Modeled on zurce/DS5Dongle-OLED.
int  bt_get_slot();
void bt_set_slot(int slot);
void bt_forget_slot(int slot);
void bt_wipe_all_slots();
bool bt_slot_occupied(int slot);
void bt_slot_get_addr(int slot, uint8_t out[6]);

#endif //DS5_BRIDGE_BT_H