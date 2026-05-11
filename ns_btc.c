#include "main.h"
#include "btstack.h"
#include "ns_lib.h"

const uint32_t _btc_poll_rate_ms = 8;
static const char hid_device_name[] = "Wireless Gamepad";

typedef struct
{
    uint16_t hid_cid;
} ns_btc_sm;

static uint8_t hid_service_buffer[700] = {0};
static uint8_t pnp_service_buffer[700] = {0};
static btstack_packet_callback_registration_t hci_event_callback_registration;
static uint16_t hid_cid = 0;

static inline void _ns_btc_hid_tunnel(const void *report, uint16_t len)
{
    uint8_t new_report[66] = {0};
    new_report[0] = 0xA1; // Type of input report

    // Byte 1 is the report ID
    memcpy(&(new_report[1]), report, len);

    if (hid_cid)
    {
        hid_device_send_interrupt_message(hid_cid, new_report, len + 1);
    }
}

uint8_t _ns_btc_output_report_data[64] = {0};
const uint8_t *_ns_btc_output_report = _ns_btc_output_report_data;
static void _ns_btc_outputreport_handler(uint16_t cid,
                                   hid_report_type_t report_type,
                                   uint16_t report_id,
                                   int report_size, uint8_t *report)
{
    if (report_type != HID_REPORT_TYPE_OUTPUT) return;
    if (cid != hid_cid) return;
    if (!report || report_size == 0) return;

    memset(_ns_btc_output_report_data, 0, 64);

    // 1. Set the Report ID as the first byte
    _ns_btc_output_report_data[0] = (uint8_t)report_id;

    if(report_size>63) report_size=63;

    // 2. Copy the actual report data starting at index 1
    // Use report_size, because 'report' points to the payload start
    memcpy(&_ns_btc_output_report_data[1], report, report_size);

    // 3. Total size is now (1 byte ID + report_size)
    ns_api_output_tunnel(_ns_btc_output_report, report_size + 1);
}

void ns_btc_enter(uint8_t device_mac[6], bool pairing_mode)
{

}