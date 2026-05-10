#include <stdio.h>

#include "ns_lib.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "hardware/timer.h"

// Colors can accept RGB Hex values 
const ns_colordata_s colors = {
    .body_r = 0,
    .body_g = 0, 
    .body_b = 0,

    .buttons_r = 0,
    .buttons_g = 0,
    .buttons_b = 0,

    .l_grip_r = 0,
    .l_grip_g = 0,
    .l_grip_b = 0,

    .r_grip_r = 0,
    .r_grip_g = 0,
    .r_grip_b = 0,
};

// Example device mac address
const uint8_t device_mac[6] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6};

int main()
{
    stdio_init_all();
    sleep_ms(1000);
    printf("Pico-W Booted.");

    ns_device_config_s config = {
        .colors = colors,
        .device_mac = device_mac,
        .host_mac = {0}, // Host data is optional
        .gyro_full_scale_dps = 2000, // DPS scale of your gyro sensor if used
        .gyro_rad_per_lsb = 0, // This is set automatically when we init the library
        .transport = NS_TRANSPORT_USB,
        .type = NS_DEVTYPE_PROCON, // NS Device Type
    };

    if(ns_lib_init(&config) == NS_CONFIG_OK)
    {

    }
}

// NS LIB CALLBACK FUNCTIONS
// These need to be implemented by us, the developer
void ns_set_led_cb(int player_leds)
{
    // 0 means LEDs are off here.
    // Otherwise we can use this as 
    // we see fit based on the player
    // number assignment.
}

void ns_set_power_cb(uint8_t shutdown)
{
    // In Bluetooth mode, this is called
    // when the gamepad receives a 'shutdown' event
}

void ns_set_usbpair_cb(ns_usbpair_s pairing_data)
{
    // When we connect the gamepad to the Nintendo Switch
    // via USB connection, after a pairing communication,
    // this function is called which provides the link key 
    // as well as the host mac address for safekeeping
    
    // Accesss with:
    // pairing_data.host_mac[6]
    // pairing_data.link_key[16]
}

void ns_set_imumode_cb(ns_imu_mode_t mode)
{
    // This is called when the gamepad
    // receives a command to change IMU mode
    switch(mode)
    {
        case NS_IMU_OFF:
        break;

        case NS_IMU_RAW:
        break;

        case NS_IMU_QUAT:
        break;
    }
}

void ns_set_haptic_indices_cb(const ns_lib_haptic_raw_sample_s *pairs, uint8_t pair_count)
{
    // Each time that the gamepad receives a valid haptic packet,
    // it is translated into a set of data pairs representing 
    // LOOKUP VALUES for up to 3
    // Hi frequency/Amplitude values, and Lo frequency/Amplitude values.

    float hi_f, lo_f, hi_a, lo_a;

    // These can be combined together by generating two PCM streams
    // utilizing the frequency/amplitude pairs. 
}

void ns_get_powerstatus_cb(ns_powerstatus_s *out)
{
    // This is tells the
    // Nintendo Switch how our 
    // gamepad is powered and
    // the status of our device power
    out->bat_lvl = 4;
    out->charging = 0;
    out->connection = 1;
    out->power_source = 1;
}

void ns_get_inputdata_cb(ns_inputdata_s *out)
{
    // See ns_inputdata_s definition for full
    // Abilities to set these params
    // Ideally we should spend as little
    // time in this function as possible
    // or just fill it with input data 
    // that we've already read!

    out->a = 0;
    out->b = 0;

    // Joystick values are 12 bit unsigned
    out->ls_x = 2048;
    out->ls_y = 2048;

    out->rs_x = 2048;
    out->rs_y = 2048;
}

// HAL Time Function
void ns_get_time_ms(uint64_t *ms)
{
    *ms = time_us_64() / 1000;
}

// IMU SENSOR DATA
void ns_get_imu_standard_cb(ns_gyrodata_s *out)
{

}

void ns_get_imu_quaternion_cb(ns_quaternion_s *out)
{

}
