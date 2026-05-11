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
    uint8_t host_mac[6];
    uint8_t link_key[16]; // Stored here as LE
} ns_storage_s;

#define NS_STORAGE_MAGIC 0xDEADFEED
#define NS_STORAGE_SIZE sizeof(ns_storage_s)
#define NS_STORAGE_PAGE 0

extern const uint8_t device_mac[6];
extern ns_storage_s device_storage;

bool ns_flash_write(uint8_t *data, uint32_t size, uint32_t page);
bool ns_flash_read(uint8_t *out, uint32_t size, uint32_t page);
void ns_flash_task();
void ns_flash_init();
void ns_usb_enter(void);
void ns_btc_enter(uint8_t device_mac[6], bool pairing_mode);

#endif
