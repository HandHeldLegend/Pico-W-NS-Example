/**
 * @file ns_ble_wake.h
 * @brief Switch 2 BLE wake capture and replay.
 *
 * Author: Mitchell Cairns
 * Copyright (c) 2026 Hand Held Legend, LLC.
 *
 * Licensed under the Creative Commons Attribution-NonCommercial 4.0 International
 * License (CC BY-NC 4.0). Non-commercial use with attribution; commercial use
 * requires permission from Hand Held Legend, LLC. Licensing inquiries:
 * support@handheldlegend.com
 * Full terms: https://creativecommons.org/licenses/by-nc/4.0/legalcode
 *
 * SPDX-License-Identifier: CC-BY-NC-4.0
 */

#ifndef NS_BLE_WAKE_H
#define NS_BLE_WAKE_H

#include "main.h"

#define NS_WAKE_MAGIC 0x57414B45

void ns_ble_capture_enter(void);
void ns_ble_wake_replay(const ns_storage_s *storage);
void ns_ble_wake_restore_controller_hci(const uint8_t device_mac[6]);
bool ns_ble_wake_stored_valid(const ns_storage_s *storage);

#endif
