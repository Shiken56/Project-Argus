#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#define PRINT(fmt, ...) tm_printf((const UB *)(fmt), ##__VA_ARGS__)

/* LwIP Includes */
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/udp.h"
#include "netif/ethernet.h"
#include "ethernetif.h"
#include <stdio.h>
#include <string.h>
#include "stm32n6xx_hal.h"

#include "app_camera.h"
#include "app_od.h"
#include "app_camera_ethernet_test.h"

struct netif gnetif;

LOCAL void net_task(INT stacd, void *exinf); // task execution function
LOCAL ID	tskid_net;			             // Task ID number
LOCAL T_CTSK ctsk_net = {				     // Task creation information
	.itskpri	= 10,
	.stksz		= 4096,
	.task		= net_task,
	.tskatr		= TA_HLNG | TA_RNG0,
};

/* Camera Task Configuration */
LOCAL ID	tskid_cam;
LOCAL T_CTSK ctsk_cam = {
	.itskpri	= 11,
	.stksz		= 4096,
	.task		= camera_task,
	.tskatr		= TA_HLNG | TA_RNG0,
};



/* 3. Standard Ethernet Broadcast ARP Request */
static void send_raw_arp_request(uint32_t seq)
{
    (void)seq;
    uint8_t frame[60];
    memset(frame, 0, sizeof(frame));

    /* Ethernet Header (14 bytes) */
    memset(&frame[0], 0xFF, 6);                         /* Dst MAC: Broadcast */
    frame[6] = 0x00; frame[7] = 0x80; frame[8] = 0xE1;
    frame[9] = 0x00; frame[10] = 0x00; frame[11] = 0x00;/* Src MAC: 00:80:e1:00:00:00 */
    frame[12] = 0x08; frame[13] = 0x06;                 /* EtherType: ARP */

    /* ARP Payload (28 bytes) */
    frame[14] = 0x00; frame[15] = 0x01;                 /* Hardware Type: Ethernet (1) */
    frame[16] = 0x08; frame[17] = 0x00;                 /* Protocol Type: IPv4 (0x0800) */
    frame[18] = 0x06;                                   /* Hardware Size: 6 */
    frame[19] = 0x04;                                   /* Protocol Size: 4 */
    frame[20] = 0x00; frame[21] = 0x01;                 /* Opcode: Request (1) */

    /* Sender MAC (00:80:e1:00:00:00) */
    frame[22] = 0x00; frame[23] = 0x80; frame[24] = 0xE1;
    frame[25] = 0x00; frame[26] = 0x00; frame[27] = 0x00;
    
    /* Sender IP (192.168.1.10) */
    frame[28] = 192; frame[29] = 168; frame[30] = 1; frame[31] = 10;

    /* Target MAC (00:00:00:00:00:00 - ignored in request) */
    memset(&frame[32], 0x00, 6);

    /* Target IP (192.168.1.100 - laptop) */
    frame[38] = 192; frame[39] = 168; frame[40] = 1; frame[41] = 100;

    ethernetif_send_raw(frame, 42); // 14 + 28 = 42 bytes. DMA will pad to 60.
}

LOCAL void net_task(INT stacd, void *exinf)
{
    ip4_addr_t ipaddr;
    ip4_addr_t netmask;
    ip4_addr_t gw;
    uint32_t tx_timer = 0;
    uint32_t pkt_seq = 0;
    PRINT("\r\n========================================\r\n");
    PRINT("  STM32N657 Ethernet TX & Ping Test    \r\n");
    PRINT("========================================\r\n");

    /* 1. Start the TCP/IP thread */
    PRINT("[NET] Starting TCP/IP thread...\r\n");
    tcpip_init(NULL, NULL);
    PRINT("[NET] TCP/IP thread running.\r\n");

    /* 2. Configure Static IP Address: 192.168.1.10 */
    IP4_ADDR(&ipaddr, 192, 168, 1, 10);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 192, 168, 1, 1);

    /* 3. Add the network interface */
    netif_add(&gnetif, &ipaddr, &netmask, &gw, NULL, &ethernetif_init, &tcpip_input);

    /* 4. Set as default, bring up the interface and link */
    netif_set_default(&gnetif);
    netif_set_up(&gnetif);
    netif_set_link_up(&gnetif);

    PRINT("[NET] Interface is UP!\r\n");
    PRINT("[NET] Static IP  : 192.168.1.10\r\n");
    PRINT("[NET] Netmask    : 255.255.255.0\r\n");
    PRINT("[NET] Gateway    : 192.168.1.1\r\n");

    /* Initialize Video + Telemetry Ethernet Streamer */
    Ethernet_Streamer_Init();

    uint32_t heartbeat_timer = 0;
    uint32_t heartbeat_sec = 0;

    while (1)
    {
        /* 1. Poll Ethernet driver for incoming packets */
        ethernetif_input(&gnetif);

        /* 2. Every ~1 second (500 * 2ms): Check link/speed & send beacon */
        if (++tx_timer >= 500) {
            tx_timer = 0;
            pkt_seq++;
            
            /* Dynamic link status & speed detection */
            ethernetif_check_link_and_speed(&gnetif);

            /* Send raw ARP Request to bypass router unicast filters */
            send_raw_arp_request(pkt_seq);
        }

        /* 3. Every ~3 seconds (1500 * 2ms): Network health heartbeat */
        if (++heartbeat_timer >= 1500) {
            heartbeat_timer = 0;
            heartbeat_sec += 3;

            PRINT("[HEARTBEAT %lus] Total RX: %lu pkts | Total TX: %lu pkts | Link: %s\r\n",
                  (unsigned long)heartbeat_sec,
                  (unsigned long)g_rx_pkt_count,
                  (unsigned long)g_tx_pkt_count,
                  netif_is_link_up(&gnetif) ? "LINK UP" : "LINK DOWN");
        }

        /* microT-Kernel OS delay: yields CPU to TCP/IP thread and idle tasks */
        tk_dly_tsk(2);
    }
}

EXPORT INT usermain(void)
{
	/* Create & Start Network Task */
	tskid_net = tk_cre_tsk(&ctsk_net);
	tk_sta_tsk(tskid_net, 0);

	/* Create & Start Camera Task */
	tskid_cam = tk_cre_tsk(&ctsk_cam);
	tk_sta_tsk(tskid_cam, 0);

	/* Start OD Task */
	start_od_task();

	tk_slp_tsk(TMO_FEVR);

	return 0;
}
