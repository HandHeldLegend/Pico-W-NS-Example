/**
 * @file main.h
 * @brief Shared declarations for the Pico W example application.
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

#ifndef MAIN_H
#define MAIN_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    uint32_t magic_byte;
    /* Paired host address saved so Bluetooth reconnect can target the last console. */
    uint8_t host_mac[6];
    /* Stored in big-endian display order to match the example's debug output and flash dumps. */
    uint8_t link_key[16];
    /* Minimal Switch 2 wake data (advertising params are fixed in firmware). */
    uint32_t wake_magic;
    uint8_t wake_valid;
    uint8_t wake_addr_type;
    uint8_t gamepad_addr[6];
    uint16_t wake_product_id;
    uint8_t wake_console_mac[6];
} ns_storage_s;

/* Simple single-page settings block used by the example's flash helper. */
#define NS_STORAGE_MAGIC 0xDEADFEED
#define NS_STORAGE_SIZE sizeof(ns_storage_s)
#define NS_STORAGE_PAGE 0

/* Device identity and pairing storage owned by main.c. */
extern const uint8_t device_mac[6];
extern ns_storage_s device_storage;

/* Flash persistence helpers shared by both USB and Bluetooth loops. */
bool ns_flash_write(uint8_t *data, uint32_t size, uint32_t page);
bool ns_flash_read(uint8_t *out, uint32_t size, uint32_t page);
void ns_flash_task();
void ns_flash_init();

/* Transport entry points selected at boot. */
void ns_usb_enter(void);
void ns_btc_enter(uint8_t device_mac[6], bool pairing_mode);
#endif
