/**
 * @file lwipopts.h
 * @brief Minimal lwIP configuration for the WLAN dongle transport.
 *
 * This enables just enough of lwIP for a CYW43 Wi-Fi station that obtains a
 * DHCP lease from the HOJA dongle AP and exchanges fixed-size UDP datagrams.
 * It targets the NO_SYS (bare-metal / threadsafe-background) cyw43_arch mode
 * used by ns_wlan.c.
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

#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

/* Bare-metal core: lwIP runs from the cyw43_arch background context, no RTOS. */
#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

/* Use lwIP's own heap rather than the C library malloc. */
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    4000

#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_ARP_QUEUE          10
#define PBUF_POOL_SIZE              24

/* Protocol features: DHCP needs UDP; the dongle protocol is UDP only. */
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_UDP                    1
#define LWIP_TCP                    0
#define LWIP_DNS                    0
#define LWIP_DHCP                   1

/*
 * DHCP is enabled so the cyw43_arch blocking connect can reach LINK_UP, but
 * ns_wlan.c immediately stops DHCP and pins the fixed gamepad address
 * (192.168.4.16) the dongle filters on. Skip the ARP probe delay either way.
 */
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_TX_SINGLE_PBUF   1

/* Statistics off to keep the build small; the example logs over UART instead. */
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0

/* The CYW43 driver computes checksums in software on this path. */
#define LWIP_CHKSUM_ALGORITHM       3

#ifndef NDEBUG
#define LWIP_DEBUG                  1
#define LWIP_STATS                  1
#define LWIP_STATS_DISPLAY          1
#endif

#define ETHARP_DEBUG                LWIP_DBG_OFF
#define NETIF_DEBUG                 LWIP_DBG_OFF
#define DHCP_DEBUG                  LWIP_DBG_OFF
#define UDP_DEBUG                   LWIP_DBG_OFF
#define IP_DEBUG                    LWIP_DBG_OFF

#endif /* _LWIPOPTS_H */
