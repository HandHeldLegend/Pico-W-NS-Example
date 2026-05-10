#include "main.h"
#include "tusb_config.h"
#include "tusb.h"
#include "ns_lib.h"
#include "ns_lib_hid.h"

const char *global_string_descriptor[] = {
    // array of pointer to string descriptors
    (char[]){0x09, 0x04}, // 0: is supported language is English (0x0409)
    "HHL",                // 1: Manufacturer
    "Gamepad",            // 2: Product
    "000000",             // 3: Serials, should use chip ID
};

volatile static bool _usb_ready = false;
volatile static bool _frame_ready = false;

uint8_t _usb_report_data[64] = {0};

// Our primary NS USB loop entry point
void ns_usb_enter(void)
{
    tusb_init();

    for(;;)
    {
        tud_task();

        if(!_usb_ready)
        {
            _usb_ready = tud_hid_ready();
        }

        if(_usb_ready && _frame_ready)
        {
            _usb_ready = false;
            _frame_ready = false;

            if(ns_api_generate_inputreport(_usb_report_data))
            {
                tud_hid_report(_usb_report_data[0], &_usb_report_data[1], 63);
            }
        }
    }
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;

    const uint8_t *config_descriptor;
    ns_hid_get_descriptor_params(NULL, NULL, &config_descriptor, NULL, NULL, NULL);
    return config_descriptor;
}

// Invoked when received GET DEVICE DESCRIPTOR
// Application return pointer to descriptor
uint8_t const *tud_descriptor_device_cb(void)
{
    const uint8_t* device_descriptor = (uint8_t const *) ns_hid_get_device_descriptor();
    return device_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)reqlen;
    (void)report_type;

    return 0;
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;

    const uint8_t *report_descriptor;
    ns_hid_get_descriptor_params(&report_descriptor, NULL, NULL, NULL, NULL, NULL);

    return report_descriptor;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;

    static uint16_t _desc_str[64];
    uint8_t chr_count;

    if (index == 0)
    {
        memcpy(&_desc_str[1], global_string_descriptor[0], 2);
        chr_count = 1;
    }
    else
    {
        const char *str = global_string_descriptor[index];

        // Cap at max char... WHY?
        chr_count = strlen(str);
        if (chr_count > 31)
            chr_count = 31;

        // Convert ASCII string into UTF-16
        for (uint8_t i = 0; i < chr_count; i++)
        {
            _desc_str[1 + i] = str[i];
        }
    }

    // first byte is length (including header), second byte is string type
    _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * chr_count + 2);
    return _desc_str;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
    if (!report_id && report_type == HID_REPORT_TYPE_OUTPUT)
    {
        // OUTPUT reports must be fed into the API
        ns_api_output_tunnel(buffer, bufsize);
    }
}

// We can enable our sof (START OF FRAME) callback
static bool sofen = false;
void tud_mount_cb()
{
    tud_sof_cb_enable(false);
    tud_sof_cb_enable(true);
    sofen = true;
}

// Here we use the tud_sof_cb to ensure that we wait 8ms between reports
// The Nintendo Switch uses 8ms polling time, and we can sync this with
// the USB SOF packets
void tud_sof_cb(uint32_t frame_count_ext) 
{
    static uint8_t frame_counter = 0;
    frame_counter++;
    if(frame_counter >= 7)
    {
        frame_counter = 0;
        _frame_ready |= true;
    }
}