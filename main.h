#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

bool ns_flash_write(uint8_t *data, uint32_t size, uint32_t page);
bool ns_flash_read(uint8_t *out, uint32_t size, uint32_t page);
void ns_flash_task();
void ns_flash_init();
void ns_usb_enter(void);
void ns_btc_enter(uint8_t device_mac[6], bool pairing_mode);

#endif
