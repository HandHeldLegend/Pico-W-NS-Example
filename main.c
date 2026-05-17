/**
 * @file main.c
 * @brief Example application entry point and NS-LIB-HID callback implementations.
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

#include "main.h"

#include "ns_ble_wake.h"
#include "ns_lib.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "hardware/timer.h"
#include "pico/rand.h"

static const uint NS_BT_BOOT_PIN = 0;
static const uint NS_BT_PAIR_BOOT_PIN = 1;
static const uint NS_BT_CAPTURE_BOOT_PIN = 2;
static const uint NS_A_BUTTON_PIN = 14;
static const uint NS_B_BUTTON_PIN = 15;

/* Boot mode is chosen once at power-up so the rest of the firmware can stay transport-specific. */
typedef struct
{
    ns_transport_t transport;
    bool pairing_mode;
    bool capture_mode;
} ns_boot_mode_s;

/* Example SPI color data reported by the library when the host queries controller identity. */
const ns_colordata_s colors = {
    .body.hex = 0xFF0000,       // Red
    .buttons.hex = 0x00FF00,    // Green
    .l_grip.hex = 0x0000FF,     // Blue
    .r_grip.hex = 0xFFFFFF,     // White
};

/* Example controller address used for descriptor identity and Bluetooth bring-up. */
const uint8_t device_mac[6] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF2};
ns_storage_s  device_storage = {0};

static void ns_print_debug(uint8_t addr[6], uint8_t link_key[16])
{
    printf("Addr: %02x:%02x:%02x:%02x:%02x:%02x\n",
    addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    printf("Link key: %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
    link_key[0], link_key[1], link_key[2], link_key[3], link_key[4], link_key[5], link_key[6], link_key[7],
    link_key[8], link_key[9], link_key[10], link_key[11], link_key[12], link_key[13], link_key[14], link_key[15]);
}

static ns_boot_mode_s ns_get_boot_mode(void)
{
    /* Default to wired operation so an unstrapped board always boots into a usable mode. */
    ns_boot_mode_s boot_mode = {
        .transport = NS_TRANSPORT_USB,
        .pairing_mode = false,
        .capture_mode = false,
    };

    /* GP0/GP1/GP2 act as mode straps. Button inputs use pull-ups, so all inputs are active-low. */
    gpio_init(NS_BT_CAPTURE_BOOT_PIN);
    gpio_set_dir(NS_BT_CAPTURE_BOOT_PIN, GPIO_IN);
    gpio_pull_up(NS_BT_CAPTURE_BOOT_PIN);

    gpio_init(NS_BT_BOOT_PIN);
    gpio_set_dir(NS_BT_BOOT_PIN, GPIO_IN);
    gpio_pull_up(NS_BT_BOOT_PIN);

    gpio_init(NS_BT_PAIR_BOOT_PIN);
    gpio_set_dir(NS_BT_PAIR_BOOT_PIN, GPIO_IN);
    gpio_pull_up(NS_BT_PAIR_BOOT_PIN);

    gpio_init(NS_A_BUTTON_PIN);
    gpio_set_dir(NS_A_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(NS_A_BUTTON_PIN);

    gpio_init(NS_B_BUTTON_PIN);
    gpio_set_dir(NS_B_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(NS_B_BUTTON_PIN);

    if (!gpio_get(NS_BT_CAPTURE_BOOT_PIN))
    {
        boot_mode.capture_mode = true;
    }
    else if (!gpio_get(NS_BT_PAIR_BOOT_PIN))
    {
        /* Pairing mode implies Bluetooth transport. */
        boot_mode.transport = NS_TRANSPORT_BTC;
        boot_mode.pairing_mode = true;
    }
    else if (!gpio_get(NS_BT_BOOT_PIN))
    {
        /* Reconnect mode uses any stored pairing material already in flash. */
        boot_mode.transport = NS_TRANSPORT_BTC;
    }

    return boot_mode;
}

int main()
{
    ns_boot_mode_s boot_mode = ns_get_boot_mode();
    stdio_init_all();
    sleep_ms(500);
    printf("Pico-W Booted!\n\n");

    ns_flash_init();

    /* Load the example's persisted host MAC and link key, if they were written previously. */
    ns_flash_read((uint8_t*) &device_storage, NS_STORAGE_SIZE, NS_STORAGE_PAGE);

    /*
     * A missing magic value means this sector has never been initialized by the
     * example, so start with a clean settings block before any pairing occurs.
     */
    if(device_storage.magic_byte != NS_STORAGE_MAGIC)
    {
        memset(&device_storage, 0, NS_STORAGE_SIZE);
        device_storage.magic_byte = NS_STORAGE_MAGIC;
    }
    else if (device_storage.wake_magic != NS_WAKE_MAGIC)
    {
        device_storage.wake_valid = 0;
    }

    ns_print_debug(device_storage.host_mac, device_storage.link_key);

    if (boot_mode.capture_mode)
    {
        printf("Entering BLE wake capture mode\n");
        ns_ble_capture_enter();
        return 0;
    }

    ns_device_config_s config = {
        .colors = colors,
        .device_mac = {0},
        .host_mac = {0}, /* Optional, but useful when reconnecting over Bluetooth. */
        .gyro_full_scale_dps = 2000, /* Placeholder IMU full-scale range for future sensor wiring. */
        .gyro_rad_per_lsb = 0, /* Computed by the library during init from gyro_full_scale_dps. */
        .transport = boot_mode.transport,
        .type = NS_DEVTYPE_PROCON, /* Example controller identity exposed to the host. */
    };

    memcpy(config.device_mac, device_mac, 6);
    memcpy(config.host_mac, device_storage.host_mac, 6);

    /* Once the library accepts the config, the rest of the app is just transport-specific plumbing. */
    if(ns_api_init(&config) == NS_CONFIG_OK)
    {
        if (boot_mode.transport == NS_TRANSPORT_BTC)
        {
            printf("Entering Bluetooth mode (pairing %s)\n",
                   boot_mode.pairing_mode ? "on" : "off");
            ns_btc_enter(config.device_mac, boot_mode.pairing_mode);
        }
        else
        {
            printf("Entering USB mode\n");
            ns_usb_enter();
        }
    }
}

/* -------------------------------------------------------------------------- */
/* NS-LIB-HID callback implementations                                         */
/* -------------------------------------------------------------------------- */

/* Platform hook: supply a monotonic millisecond clock for report timers. */
void ns_api_hook_get_time_ms(uint64_t *ms)
{
    *ms = time_us_64() / 1000;
}

/* Platform hook: used by the library when it needs a random byte source. */
uint8_t ns_api_hook_get_random_u8(void)
{
    return (uint8_t) (get_rand_32() & 0xFF);
}

void ns_api_hook_set_haptic_packet_raw(ns_haptics_packet_raw_s *packet)
{
    /*
     * NS-LIB-HID decodes HD-rumble packets into lookup values. The example
     * stops at that decoded representation because it has no actuator driver.
     */
    (void)&packet;

    /* 
    *  A real implementation would convert the decoded pairs into motor drive data. 
    *  ns_api_convert_haptic_packet( ns_haptics_packet_raw_s *in, ns_haptics_packet_processed_s *out )
    */
}

void ns_api_hook_set_led(int player_leds)
{
    /*
     * The host uses this to communicate player index state. The example leaves
     * it empty because there is no LED hardware attached.
     */
    (void)player_leds;
}

void ns_api_hook_set_power(uint8_t shutdown)
{
    /*
     * Bluetooth hosts may request that the controller power down. A product
     * build could latch this into a PMIC or enter a low-power state here.
     */
    (void)shutdown;
}

void ns_api_hook_set_usbpair(ns_usbpair_s pairing_data)
{
    /*
     * Pairing data is one of the few pieces of state that must survive resets.
     * Saving it here allows the Bluetooth path to reconnect later without
     * repeating the full pairing flow every boot.
     */

    memcpy(device_storage.link_key, pairing_data.link_key, 16);
    memcpy(device_storage.host_mac, pairing_data.host_mac, 6);

    /* Flash writes are queued and executed from the transport loops via ns_flash_task(). */
    ns_flash_write((uint8_t *) &device_storage, NS_STORAGE_SIZE, NS_STORAGE_PAGE);
}

void ns_api_hook_get_powerstatus(ns_powerstatus_s *out)
{
    /* Report a simple always-on, externally-powered device to keep the example deterministic. */
    out->bat_lvl = 4;
    out->charging = 0;
    out->connection = 1;
    out->power_source = 1;
}

void ns_api_hook_set_imu_mode(ns_imu_mode_t imu_mode)
{
    /* The host can switch between no IMU, raw IMU, and quaternion-based reporting. */
    switch(imu_mode)
    {
        case NS_IMU_OFF:
        break;

        case NS_IMU_RAW:
        break;

        case NS_IMU_QUAT:
        break;
    }
}

/* IMU hooks are left blank in the example because no sensor is wired in. */
void ns_api_hook_get_imu(ns_gyrodata_s *out)
{
    (void)&out;
}

void ns_api_hook_get_quaternion(ns_quaternion_s *out)
{
    /*
    * In Quatnernion IMU mode, the app is responsible for calling
    * ns_api_motion_update_quaternion( ) as often as you poll your IMU
    * data source. 
    * 
    * Within this hook, you should provide the most recent state 
    * So the API can send the updated data to the host.
    */

    (void)&out;
}

void ns_api_hook_get_input(ns_input_s *out)
{
    /*
     * Keep this path light: the library may call it frequently, so production
     * firmware should ideally copy from already-sampled input state.
     */

    out->a = !gpio_get(NS_A_BUTTON_PIN);
    out->b = !gpio_get(NS_B_BUTTON_PIN);

    /* Center both sticks. Switch stick channels are 12-bit unsigned values. */
    out->ls_x = 2048;
    out->ls_y = 2048;

    out->rs_x = 2048;
    out->rs_y = 2048;
}
