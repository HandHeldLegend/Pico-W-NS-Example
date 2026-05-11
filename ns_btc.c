#include "main.h"

#include "pico/cyw43_arch.h"
#include "pico/btstack_chipset_cyw43.h"
#include "btstack.h"

#include "ns_lib.h"

#define NS_BTC_COD 0x2508

static const uint32_t _btc_poll_rate_ms = 8;
static uint32_t _btc_last_hid_report_timestamp_ms = 0;
static const char hid_device_name[] = "Wireless Gamepad";
static const char hid_gap_name[] = "Pro Controller";
static bool hid_device_pair_enabled = false;
static btstack_timer_source_t hid_timer;

static inline void _btc_reverse_bytes(const uint8_t *in, uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        out[i] = in[len - 1 - i];
    }
}

/*
 * BTstack persists the link key type alongside the key. Our app-level flash
 * storage only keeps the 16-byte key, so restoring a saved key requires us to
 * pick a type here while testing interoperability with the Switch.
 *
 * Candidates worth trying:
 *   COMBINATION_KEY
 *   UNAUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P192
 *   AUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P192
 *   UNAUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P256
 *   AUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P256
 */
#define NS_BTC_RESTORE_LINK_KEY_TYPE UNAUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P192

typedef struct
{
    uint16_t hid_cid;
} ns_btc_sm;

static uint8_t hid_service_buffer[700] = {0};
static uint8_t pnp_service_buffer[700] = {0};
static btstack_packet_callback_registration_t hci_event_callback_registration;
static uint16_t hid_cid = 0;

static bool _ns_btc_array_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

static void _ns_btstack_debug_print(bd_addr_t addr, link_key_type_t link_key_type, link_key_t link_key)
{
    printf("Address: %s\n", bd_addr_to_str(addr));
    printf("Link key type: %u\n", link_key_type);
    printf("Link key: %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
    link_key[0], link_key[1], link_key[2], link_key[3], link_key[4], link_key[5], link_key[6], link_key[7],
    link_key[8], link_key[9], link_key[10], link_key[11], link_key[12], link_key[13], link_key[14], link_key[15]);
}

// Timer handler to request can send now event after delay
static void _hid_timer_handler(btstack_timer_source_t *ts)
{
    // Avoid compiler warning for unused parameter
    (void)ts;

    // Request another CAN_SEND_NOW event
    hid_device_request_can_send_now_event(hid_cid);
}

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

// Handler for HID OUTPUT reports from the game console/Host device
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
    // Call into our NS API output tunnel handler
    ns_api_output_tunnel(_ns_btc_output_report, report_size + 1);
}


static void _ns_btc_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t packet_size)
{
    UNUSED(channel);
    UNUSED(packet_size);
    uint8_t status;
    if (packet_type == HCI_EVENT_PACKET)
    {
        switch (packet[0])
        {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING)
                return;

            if (!hid_cid)
            {
                // If device pairing is enabled, set to discoverable
                if (hid_device_pair_enabled)
                {
                    gap_discoverable_control(1);
                }
                else
                {
                    // If we have a stored host mac, ensure we have the link key stored
                    // in BTStack memory, then connect if so
                    if(device_storage.magic_byte == NS_STORAGE_MAGIC)
                    {
                        // Check if link key matches
                        link_key_type_t read_type;
                        link_key_t read_key;
                        
                        bool overwrite_key = false;

                        if(gap_get_link_key_for_bd_addr(device_storage.host_mac, read_key, &read_type))
                        {
                            printf("BTStack Stored Link Key:\n");
                            link_key_t read_key_be;
                            _btc_reverse_bytes(read_key, read_key_be, 16);
                            _ns_btstack_debug_print(device_storage.host_mac, read_type, read_key_be);

                            if(!_ns_btc_array_equal(read_key_be, device_storage.link_key, 16))
                            {
                                printf("\nBTStack Link Key mismatches local stored key!\n");
                                overwrite_key = true;
                            }
                        }
                        else
                        {
                            printf("BTStack Link Key for BD ADDR not found!\n");
                            overwrite_key = true;
                        }
                        
                        if(overwrite_key)
                        {
                            printf("\nBTStack Will Now SAVE this Link Key:\n");
                            _ns_btstack_debug_print(device_storage.host_mac, 
                                UNAUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P192, 
                                device_storage.link_key);
                            link_key_t link_key_le;
                            _btc_reverse_bytes(device_storage.link_key, link_key_le, 16);

                            gap_store_link_key_for_bd_addr(device_storage.host_mac, 
                                link_key_le, UNAUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P192);
                        }

                        hid_device_connect(device_storage.host_mac, &hid_cid);
                    }
                    // If we do not have any stored data, enable pairing
                    else
                    {
                        gap_discoverable_control(1);
                    }
                }
            }
            break;

        case HCI_EVENT_LINK_KEY_NOTIFICATION:
            printf("HCI_EVENT_LINK_KEY_NOTIFICATION\n");
            bd_addr_t addr;
            hci_event_link_key_request_get_bd_addr(packet, addr);

            link_key_t link_key_be;
            link_key_t link_key_le;
            link_key_type_t link_key_type = (link_key_type_t)packet[24];

            // cache link key. link keys stored in little-endian format for legacy reasons
            memcpy(link_key_le, &packet[8], 16);

            // Change LE out for BE
            _btc_reverse_bytes(link_key_le, link_key_be, 16);

            _ns_btstack_debug_print(addr, link_key_type, link_key_be);
            
            ns_usbpair_s usbpair = {0};
            memcpy(usbpair.host_mac, addr, 6);
            memcpy(usbpair.link_key, link_key_be, 16);
            ns_set_usbpair_cb(usbpair);
        break;

        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            // ssp: inform about user confirmation request
            printf("SSP User Confirmation Auto accept\n");
            break;

        case HCI_EVENT_HID_META:
            switch (hci_event_hid_meta_get_subevent_code(packet))
            {
            case HID_SUBEVENT_CONNECTION_OPENED:
                status = hid_subevent_connection_opened_get_status(packet);
                if (status)
                {
                    // outgoing connection failed
                    printf("Connection failed, status 0x%x\n", status);
                    gap_discoverable_control(1);
                    hid_cid = 0;
                    return;
                }

                hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
                bd_addr_t addr;
                uint8_t link_key[16] = {0};

                hid_subevent_connection_opened_get_bd_addr(packet, addr);

                printf("HID Connected to: %s\n", bd_addr_to_str(addr));
                hid_device_request_can_send_now_event(hid_cid);

                break;
            case HID_SUBEVENT_CONNECTION_CLOSED:
                printf("HID Disconnected\n");
                break;
            case HID_SUBEVENT_CAN_SEND_NOW:
                if (hid_cid)
                {
                    uint32_t current_time_ms = btstack_run_loop_get_time_ms();
                    uint32_t time_elapsed = current_time_ms - _btc_last_hid_report_timestamp_ms;
                    uint8_t _ns_btc_report_data[64] = {0};

                    if (time_elapsed >= _btc_poll_rate_ms)
                    {   
                        if(ns_api_generate_inputreport(_ns_btc_report_data))
                        {
                            _ns_btc_hid_tunnel(_ns_btc_report_data, 64);
                        }
                        else break;

                        _btc_last_hid_report_timestamp_ms = current_time_ms;
                        hid_device_request_can_send_now_event(hid_cid);
                    }
                    else
                    {
                        // Not enough time has passed, schedule another send event after delay
                        uint32_t time_elapsed = current_time_ms - _btc_last_hid_report_timestamp_ms;
                        uint32_t delay = _btc_poll_rate_ms - time_elapsed;
                        btstack_run_loop_set_timer(&hid_timer, delay);
                        btstack_run_loop_set_timer_handler(&hid_timer, &_hid_timer_handler);
                        btstack_run_loop_add_timer(&hid_timer);
                    }
                }
                break;
            case HID_SUBEVENT_SNIFF_SUBRATING_PARAMS:
            {
                uint16_t max = hid_subevent_sniff_subrating_params_get_host_max_latency(packet);
                uint16_t min = hid_subevent_sniff_subrating_params_get_host_min_timeout(packet);
                printf("Sniff: %d, %d\n", max, min);
            }
            break;

            default:
                break;
            }
            break;
        default:
            break;
        }
    }
}

void ns_btc_enter(uint8_t device_mac[6], bool pairing_mode)
{
    // Init CYW43 Driver
    // If the init fails it returns true lol
    cyw43_arch_init();

    // GAP setup
    gap_set_bondable_mode(1);
    // Gamepad CoD
    gap_set_class_of_device(NS_BTC_COD);
    // Link policy
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH | LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    // Device name (Required for wireless pairing and discovery)
    gap_set_local_name("Pro Controller");
    // Allow role switching
    gap_set_allow_role_switch(true);

    // Set HCI chipset pathway
    hci_set_chipset(btstack_chipset_cyw43_instance());

    // Enable L2CAP via HCI pathway
    l2cap_init();
    // Enable Security Manager for L2CAP
    sm_init();

    // SDP Server
    sdp_init();

    const uint8_t* ns_hid_descriptor = NULL;
    uint16_t ns_hid_descriptor_len = 0;
    uint16_t ns_hid_vid = 0;
    uint16_t ns_hid_pid = 0;

    ns_hid_get_descriptor_params(&ns_hid_descriptor, &ns_hid_descriptor_len, NULL, NULL, &ns_hid_vid, &ns_hid_pid);

    // Craft SDP record
    hid_sdp_record_t hid_sdp_record = {
        .hid_device_subclass = NS_BTC_COD,  // Device Subclass HID
        .hid_country_code = 33,             // Country Code
        .hid_virtual_cable = 1,             // HID Virtual Cable
        .hid_remote_wake = 1,               // HID Remote Wake
        .hid_reconnect_initiate = 1,        // HID Reconnect Initiate
        .hid_normally_connectable = 0,      // HID Normally Connectable
        .hid_boot_device = 0,               // HID Boot Device
        .hid_ssr_host_max_latency = 0xFFFF, // = x * 0.625ms
        .hid_ssr_host_min_timeout = 0xFFFF,
        .hid_supervision_timeout = 3200,                 // HID Supervision Timeout
        .hid_descriptor         = ns_hid_descriptor,     // HID Descriptor
        .hid_descriptor_size    = ns_hid_descriptor_len, // HID Descriptor Length
        .device_name = hid_gap_name};                    // Device Name

    // Register SDP services

    // HID service is required for the Switch to obtain HID descriptors
    memset(hid_service_buffer, 0, sizeof(hid_service_buffer));
    hid_create_sdp_record(hid_service_buffer, sdp_create_service_record_handle(), &hid_sdp_record);
    sdp_register_service(hid_service_buffer);

    // PnP service is required for connecting with Switch 2
    memset(pnp_service_buffer, 0, sizeof(pnp_service_buffer));
    device_id_create_sdp_record(pnp_service_buffer, sdp_create_service_record_handle(), DEVICE_ID_VENDOR_ID_SOURCE_USB,
                                ns_hid_vid, ns_hid_pid, 0x0100);
    sdp_register_service(pnp_service_buffer);

    // Initiate HID device API
    hid_device_init(0, ns_hid_descriptor_len, ns_hid_descriptor);

    // This allows reports with trimmed zeros to still be accepted
    // Required for compatibility with Android and other devices
    hid_device_accept_truncated_hid_reports(true);

    // HCI system events and HID api events are handled
    // using the same event handler callback
    hci_event_callback_registration.callback = &_ns_btc_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);
    hid_device_register_packet_handler(&_ns_btc_packet_handler);

    // When the device receives HID report data, this callback is fired
    hid_device_register_report_data_callback(&_ns_btc_outputreport_handler);

    // Set pairing mode flag
    hid_device_pair_enabled = pairing_mode;

    // Power on HCI path
    hci_power_control(HCI_POWER_ON);
    // Use device MAC as HCI MAC
    hci_set_bd_addr(device_mac);

    // Task loop
    for(;;)
    {
        ns_flash_task();
    }
}
