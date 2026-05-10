#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>
#include <stdbool.h>

void ns_usb_enter(void);
void ns_btc_enter(uint8_t device_mac[6], bool pairing_mode);

#endif
