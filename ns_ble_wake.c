/**
 * @file ns_ble_wake.c
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

#include "ns_ble_wake.h"

#include "pico/cyw43_arch.h"
#include "pico/btstack_chipset_cyw43.h"
#include "pico/time.h"
#include "btstack.h"
#include "ad_parser.h"

#define NS_BLE_NINTENDO_COMPANY_ID 0x0553
#define NS_BLE_WAKE_TYPE 0x01
#define NS_BLE_WAKE_REPLAY_MS 5500

/* Fixed advertising parameters (same for every controller). */
#define NS_WAKE_ADV_INT_MIN 0x0030u
#define NS_WAKE_ADV_INT_MAX 0x0060u
#define NS_WAKE_ADV_TYPE 0u

static volatile bool _hci_working = false;
static btstack_packet_callback_registration_t _hci_event_callback_registration;

static void _ns_ble_wake_poll_ms(uint32_t ms)
{
    uint32_t start = to_ms_since_boot(get_absolute_time());
    while (to_ms_since_boot(get_absolute_time()) - start < ms) {
        cyw43_arch_poll();
        ns_flash_task();
        sleep_us(1000);
    }
}

static bool _ns_ble_wake_parse_manufacturer(const uint8_t *manu, uint8_t manu_len, uint16_t *pid_out,
                                            uint8_t console_mac[6])
{
    if (manu_len < 17 || pid_out == NULL || console_mac == NULL) return false;

    uint16_t company = (uint16_t)manu[0] | ((uint16_t)manu[1] << 8);
    if (company != NS_BLE_NINTENDO_COMPANY_ID || manu[2] != NS_BLE_WAKE_TYPE) return false;

    *pid_out = (uint16_t)manu[7] | ((uint16_t)manu[8] << 8);
    memcpy(console_mac, &manu[11], 6);
    return true;
}

static bool _ns_ble_wake_packet_valid(uint8_t ad_len, const uint8_t *ad_data)
{
    if (!ad_data || ad_len == 0) return false;

    ad_context_t context;
    for (ad_iterator_init(&context, ad_len, ad_data); ad_iterator_has_more(&context);
         ad_iterator_next(&context)) {
        if (ad_iterator_get_data_type(&context) != BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA) {
            continue;
        }
        uint16_t pid;
        uint8_t console_mac[6];
        if (_ns_ble_wake_parse_manufacturer(ad_iterator_get_data(&context),
                                            ad_iterator_get_data_len(&context), &pid, console_mac)) {
            return true;
        }
    }
    return false;
}

bool ns_ble_wake_stored_valid(const ns_storage_s *storage)
{
    return storage && storage->wake_valid && storage->wake_magic == NS_WAKE_MAGIC;
}

static void _ns_ble_wake_build_ad(const ns_storage_s *storage, uint8_t ad[31])
{
    memset(ad, 0, 31);
    ad[0] = 2;
    ad[1] = BLUETOOTH_DATA_TYPE_FLAGS;
    ad[2] = 0x06;
    ad[3] = 27;
    ad[4] = BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA;
    ad[5] = 0x53;
    ad[6] = 0x05;
    ad[7] = NS_BLE_WAKE_TYPE;
    ad[8] = 0x00;
    ad[9] = 0x03;
    ad[10] = 0x7e;
    ad[11] = 0x05;
    ad[12] = (uint8_t)(storage->wake_product_id & 0xffu);
    ad[13] = (uint8_t)(storage->wake_product_id >> 8);
    ad[14] = 0x00;
    ad[15] = 0x01;
    memcpy(&ad[16], storage->wake_console_mac, 6);
    ad[22]= 0x48;
    ad[23]= 0x0F;
}

static void _ns_ble_wake_save(ns_storage_s *storage, uint8_t addr_type, const uint8_t gamepad_addr[6],
                              uint16_t product_id, const uint8_t console_mac[6])
{
    storage->wake_magic = NS_WAKE_MAGIC;
    storage->wake_valid = 1;
    storage->wake_addr_type = addr_type;
    memcpy(storage->gamepad_addr, gamepad_addr, 6);
    storage->wake_product_id = product_id;
    memcpy(storage->wake_console_mac, console_mac, 6);
    ns_flash_write((uint8_t *)storage, NS_STORAGE_SIZE, NS_STORAGE_PAGE);
}

void ns_ble_wake_replay(const ns_storage_s *storage)
{
    if (!ns_ble_wake_stored_valid(storage)) return;

    static uint8_t ad[31];
    bd_addr_t null_addr = {0};

    _ns_ble_wake_build_ad(storage, ad);
    gap_advertisements_enable(1);
    gap_advertisements_set_params(NS_WAKE_ADV_INT_MIN, NS_WAKE_ADV_INT_MAX, NS_WAKE_ADV_TYPE, 0, null_addr,
                                  0x07, 0x00);
    gap_advertisements_set_data(sizeof(ad), ad);
    gap_scan_response_set_data(0, NULL);
    

    printf("Replaying wake AD for %u ms (PID 0x%04x)...\n", NS_BLE_WAKE_REPLAY_MS,
           storage->wake_product_id);

    _ns_ble_wake_poll_ms(NS_BLE_WAKE_REPLAY_MS);
    gap_advertisements_enable(0);
    _ns_ble_wake_poll_ms(50);

    printf("Wake replay complete.\n");
}

void ns_ble_wake_restore_controller_hci(const uint8_t device_mac[6])
{
    bd_addr_t restore;
    memcpy(restore, device_mac, 6);

    gap_advertisements_enable(0);
    gap_random_address_set_mode(GAP_RANDOM_ADDRESS_TYPE_OFF);

    hci_power_control(HCI_POWER_OFF);
    _ns_ble_wake_poll_ms(200);
    hci_power_control(HCI_POWER_ON);
    hci_set_bd_addr(restore);

    printf("Restored controller MAC: %s\n", bd_addr_to_str(restore));
}

static void _ns_ble_wake_wait_hci_working(void)
{
    uint32_t start = to_ms_since_boot(get_absolute_time());
    while (!_hci_working) {
        ns_flash_task();
        if (to_ms_since_boot(get_absolute_time()) - start > 10000) return;
        _ns_ble_wake_poll_ms(10);
    }
}

static void _ns_ble_stack_bringup(btstack_packet_handler_t handler)
{
    cyw43_arch_init();
    hci_set_chipset(btstack_chipset_cyw43_instance());
    l2cap_init();
    sm_init();

    _hci_working = false;
    _hci_event_callback_registration.callback = handler;
    hci_add_event_handler(&_hci_event_callback_registration);
    hci_power_control(HCI_POWER_ON);
    _ns_ble_wake_wait_hci_working();
}

static void _ns_ble_capture_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet,
                                      uint16_t packet_size)
{
    (void)channel;
    (void)packet_size;

    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            _hci_working = true;
            printf("BLE capture: press HOME on a paired Joy-Con 2 or Pro Controller 2.\n");
            gap_set_scan_parameters(1, 0x0030, 0x0030);
            gap_start_scan();
        }
        break;

    case GAP_EVENT_ADVERTISING_REPORT: {
        uint8_t ad_len = gap_event_advertising_report_get_data_length(packet);
        const uint8_t *ad_data = gap_event_advertising_report_get_data(packet);
        if (!_ns_ble_wake_packet_valid(ad_len, ad_data)) break;

        bd_addr_t gamepad_addr;
        gap_event_advertising_report_get_address(packet, gamepad_addr);
        uint8_t addr_type = gap_event_advertising_report_get_address_type(packet);

        ad_context_t context;
        uint16_t pid = 0;
        uint8_t console_mac[6] = {0};
        for (ad_iterator_init(&context, ad_len, ad_data); ad_iterator_has_more(&context);
             ad_iterator_next(&context)) {
            if (ad_iterator_get_data_type(&context) != BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA) {
                continue;
            }
            if (_ns_ble_wake_parse_manufacturer(ad_iterator_get_data(&context),
                                                ad_iterator_get_data_len(&context), &pid,
                                                console_mac)) {
                break;
            }
        }

        _ns_ble_wake_save(&device_storage, addr_type, gamepad_addr, pid, console_mac);

        printf("\n*** Wake data captured ***\n");
        printf("  Gamepad: %s (type %u)\n", bd_addr_to_str(gamepad_addr), addr_type);
        printf("  Product ID: 0x%04x\n", pid);
        printf("  Console: %02x:%02x:%02x:%02x:%02x:%02x\n", console_mac[0], console_mac[1],
               console_mac[2], console_mac[3], console_mac[4], console_mac[5]);
        printf("Saved. Reboot with GP0 held to wake + reconnect.\n\n");

        gap_stop_scan();
        break;
    }
    default:
        break;
    }
}

void ns_ble_capture_enter(void)
{
    _ns_ble_stack_bringup(&_ns_ble_capture_handler);
    for (;;) {
        ns_flash_task();
    }
}
