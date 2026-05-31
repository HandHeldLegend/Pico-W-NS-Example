/**
 * @file ns_wlan.c
 * @brief HOJA WLAN dongle transport (Switch gamepad mode) for the Pico W example.
 *
 * This transport joins the HOJA dongle's private Wi-Fi AP as a station and
 * speaks the dongle's fixed-size UDP protocol (see
 * external/HOJA-LIB-DONGLE/docs/GAMEPAD_IMPLEMENTATION.md).
 *
 * The single most important protocol rule is implemented here literally: the
 * gamepad is purely reactive. We bind UDP, wait for the dongle to transmit,
 * and emit exactly one datagram in response to each datagram we receive. There
 * is no local timer, keepalive, or background input stream.
 *
 * To absorb bursts, the UDP receive callback does not process datagrams. With
 * the threadsafe-background cyw43_arch it runs in the network background
 * context, so it only validates and copies each datagram into a fixed-size
 * SPSC FIFO. The main loop drains that FIFO and does all protocol work (and
 * the single reply per datagram) on one predictable context.
 *
 * Addressing: the dongle's RX filter only accepts UDP from the gamepad address
 * 192.168.4.16. The dongle DHCP server is supposed to reserve that lease for
 * us, but it can hand out a different address (e.g. 192.168.4.17) if a stale
 * lease exists. To stay inside the dongle's filter, this transport does NOT
 * rely on the DHCP-assigned address: after association it pins the netif to the
 * fixed gamepad address (DONGLE_GAMEPAD_IP*) so our source IP is deterministic.
 *
 * In Switch mode the dongle exposes a Nintendo Switch / NS HID USB device to
 * the console and tunnels the Switch protocol to us:
 *   - host OUT reports arrive as DONGLE_PID_CORE_RELIABLE payloads, which we
 *     feed straight into ns_api_output_tunnel().
 *   - we answer with the 64-byte Switch input report produced by
 *     ns_api_generate_inputreport(), carried in DONGLE_PID_CORE_UNRELIABLE.
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

/*
 * Compile-time guard from the guide's bring-up checklist (§14): with the
 * mandatory #pragma pack(push,1) the wire struct is exactly 71 bytes
 * (2 + 2 + 1 + 2 + 64). If padding sneaks in, the dongle's length filter
 * silently drops every datagram we send.
 */
_Static_assert(sizeof(dongle_pkt_s) == 71, "dongle_pkt_s must be packed to 71 bytes");
_Static_assert(sizeof(dongle_status_u) == 8, "dongle_status_u must be 8 bytes");

/* -------------------------------------------------------------------------- */
/* Dongle AP credentials and endpoints.                                       */
/* These mirror §1 of the dongle gamepad implementation guide. Update them    */
/* here if the dongle firmware changes its SSID / password / IP layout.       */
/* -------------------------------------------------------------------------- */
#define NS_WLAN_SSID            "HOJA_WLAN_1234"
#define NS_WLAN_PASSWORD        "HOJA_1234"
#define NS_WLAN_AUTH            CYW43_AUTH_WPA2_AES_PSK

/* The dongle (AP) lives at 192.168.4.1 and listens on DONGLE_WLAN_PORT. */
#define NS_WLAN_DONGLE_IP0      192
#define NS_WLAN_DONGLE_IP1      168
#define NS_WLAN_DONGLE_IP2      4
#define NS_WLAN_DONGLE_IP3      1

/*
 * Our own (gamepad) address. The dongle filters on exactly this source IP, so
 * we assign it statically from the DONGLE_GAMEPAD_IP* constants rather than
 * accepting whatever DHCP hands out. The gateway is the dongle AP itself.
 */
#define NS_WLAN_NETMASK0        255
#define NS_WLAN_NETMASK1        255
#define NS_WLAN_NETMASK2        255
#define NS_WLAN_NETMASK3        0

/* How long to wait for a single association attempt before retrying. */
#define NS_WLAN_CONNECT_TIMEOUT_MS  20000

/*
 * USB personality we want the dongle to expose to the console. The dongle
 * applies vid/pid from our WAKE reply when it brings up its Switch USB core.
 */
#define NS_WLAN_DONGLE_MODE     DONGLE_MODE_SWITCH

/* -------------------------------------------------------------------------- */
/* Session / link state (the "persistent variables" from §13 of the guide).   */
/* -------------------------------------------------------------------------- */
static struct udp_pcb *_wlan_pcb = NULL;
static ip_addr_t        _wlan_dongle_addr;

/* Current logical session: USB mode (4 bits) + random session id (12 bits). */
static dongle_session_s _wlan_session = {0};

/* WAKE body we advertise; cached so re-sends stay byte-identical for dedupe. */
static dongle_wake_s    _wlan_wake = {0};

/* Last status snapshot delivered by the dongle (rumble / brake / player / link). */
static dongle_status_u  _wlan_status = {0};

/* True once the dongle has started polling us (WLAN link, dongle side up). */
static volatile bool    _wlan_link_up = false;

/* True while the console side is enumerated (transport_status CONNECTED). */
static bool             _wlan_transport_connected = false;

/* True once we have replied to a WAKE beacon and not yet seen any other valid
 * packet type. The dongle re-broadcasts WAKE beacons rapidly until the link is
 * established; replying to every one spams a fresh input/session burst, so we
 * answer the first beacon only and stay silent for repeats. Receiving any other
 * valid packet (STATUS / CORE_RELIABLE) means the dongle has moved on, so we
 * clear this and will answer the next WAKE beacon again. */
static bool             _wlan_wake_replied = false;

/* USB identity advertised to the dongle, pulled from NS-LIB-HID at startup. */
static uint16_t         _wlan_vid = 0;
static uint16_t         _wlan_pid = 0;

/* Last CORE_RELIABLE ack token whose host-OUT payload we tunneled into NS-LIB.
 * The dongle uses stop-and-wait: it resends the same CORE_RELIABLE packet (with
 * an identical ack token) until it sees that token echoed back. A given host OUT
 * must be tunneled exactly once; any resend carrying an ack we have already
 * processed is a duplicate and re-queuing it would emit duplicate command
 * replies. The companion bool distinguishes "no reliable payload seen yet" from
 * a legitimately processed ack of 0. Both are cleared on link loss. */
static uint16_t         _wlan_last_reliable_ack = 0;
static bool             _wlan_have_reliable_ack = false;

/* -------------------------------------------------------------------------- */
/* RX FIFO (single-producer / single-consumer ring buffer).                   */
/*                                                                            */
/* With pico_cyw43_arch_lwip_threadsafe_background the UDP receive callback   */
/* runs in the cyw43 background (IRQ) context and can preempt the main loop.  */
/* To absorb bursts of datagrams and keep all protocol work on one context,   */
/* the callback only copies each datagram into this queue (producer) and the  */
/* main loop drains and processes it (consumer).                              */
/*                                                                            */
/* This is a classic lock-free SPSC ring: the producer owns _wlan_rx_head,    */
/* the consumer owns _wlan_rx_tail, and one slot is always left empty to tell */
/* "full" from "empty". Word-sized index loads/stores are atomic on the core, */
/* so no locking is needed as long as there is exactly one of each. The queue */
/* holds NS_WLAN_RX_QUEUE_LEN slots, i.e. NS_WLAN_RX_QUEUE_LEN - 1 usable.    */
/* -------------------------------------------------------------------------- */
#define NS_WLAN_RX_QUEUE_LEN 32

static dongle_pkt_s     _wlan_rx_queue[NS_WLAN_RX_QUEUE_LEN];
static volatile uint32_t _wlan_rx_head = 0;     /* written only by the RX callback (producer). */
static volatile uint32_t _wlan_rx_tail = 0;     /* written only by the main loop (consumer).   */
static volatile uint32_t _wlan_rx_dropped = 0;  /* datagrams discarded because the queue was full. */

/* -------------------------------------------------------------------------- */
/* Throughput stats.                                                          */
/*                                                                            */
/* The dongle polls at the report rate, so logging per packet floods the      */
/* terminal. Instead we count the unreliable input reports we emit and print  */
/* a single summary line once per second from the main loop. These counters   */
/* are only touched on the main-loop context (the RX drain and the stats      */
/* print both run there), so no synchronization is needed.                    */
/* -------------------------------------------------------------------------- */
#define NS_WLAN_STATS_PERIOD_MS 1000

static uint32_t _wlan_stat_input_reports = 0;   /* unreliable reports sent this interval. */
static uint32_t _wlan_stat_last_ms       = 0;   /* timestamp of the last stats print.      */
static uint32_t _wlan_stat_last_dropped  = 0;   /* _wlan_rx_dropped at the last print.     */

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

/* Pick a fresh 12-bit session id (1..0xFFF). A new id tells the dongle that a
 * new client attached or the gamepad rebooted, forcing a clean core_init. */
static uint16_t _ns_wlan_new_session_id(void)
{
    uint16_t sid = (uint16_t)(get_rand_32() & 0x0FFFu);
    if (sid == 0)
    {
        sid = 1;
    }
    return sid;
}

/* Refresh the cached WAKE body for the current mode / session / USB identity. */
static void _ns_wlan_refresh_wake(void)
{
    _wlan_session.mode = DONGLE_MODE_SWITCH;
    _wlan_session.id   = _ns_wlan_new_session_id();

    /*
     * dongle_wake_s.session is the PACKED uint16_t (identical to pkt->session),
     * NOT a nested dongle_session_s struct (guide §4). Pack it explicitly.
     */
    _wlan_wake.session = dongle_session_pack(&_wlan_session);
    _wlan_wake.vid     = _wlan_vid;
    _wlan_wake.pid     = _wlan_pid;

    printf("[WLAN] New session id 0x%03X (mode %u, vid 0x%04X, pid 0x%04X)\n",
           _wlan_session.id, _wlan_session.mode, _wlan_vid, _wlan_pid);
}

/* Transmit exactly one fully-formed dongle_pkt_s to the dongle. This is the
 * ONLY place WLAN traffic ever leaves the gamepad, and it is only reachable
 * from the RX queue drain that runs in the main loop.
 *
 * Because this now executes in thread context (not inside an lwIP callback),
 * the lwIP calls below are bracketed with cyw43_arch_lwip_begin()/end() to
 * take the stack lock against the background context. */
static void _ns_wlan_send(const dongle_pkt_s *tx)
{
    if (_wlan_pcb == NULL || tx == NULL)
    {
        return;
    }

    /* Always transmit the full fixed structure; bytes past len stay zeroed. */
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(dongle_pkt_s), PBUF_RAM);
    if (p == NULL)
    {
        cyw43_arch_lwip_end();
        printf("[WLAN] TX drop: pbuf_alloc failed\n");
        return;
    }

    memcpy(p->payload, tx, sizeof(dongle_pkt_s));

    err_t err = udp_sendto(_wlan_pcb, p, &_wlan_dongle_addr, DONGLE_WLAN_PORT);

    pbuf_free(p);

    if (err != ERR_OK)
    {
        printf("[WLAN] TX error: udp_sendto -> %d\n", (int)err);
    }
}

/* Build a WAKE reply to a dongle WAKE beacon. */
static void _ns_wlan_build_wake_reply(dongle_pkt_s *tx)
{
    tx->session = dongle_session_pack(&_wlan_session);
    tx->id      = DONGLE_PID_WAKE;
    tx->len     = (uint16_t)sizeof(dongle_wake_s);
    memcpy(tx->data, &_wlan_wake, sizeof(dongle_wake_s));

    if (!_wlan_link_up)
    {
        printf("[WLAN] Replying to WAKE beacon (session 0x%03X)\n", _wlan_session.id);
    }
}

/* Build the current Switch input report into a CORE_UNRELIABLE reply. */
static void _ns_wlan_build_input_reply(dongle_pkt_s *tx)
{
    uint8_t report[64] = {0};

    tx->id = DONGLE_PID_CORE_UNRELIABLE;

    if (ns_api_generate_inputreport(report))
    {
        tx->id = (report[0] == 0x30) ? DONGLE_PID_CORE_UNRELIABLE : DONGLE_PID_CORE_RELIABLE;

        /* Count the steady-state unreliable input reports so the main loop can
         * print a clean per-second input rate (see _ns_wlan_report_stats). */
        if (tx->id == DONGLE_PID_CORE_UNRELIABLE)
        {
            _wlan_stat_input_reports++;
        }

        /* In Switch mode the whole 64-byte report (report id at byte 0) is the
         * CORE_UNRELIABLE payload the dongle relays to the console. */
        tx->len = 64;
        memcpy(tx->data, report, 64);
    }
    else
    {
        /* Nothing to report this poll. Still answer (one reply per RX) so the
         * dongle watchdog stays fed and any ack we owe is delivered. */
        tx->len = 0;
    }
}

/* Apply a STATUS payload (link / transport / player / rumble / brake).
 *
 * Per guide §11 the old single "connection" field is gone; status now carries
 * two independent fields:
 *   - link_status      : the wireless link (owned by the dongle WLAN core).
 *   - transport_status : whether the console side is actually enumerated.
 * Use transport_status (not link_status) to decide if we are truly "live" on
 * the console for player-LED / haptic feedback purposes. */
static void _ns_wlan_apply_status(const dongle_status_u *status)
{
    _wlan_status = *status;

    bool connected = (_wlan_status.transport_status == DONGLE_TRANSPORT_CONNECTED);
    if (connected != _wlan_transport_connected)
    {
        _wlan_transport_connected = connected;
        printf("[WLAN] Console transport %s\n",
               connected ? "CONNECTED" : "IDLE");
    }

    /*
     * No physical actuator is wired into this example, so the rumble/brake
     * amplitudes are simply discarded. A real product would drive its motors
     * here, and its player LEDs from player_number. We intentionally do NOT log
     * per-STATUS data: the dongle polls at the report rate, so printing here
     * would flood the terminal (see the periodic stats line instead).
     */
}

/* -------------------------------------------------------------------------- */
/* Packet processing: the heart of the respond-only protocol.                 */
/* One dongle datagram in -> at most one gamepad datagram out.                */
/*                                                                            */
/* This runs in the main loop (consumer side of the RX FIFO), so every call   */
/* into NS-LIB and lwIP happens on a single, predictable context.             */
/* -------------------------------------------------------------------------- */
static void _ns_wlan_process_packet(const dongle_pkt_s *rx)
{
    /*
     * Prepare a zeroed reply tagged with our session. Echo this datagram's ack
     * token directly: the dongle checks pkt->ack on every RX, so echoing rx.ack
     * on any reply opportunistically retires whatever it has inflight and is the
     * documented, robust behavior (guide §9). It is harmless on non-reliable
     * traffic, where rx.ack is simply 0.
     */
    dongle_pkt_s tx = {0};
    tx.session = dongle_session_pack(&_wlan_session);
    tx.ack     = rx->ack;

    bool send_reply = true;

    switch ((dongle_pid_t)rx->id)
    {
    case DONGLE_PID_WAKE:
        /* Only beacons (len == 0) get a WAKE reply, and only the first one in a
         * run of repeats: the dongle floods beacons until we are acknowledged,
         * but replying to each restarts our input/session and spams the host. */
        if (rx->len == 0 && !_wlan_wake_replied)
        {
            _ns_wlan_build_wake_reply(&tx);
            _wlan_wake_replied = true;
        }
        else
        {
            send_reply = false;
        }
        break;

    case DONGLE_PID_STATUS:
        /* Any non-WAKE traffic means the dongle has advanced past the beacon
         * phase, so re-arm WAKE handling for the next beacon run. */
        _wlan_wake_replied = false;
        if (!_wlan_link_up)
        {
            _wlan_link_up = true;
            printf("[WLAN] Link up (dongle is polling us)\n");
        }
        /* A STATUS payload is exactly sizeof(dongle_status_u) (== 8) bytes. */
        if (rx->len == sizeof(dongle_status_u))
        {
            _ns_wlan_apply_status((const dongle_status_u *)rx->data);
        }
        _ns_wlan_build_input_reply(&tx);
        break;

    case DONGLE_PID_CORE_RELIABLE:
        /* Non-WAKE traffic: re-arm WAKE handling (see STATUS case above). */
        _wlan_wake_replied = false;
        if (!_wlan_link_up)
        {
            _wlan_link_up = true;
            printf("[WLAN] Link up (dongle is polling us)\n");
        }
        /* Host OUT (Switch commands / rumble). Hand it to the protocol engine;
         * any reply it queues is popped by the input report build below and
         * delivered in this same reply. rx->ack was already echoed into tx.ack
         * above, which retires the dongle's inflight reliable packet (guide §9).
         * ns_api_output_tunnel() is effectively idempotent on resends. */
        if (rx->len > 0)
        {
            /* Stop-and-wait dedup: the dongle resends the same CORE_RELIABLE
             * packet (identical ack token) until it sees the token echoed back.
             * Tunnel each host OUT exactly once; a resend carrying an ack we have
             * already processed is a duplicate and must not be re-queued (doing so
             * would emit duplicate command replies). We still fall through to send
             * a reply below, echoing rx.ack so the dongle can retire the packet. */
            if (_wlan_have_reliable_ack && rx->ack == _wlan_last_reliable_ack)
            {
                /* Duplicate resend; silently re-ack below without re-tunneling. */
            }
            else
            {
                ns_api_output_tunnel(rx->data, rx->len);
                _wlan_last_reliable_ack = rx->ack;
                _wlan_have_reliable_ack = true;
            }
        }
        _ns_wlan_build_input_reply(&tx);
        break;

    case DONGLE_PID_BULK_UNRELIABLE:
    case DONGLE_PID_CONFIG_RELIABLE:
        /* Reserved endpoints (webUSB bulk / config bulk) — not yet fleshed out
         * on the dongle (guide §5). Ignore them and stay silent for now. */
        send_reply = false;
        break;

    default:
        /* Dongle sent something we do not answer. Stay silent (no proactive TX). */
        send_reply = false;
        break;
    }

    if (send_reply)
    {
        _ns_wlan_send(&tx);
    }
}

/* -------------------------------------------------------------------------- */
/* UDP receive callback (producer).                                           */
/*                                                                            */
/* Runs in the cyw43 background context. Keep it as short as possible: just   */
/* validate the datagram length and copy it into the RX FIFO. All protocol    */
/* work is deferred to _ns_wlan_rx_task() on the main loop. This lets a burst */
/* of datagrams queue up instead of forcing a reply (and an NS-LIB call) from */
/* inside the network callback.                                               */
/* -------------------------------------------------------------------------- */
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
     * is not part of the protocol, so drop it without queueing. */
    if (p->tot_len != sizeof(dongle_pkt_s))
    {
        printf("[WLAN] RX drop: unexpected length %u (want %u)\n",
               p->tot_len, (unsigned)sizeof(dongle_pkt_s));
        pbuf_free(p);
        return;
    }

    uint32_t head = _wlan_rx_head;
    uint32_t next = (head + 1u) % NS_WLAN_RX_QUEUE_LEN;

    /* next == tail means the ring is full (one slot is always kept empty). */
    if (next == _wlan_rx_tail)
    {
        _wlan_rx_dropped++;
        pbuf_free(p);
        return;
    }

    pbuf_copy_partial(p, &_wlan_rx_queue[head], sizeof(dongle_pkt_s), 0);
    pbuf_free(p);

    /* Publish the slot only after it is fully written so the consumer never
     * observes a half-filled entry. */
    _wlan_rx_head = next;
}

/* Drain the RX FIFO (consumer). Called from the main loop; processes every
 * datagram queued by the receive callback since the last pass, in order. */
static void _ns_wlan_rx_task(void)
{
    while (_wlan_rx_tail != _wlan_rx_head)
    {
        uint32_t tail = _wlan_rx_tail;

        _ns_wlan_process_packet(&_wlan_rx_queue[tail]);

        /* Release the slot only after processing so the producer cannot
         * overwrite an entry we are still reading. */
        _wlan_rx_tail = (tail + 1u) % NS_WLAN_RX_QUEUE_LEN;
    }
}

/* Print a single, clean throughput line once per NS_WLAN_STATS_PERIOD_MS.
 * Reports the live unreliable-input rate (reports/s) and any RX drops seen in
 * the interval. Called from the main loop; stays quiet until the link is up so
 * idle boards do not chatter. */
static void _ns_wlan_report_stats(void)
{
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t elapsed = now - _wlan_stat_last_ms;
    if (elapsed < NS_WLAN_STATS_PERIOD_MS)
    {
        return;
    }

    uint32_t reports = _wlan_stat_input_reports;
    _wlan_stat_input_reports = 0;
    _wlan_stat_last_ms = now;

    /* _wlan_rx_dropped is a free-running counter owned by the RX callback; take
     * a delta against the last snapshot rather than resetting it, to avoid a
     * lost increment racing with the background context. */
    uint32_t dropped_total = _wlan_rx_dropped;
    uint32_t dropped = dropped_total - _wlan_stat_last_dropped;
    _wlan_stat_last_dropped = dropped_total;

    if (!_wlan_link_up)
    {
        return;
    }

    /* Normalize to a per-second rate in case the loop tick drifts. */
    uint32_t rate = (elapsed > 0) ? (reports * 1000u) / elapsed : reports;

    printf("[WLAN] input %lu rep/s | dropped %lu\n",
           (unsigned long)rate, (unsigned long)dropped);
}

/* -------------------------------------------------------------------------- */
/* Association / bring-up                                                      */
/* -------------------------------------------------------------------------- */

/*
 * Pin the station to the fixed gamepad address the dongle filters on. lwIP /
 * cyw43_arch start DHCP automatically on link-up, so we stop it and overwrite
 * the address here. After this our source IP is deterministic (192.168.4.16)
 * regardless of what the dongle DHCP server offered.
 */
static void _ns_wlan_apply_static_ip(void)
{
    ip4_addr_t ip, mask, gw;
    IP4_ADDR(&ip, DONGLE_GAMEPAD_IP0, DONGLE_GAMEPAD_IP1, DONGLE_GAMEPAD_IP2, DONGLE_GAMEPAD_IP3);
    IP4_ADDR(&mask, NS_WLAN_NETMASK0, NS_WLAN_NETMASK1, NS_WLAN_NETMASK2, NS_WLAN_NETMASK3);
    IP4_ADDR(&gw, NS_WLAN_DONGLE_IP0, NS_WLAN_DONGLE_IP1, NS_WLAN_DONGLE_IP2, NS_WLAN_DONGLE_IP3);

    cyw43_arch_lwip_begin();
    struct netif *nif = netif_default;
    if (nif != NULL)
    {
        dhcp_stop(nif);
        netif_set_addr(nif, &ip, &mask, &gw);
    }
    cyw43_arch_lwip_end();
}

/* Join the dongle AP, retrying until it succeeds, then pin our static IP. */
static void _ns_wlan_connect(void)
{
    cyw43_arch_enable_sta_mode();
    cyw43_wifi_set_roam_enabled(&cyw43_state, false);
    cyw43_wifi_set_interference_mode(&cyw43_state, CYW43_IFMODE_NONE);

    printf("[WLAN] Station mode enabled, joining SSID \"%s\"\n", NS_WLAN_SSID);

    for (;;)
    {
        printf("[WLAN] Associating...\n");
        int rc = cyw43_arch_wifi_connect_timeout_ms(NS_WLAN_SSID, NS_WLAN_PASSWORD,
                                                     NS_WLAN_AUTH, NS_WLAN_CONNECT_TIMEOUT_MS);
        if (rc == 0)
        {
            break;
        }

        printf("[WLAN] Association failed (rc=%d), retrying in 1s\n", rc);
        sleep_ms(1000);
    }

    /*
     * Associated. Discard any DHCP lease and assign the address the dongle
     * expects, so our datagrams pass its source-IP filter (192.168.4.16).
     */
    const ip4_addr_t *dhcp_ip = netif_ip4_addr(netif_default);
    printf("[WLAN] Associated (DHCP offered %s)\n", ip4addr_ntoa(dhcp_ip));

    _ns_wlan_apply_static_ip();

    const ip4_addr_t *ip = netif_ip4_addr(netif_default);
    printf("[WLAN] Using static gamepad IP %s (gw 192.168.4.1)\n", ip4addr_ntoa(ip));

    cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
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

/* Tear down the UDP socket. Safe to call with no socket bound. Must run before
 * cyw43_arch_deinit() so the lwIP lock is still valid. */
static void _ns_wlan_unbind(void)
{
    if (_wlan_pcb != NULL)
    {
        cyw43_arch_lwip_begin();
        udp_remove(_wlan_pcb);
        cyw43_arch_lwip_end();
        _wlan_pcb = NULL;
    }
}

/* Full radio bring-up: init the cyw43 driver, associate to the dongle AP, pin
 * our static IP, and bind UDP. Returns false (after cleaning up) if init or
 * bind fails; association itself retries internally until it succeeds. */
static bool _ns_wlan_bringup(void)
{
    if (cyw43_arch_init())
    {
        printf("[WLAN] cyw43_arch_init() failed\n");
        return false;
    }

    _ns_wlan_connect();

    if (!_ns_wlan_bind())
    {
        cyw43_arch_deinit();
        return false;
    }

    return true;
}

/* Full radio teardown: drop the UDP socket and power down / de-init the cyw43
 * driver. Pairs with _ns_wlan_bringup() for a clean re-initialization. */
static void _ns_wlan_teardown(void)
{
    _ns_wlan_unbind();
    cyw43_arch_deinit();
}

void ns_wlan_enter(void)
{
    printf("[WLAN] Entering HOJA dongle Switch mode\n");

    /* USB identity to advertise to the dongle comes from NS-LIB-HID. The
     * library was configured with NS_TRANSPORT_USB so this lookup succeeds. */
    if (!ns_hid_get_descriptor_params(NULL, NULL, NULL, NULL, &_wlan_vid, &_wlan_pid))
    {
        printf("[WLAN] WARNING: could not read NS-LIB vid/pid; using 0/0\n");
    }

    /* Destination for every reply: the dongle AP. */
    IP4_ADDR(ip_2_ip4(&_wlan_dongle_addr),
             NS_WLAN_DONGLE_IP0, NS_WLAN_DONGLE_IP1, NS_WLAN_DONGLE_IP2, NS_WLAN_DONGLE_IP3);
    IP_SET_TYPE(&_wlan_dongle_addr, IPADDR_TYPE_V4);

    /* Pick the first session id before any traffic flows. */
    _ns_wlan_refresh_wake();

    while (!_ns_wlan_bringup())
    {
        printf("[WLAN] Bring-up failed, retrying in 1s\n");
        sleep_ms(1000);
    }

    /*
     * Reactive loop: the UDP receive callback only queues datagrams; here we
     * drain that FIFO and run all protocol work (one reply per datagram), then
     * service deferred flash writes, print throughput stats, and watch the
     * Wi-Fi link so we can recover if the AP drops.
     */
    for (;;)
    {
        _ns_wlan_rx_task();

        ns_flash_task();

        _ns_wlan_report_stats();

        int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (link != CYW43_LINK_UP)
        {
            printf("[WLAN] Link lost (status=%d), reinitializing radio\n", link);
            _wlan_link_up = false;
            _wlan_transport_connected = false;
            _wlan_wake_replied = false;
            _wlan_have_reliable_ack = false;

            /* Drop anything still queued from the old session so the fresh
             * session never replies to stale datagrams. The producer is idle
             * while the link is down, so resetting both indices is safe. */
            _wlan_rx_tail = _wlan_rx_head;

            /*
             * A dropped link is recovered by fully cycling the radio: bring the
             * cyw43 driver all the way down and re-initialize it, rather than
             * just re-associating on the existing instance. This clears any
             * wedged driver/netif state. A reconnect is also a new logical
             * session per the protocol guide, so refresh the session id.
             */
            _ns_wlan_teardown();
            _ns_wlan_refresh_wake();

            while (!_ns_wlan_bringup())
            {
                printf("[WLAN] Re-init failed, retrying in 1s\n");
                sleep_ms(1000);
            }
        }

        sleep_ms(1);
    }
}
