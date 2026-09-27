#ifndef __LWIPOPTS_H__
#define __LWIPOPTS_H__

// Disable RTOS dependencies (Bare-metal mode)
#define NO_SYS 0

// Enable DHCP
#define LWIP_DHCP 1
#define LWIP_UDP 1
#define LWIP_TCP 1
#define LWIP_ICMP 1

// Massive memory pool to hold an entire 150KB frame at once!
#define MEM_ALIGNMENT 4
#define MEM_SIZE (256 * 1024)

// OS Thread Stack Sizes (Crucial for preventing Usage Faults)
#define TCPIP_THREAD_STACKSIZE 4096
#define TCPIP_THREAD_PRIO 8
#define TCPIP_MBOX_SIZE 32
#define DEFAULT_THREAD_STACKSIZE 4096
#define DEFAULT_TCP_RECVMBOX_SIZE 128
#define DEFAULT_UDP_RECVMBOX_SIZE 128
#define DEFAULT_RAW_RECVMBOX_SIZE 128
#define DEFAULT_ACCEPTMBOX_SIZE 128

// Debugging options — DISABLED for streaming performance!
// When LWIP_DEBUG is enabled, every udp_sendto() triggers ~13 blocking
// tm_printf() calls over UART. At 94 packets/frame, that's ~1222 serial
// prints per frame, causing a 7+ second UART stall and dropping FPS to 0.1.
// Re-enable temporarily only when debugging lwIP protocol issues.
// #define LWIP_DEBUG 1
// #define ETHARP_DEBUG LWIP_DBG_ON
// #define NETIF_DEBUG LWIP_DBG_ON
// #define ICMP_DEBUG LWIP_DBG_ON
// #define IP_DEBUG LWIP_DBG_ON
// #define TCPIP_DEBUG LWIP_DBG_ON
// #define UDP_DEBUG LWIP_DBG_ON
// #define INET_DEBUG LWIP_DBG_ON
// #define IP_REASS_DEBUG LWIP_DBG_ON

#endif /* __LWIPOPTS_H__ */
