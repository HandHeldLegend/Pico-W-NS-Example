/**
 * @file ns_wlan.c
 * @brief HOJA WLAN dongle transport (Switch gamepad mode) for the Pico W example.
 *
 * This file is now a thin PLATFORM ADAPTER for HOJA-LIB-DONGLE. The dongle
 * gamepad protocol (the respond-only state machine, session/WAKE handling,
 * status apply, reliable-lane dedup, and input replies) lives in the
 * platform-agnostic library (external/HOJA-LIB-DONGLE/dongle_gamepad.c). This
 * file only provides the platform "hooks" the library calls into:
 *
 *   - radio bring-up / bind (done once in ns_wlan_enter)
 *   - dongle_api_gamepad_hook_connect_async  -> CYW43 STA association
 *   - dongle_api_gamepad_hook_link_up        -> CYW43 link status
 *   - dongle_api_gamepad_hook_apply_static_ip-> pin the gamepad IP via lwIP
 *   - dongle_api_gamepad_hook_udp_tx         -> lwIP UDP send (the ONLY TX path)
 *   - dongle_api_gamepad_hook_get_inputreport-> NS-LIB-HID input report
 *   - dongle_api_gamepad_hook_set_outputreport-> NS-LIB-HID host OUT tunnel
 *   - dongle_api_gamepad_hook_set_{player,transport,rumble}
 *   - dongle_api_gamepad_hook_reset_network  -> full radio cycle on link loss
 *   - dongle_api_hook_get_time_us_u64 / get_rand_u16 -> platform primitives
 *
 * The RX path stays here too: the lwIP receive callback validates the datagram
 * size and feeds it to dongle_api_gamepad_udp_rx(), which processes it inline
 * (one reply per datagram via the udp_tx hook).
 *
 * The single most important protocol rule is enforced by the library: the
 * gamepad is purely reactive. We bind UDP, wait for the dongle to transmit,
 * and emit exactly one datagram in response to each datagram we receive. There
 * is no local timer, keepalive, or background input stream.
 *
 * Addressing: the dongle's RX filter only accepts UDP from the gamepad address
 * 192.168.4.16. The dongle DHCP server is supposed to reserve that lease for
 * us, but it can hand out a different address. The apply_static_ip hook keeps
 * the DHCP lease when it is already the gamepad address; otherwise it stops
 * DHCP and pins the netif to that address so our source IP is deterministic.
 *
 * In Switch mode the dongle exposes a Nintendo Switch / NS HID USB device to
 * the console and tunnels the Switch protocol to us:
 *   - host OUT reports arrive as DONGLE_PID_CORE_RELIABLE payloads, which the
 *     library hands to set_outputreport() -> ns_api_output_tunnel().
 *   - we answer with the 64-byte Switch input report produced by
 *     ns_api_generate_inputreport(), supplied via get_inputreport() and carried
 *     in DONGLE_PID_CORE_UNRELIABLE.
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
#include <ns_lib.h>

#include "pico/cyw43_arch.h"
#include "pico/rand.h"
#include "pico/time.h"

#include "lwip/udp.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"

#include "ns_lib.h"
#include "dongle.h"
#include "dongle_gamepad.h"

/* -------------------------------------------------------------------------- */
/* Platform configuration.                                                    */
/* SSID / password / IP layout now come from the library (DONGLE_DEFAULT_*    */
/* and DONGLE_*_IP*); only the CYW43 auth mode and the USB personality we     */
/* advertise are platform/example choices.                                    */
/* -------------------------------------------------------------------------- */
#define NS_WLAN_AUTH        CYW43_AUTH_WPA2_AES_PSK

/*
 * USB personality we want the dongle to expose to the console. The dongle
 * applies vid/pid from our WAKE reply when it brings up its Switch USB core.
 */
#define NS_WLAN_DONGLE_MODE DONGLE_MODE_SWITCH

/* The Switch steady-state input report id; everything else is reliable. */
#define NS_WLAN_SWITCH_FULL_REPORT_ID 0x30

/* -------------------------------------------------------------------------- */
/* Adapter state.                                                             */
/* -------------------------------------------------------------------------- */
static struct udp_pcb *_wlan_pcb = NULL;

/* Scratch RX packet reused by the lwIP receive callback. */
static dongle_pkt_s _rx_pkt;

/* Forward declaration: used by the network-reset hook to re-bind. */
static bool _ns_wlan_bind(void);

/* -------------------------------------------------------------------------- */
/* Generic utility hooks (dongle.h)                                           */
/* -------------------------------------------------------------------------- */

uint16_t dongle_api_hook_get_rand_u16(void)
{
    return (uint16_t)(get_rand_32() & 0xFFFF);
}

uint64_t dongle_api_hook_get_time_us_u64(void)
{
    return time_us_64();
}

/* -------------------------------------------------------------------------- */
/* Gamepad hooks (dongle_gamepad.h)                                           */
/* -------------------------------------------------------------------------- */

/* Map the CYW43 station link status onto the library's link enum. */
dongle_link_status_t dongle_api_gamepad_hook_link_up(void)
{
    int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    return (link == CYW43_LINK_UP) ? DONGLE_LINK_UP : DONGLE_LINK_DOWN;
}

/* Start a non-blocking join to the dongle AP. Completion (static IP) is driven
 * by the library poll via dongle_api_gamepad_hook_apply_static_ip(). */
void dongle_api_gamepad_hook_connect_async(const char *ssid, const char *pw)
{
    cyw43_arch_enable_sta_mode();
    cyw43_wifi_set_roam_enabled(&cyw43_state, false);
    cyw43_wifi_set_interference_mode(&cyw43_state, CYW43_IFMODE_NONE);

    printf("[WLAN] Station mode enabled, joining SSID \"%s\"\n", ssid);

    int rc = cyw43_arch_wifi_connect_async(ssid, pw, NS_WLAN_AUTH);
    if (rc != 0)
    {
        printf("[WLAN] connect_async failed (rc=%d)\n", rc);
    }
}

/*
 * Ensure the station uses the gamepad address the dongle filters on. lwIP /
 * cyw43_arch run DHCP on link-up. dhcp_stop() always clears the netif, so we
 * re-apply the target address afterward (a no-op when DHCP already offered it).
 * Returns true once the interface holds the gamepad address.
 */
bool dongle_api_gamepad_hook_apply_static_ip(uint8_t addr[4], uint8_t mask[4], uint8_t gateway[4])
{
    ip4_addr_t target, m, gw;
    IP4_ADDR(&target, addr[0], addr[1], addr[2], addr[3]);
    IP4_ADDR(&m, mask[0], mask[1], mask[2], mask[3]);
    IP4_ADDR(&gw, gateway[0], gateway[1], gateway[2], gateway[3]);

    struct netif *nif = netif_default;
    if (nif == NULL)
    {
        return false;
    }

    const ip4_addr_t *current = netif_ip4_addr(nif);
    bool dhcp_had_target = ip4_addr_cmp(current, &target);

    if (!dhcp_had_target)
    {
        printf("[WLAN] Associated (DHCP offered %s), pinning gamepad IP\n", ip4addr_ntoa(current));
    }

    /*
     * dhcp_stop() sends RELEASE and clears the netif to 0.0.0.0, so we must
     * always re-apply the gamepad address afterward even when DHCP already
     * offered the correct lease.
     */
    dhcp_stop(nif);
    netif_set_addr(nif, &target, &m, &gw);

    /* Disable Wi-Fi power management for deterministic, low-latency polling. */
    cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);

    if (dhcp_had_target)
    {
        printf("[WLAN] Associated (kept DHCP gamepad IP %s)\n", ip4addr_ntoa(netif_ip4_addr(nif)));
    }
    else
    {
        printf("[WLAN] Using static gamepad IP %s (gw %s)\n",
               ip4addr_ntoa(netif_ip4_addr(nif)), ip4addr_ntoa(&gw));
    }

    return true;
}

/* Transmit exactly one fully-formed dongle_pkt_s. This is the ONLY place WLAN
 * traffic ever leaves the gamepad, and it is only reachable from the library's
 * packet processing, which runs inside the lwIP receive callback. Because that
 * is the lwIP/background context, the udp_sendto below needs no extra locking. */
void dongle_api_gamepad_hook_udp_tx(const dongle_pkt_s *pkt, uint8_t ip[4], uint16_t port)
{
    if (_wlan_pcb == NULL || pkt == NULL)
    {
        return;
    }

    /* Always transmit the full fixed structure; bytes past len stay zeroed. */
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(dongle_pkt_s), PBUF_RAM);
    if (p == NULL)
    {
        printf("[WLAN] TX drop: pbuf_alloc failed\n");
        return;
    }

    memcpy(p->payload, pkt, sizeof(dongle_pkt_s));

    ip_addr_t dst;
    IP4_ADDR(ip_2_ip4(&dst), ip[0], ip[1], ip[2], ip[3]);
    IP_SET_TYPE(&dst, IPADDR_TYPE_V4);

    err_t err = udp_sendto(_wlan_pcb, p, &dst, port);

    pbuf_free(p);

    if (err != ERR_OK)
    {
        printf("[WLAN] TX error: udp_sendto -> %d\n", (int)err);
    }
}

/* Provide the latest Switch input report from NS-LIB-HID. The whole 64-byte
 * report (report id at byte 0) is the payload the dongle relays to the console.
 * Steady-state full reports (id 0x30) ride the unreliable lane; anything else
 * (command replies) rides the reliable lane. */
bool dongle_api_gamepad_hook_get_inputreport(uint8_t data[64], uint16_t *len, bool *reliable)
{
    uint8_t report[64] = {0};

    if (!ns_api_generate_inputreport(report))
    {
        return false;
    }

    memcpy(data, report, 64);
    *len = 64;
    *reliable = (report[0] != NS_WLAN_SWITCH_FULL_REPORT_ID);
    return true;
}

/* Host OUT (Switch commands / rumble). Feed it straight into NS-LIB-HID. */
void dongle_api_gamepad_hook_set_outputreport(const uint8_t data[64], uint16_t len)
{
    ns_api_output_tunnel(data, len);
}

void dongle_api_gamepad_hook_set_player(uint8_t player_number)
{
    /* No player LED hardware in this example; log the assignment for visibility. */
    printf("[WLAN] Player number %u\n", player_number);
}

void dongle_api_gamepad_hook_set_transport(bool connected)
{
    printf("[WLAN] Console transport %s\n", connected ? "CONNECTED" : "IDLE");
}

void dongle_api_gamepad_hook_set_rumble(uint8_t left, uint8_t right, uint8_t left_brake, uint8_t right_brake)
{
    /*
     * No physical actuator is wired into this example, so the rumble/brake
     * amplitudes are simply discarded. A real product would drive its motors
     * here.
     */
    (void)left;
    (void)right;
    (void)left_brake;
    (void)right_brake;
}

/* Full radio cycle after the library detects link loss / connect failure.
 * Pairs the teardown + bring-up so the next connect_async starts clean. The
 * library re-drives association after this returns (its poll calls
 * connect_async again while the link reads DOWN). */
void dongle_api_gamepad_hook_reset_network(void)
{
    printf("[WLAN] Reset requested, reinitializing radio\n");

    if (_wlan_pcb != NULL)
    {
        udp_remove(_wlan_pcb);
        _wlan_pcb = NULL;
    }
    cyw43_arch_deinit();

    while (cyw43_arch_init())
    {
        printf("[WLAN] Re-init failed, retrying in 1s\n");
        sleep_ms(1000);
    }

    while (!_ns_wlan_bind())
    {
        printf("[WLAN] Re-bind failed, retrying in 1s\n");
        sleep_ms(1000);
    }
}

/* -------------------------------------------------------------------------- */
/* RX path                                                                    */
/* -------------------------------------------------------------------------- */

/* lwIP receive callback. Runs in the cyw43 background context. Validate the
 * datagram length and feed it to the library, which processes it inline and
 * emits at most one reply through dongle_api_gamepad_hook_udp_tx(). */
static void _ns_wlan_udp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                              const ip_addr_t *addr, u16_t port)
{
    (void)arg;
    (void)pcb;
    (void)addr;
    (void)port;

    if (p == NULL)
    {
        return;
    }

    /* The dongle only ever sends a full, fixed-size dongle_pkt_s. Anything else
     * is not part of the protocol, so drop it. */
    if (p->tot_len != sizeof(dongle_pkt_s))
    {
        printf("[WLAN] RX drop: unexpected length %u (want %u)\n",
               p->tot_len, (unsigned)sizeof(dongle_pkt_s));
        pbuf_free(p);
        return;
    }

    pbuf_copy_partial(p, &_rx_pkt, sizeof(dongle_pkt_s), 0);
    pbuf_free(p);

    dongle_api_gamepad_udp_rx(&_rx_pkt);
}

/* Bind UDP port 4444 and register the receive callback. */
static bool _ns_wlan_bind(void)
{
    _wlan_pcb = udp_new();
    if (_wlan_pcb == NULL)
    {
        printf("[WLAN] udp_new() failed\n");
        return false;
    }

    err_t err = udp_bind(_wlan_pcb, IP_ANY_TYPE, DONGLE_WLAN_PORT);
    if (err != ERR_OK)
    {
        printf("[WLAN] udp_bind(%u) failed -> %d\n", DONGLE_WLAN_PORT, (int)err);
        udp_remove(_wlan_pcb);
        _wlan_pcb = NULL;
        return false;
    }

    udp_recv(_wlan_pcb, _ns_wlan_udp_recv, NULL);
    printf("[WLAN] UDP bound on port %u, waiting for dongle (no proactive TX)\n",
           DONGLE_WLAN_PORT);
    return true;
}

/* -------------------------------------------------------------------------- */
/* Entry point                                                                */
/* -------------------------------------------------------------------------- */

void ns_wlan_enter(void)
{
    printf("[WLAN] Entering HOJA dongle Switch mode\n");

    /* USB identity to advertise to the dongle comes from NS-LIB-HID. The
     * library was configured with NS_TRANSPORT_USB so this lookup succeeds. */
    uint16_t vid = 0;
    uint16_t pid = 0;
    if (!ns_hid_get_descriptor_params(NULL, NULL, NULL, NULL, &vid, &pid))
    {
        printf("[WLAN] WARNING: could not read NS-LIB vid/pid; using 0/0\n");
    }

    /* Platform owns radio bring-up + UDP bind (done once). The dongle library
     * orchestrates (re)association from here via the connect/IP hooks. */
    while (cyw43_arch_init())
    {
        printf("[WLAN] cyw43_arch_init() failed, retrying in 1s\n");
        sleep_ms(1000);
    }

    while (!_ns_wlan_bind())
    {
        printf("[WLAN] Bind failed, retrying in 1s\n");
        sleep_ms(1000);
    }

    /* Hand the personality to the library and pick the first session id.
     * Event subscriptions decide which STATUS-derived callbacks fire: this
     * example logs transport/player changes but has no haptic actuator, so it
     * opts out of rumble events (the set_rumble hook is never invoked). */
    dongle_cfg_gamepad_s cfg = {0};
    cfg.mode = NS_WLAN_DONGLE_MODE;
    cfg.vid  = vid;
    cfg.pid  = pid;
    cfg.evt.transport_status = true;
    cfg.evt.player_number    = true;
    cfg.evt.rumble           = false;
    dongle_api_gamepad_wlan_init(&cfg);

    /*
     * Reactive loop: the UDP receive callback feeds the library (one reply per
     * datagram); here we service deferred flash writes and let the library
     * drive the connection state machine (link poll, reconnect, static IP).
     */
    for (;;)
    {
        ns_flash_task();

        dongle_api_gamepad_wlan_task();

        sleep_ms(1);
    }
}
