#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

// lwIP configuration for the test rig: pico_cyw43_arch_lwip_poll (NO_SYS),
// HTTP *client* only — one short-lived connection per JSON command against
// the pico2w_remote target. Derived from pico2w_remote's lwipopts, minus
// the mDNS responder and DNS (the target IP comes from its CDC netstatus).

#define NO_SYS 1
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0

#define MEM_LIBC_MALLOC 0
#define MEM_ALIGNMENT 4
#define MEM_SIZE 16384
#define MEMP_NUM_TCP_SEG 32
#define MEMP_NUM_ARP_QUEUE 10
#define PBUF_POOL_SIZE 16

#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_RAW 1
#define LWIP_IPV4 1
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_DNS 0
#define LWIP_DHCP 1
#define DHCP_DOES_ARP_CHECK 0
#define LWIP_DHCP_DOES_ACD_CHECK 0

// Receive-biased TCP buffering: getframebuffer responses stream a few
// hundred KB toward the rig; requests are tiny.
#define TCP_MSS 1460
#define TCP_WND (8 * TCP_MSS)
#define TCP_SND_BUF (4 * TCP_MSS)
#define TCP_SND_QUEUELEN ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_TCP_KEEPALIVE 1
#define LWIP_NETIF_TX_SINGLE_PBUF 1

#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETIF_HOSTNAME 1

#define LWIP_CHKSUM_ALGORITHM 3

#define MEM_STATS 0
#define SYS_STATS 0
#define MEMP_STATS 0
#define LINK_STATS 0

#ifndef NDEBUG
#define LWIP_DEBUG 0
#endif

#endif  // _LWIPOPTS_H
