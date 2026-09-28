#ifndef __LWIPOPTS_H__
#define __LWIPOPTS_H__

// ============================================================
// RTOS / THREADING
// ============================================================
#define NO_SYS 0

// Enable LwIP TCP/IP Core Locking for thread-safe multi-task access.
// All external tasks (net_task, ethernet_stream_task) acquire this mutex
// before touching any LwIP API. tcpip_thread holds it while processing.
#define LWIP_TCPIP_CORE_LOCKING 1

// CRITICAL: Enable lightweight (interrupt-disable) critical sections inside
// LwIP's memp allocator. Without this, SYS_ARCH_PROTECT() is a no-op and
// do_memp_malloc_pool()'s free-list walk has NO protection against Ethernet
// DMA ISRs firing mid-update — corrupting desc->tab and causing Bus Faults.
// sys_arch_protect() = DI() and sys_arch_unprotect() = EI() in sys_arch.c.
#define SYS_LIGHTWEIGHT_PROT 1

// ============================================================
// PROTOCOLS
// ============================================================
#define LWIP_DHCP  1
#define LWIP_UDP   1
#define LWIP_TCP   1
#define LWIP_ICMP  1

// ============================================================
// MEMORY
// ============================================================
#define MEM_ALIGNMENT  4
#define MEM_SIZE       (256 * 1024)   /* 256 KB heap for UDP TX frames  */

// PBUF_POOL is used by low_level_input() for every incoming RX packet.
// Default (16) is too small: during a 12 ms TX burst tcpip_thread is
// blocked, RX pbufs back up, and the pool exhausts → NULL free-list pointer
// → Bus Fault on the next dereference. 32 entries gives ~2 s of headroom.
#define PBUF_POOL_SIZE 32

// ============================================================
// OS THREAD PARAMETERS
// ============================================================
#define TCPIP_THREAD_STACKSIZE    4096
#define TCPIP_THREAD_PRIO         8
#define TCPIP_MBOX_SIZE           32
#define DEFAULT_THREAD_STACKSIZE  4096
#define DEFAULT_TCP_RECVMBOX_SIZE 128
#define DEFAULT_UDP_RECVMBOX_SIZE 128
#define DEFAULT_RAW_RECVMBOX_SIZE 128
#define DEFAULT_ACCEPTMBOX_SIZE   128

// ============================================================
// DIAGNOSTICS  (enabled for crash hunting — disable for production)
// ============================================================
// Track per-pool allocation counts/errors.  Access via lwip_stats.memp[].
#define LWIP_STATS      1
#define MEMP_STATS      1
#define MEM_STATS       1

// ============================================================
// DEBUG UART (disabled — see comments below)
// ============================================================
// When LWIP_DEBUG is enabled, every udp_sendto() triggers ~13 blocking
// tm_printf() calls over UART. At 94 packets/frame → ~1222 serial prints/frame
// → 7+ second UART stall, FPS drops to 0.1. Re-enable only when debugging.
// #define LWIP_DEBUG 1
// #define ETHARP_DEBUG LWIP_DBG_ON
// #define NETIF_DEBUG  LWIP_DBG_ON
// #define ICMP_DEBUG   LWIP_DBG_ON
// #define IP_DEBUG     LWIP_DBG_ON
// #define TCPIP_DEBUG  LWIP_DBG_ON
// #define UDP_DEBUG    LWIP_DBG_ON
// #define INET_DEBUG   LWIP_DBG_ON
// #define IP_REASS_DEBUG LWIP_DBG_ON

#endif /* __LWIPOPTS_H__ */

