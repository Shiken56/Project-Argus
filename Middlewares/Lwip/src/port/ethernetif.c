#include "ethernetif.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/opt.h"
#include "netif/ethernet.h"
#include "stm32n6xx_hal.h"
#include <stdint.h>
#include <string.h>

extern int tm_printf(const unsigned char *format, ...);

/* The global ST HAL Ethernet handle defined in main.c */
extern ETH_HandleTypeDef heth1;

#define IFNAME0 's'
#define IFNAME1 't'

/* Packet counters for live debugging */
volatile uint32_t g_rx_pkt_count = 0;
volatile uint32_t g_tx_pkt_count = 0;

/* ========================================================================= */
/* DIAGNOSTIC LOGGING & MEMORY DUMP HELPERS                                  */
/* ========================================================================= */

/**
 * @brief  Hex memory dump to serial monitor.
 */
static void dump_hex_memory(const char *label, const void *ptr, uint32_t len) {
  const uint8_t *p = (const uint8_t *)ptr;
  tm_printf("[HEX DUMP] %s @ 0x%08lX (%lu bytes):\r\n  ", label,
            (unsigned long)(uintptr_t)ptr, (unsigned long)len);
  for (uint32_t i = 0; i < len; i++) {
    tm_printf("%02X ", p[i]);
    if ((i + 1) % 16 == 0 && (i + 1) < len) {
      tm_printf("\r\n  ");
    }
  }
  tm_printf("\r\n");
}

/**
 * @brief  Decode and log Ethernet / ARP / IPv4 / ICMP / UDP packet headers.
 */
static void log_packet_details(const char *direction, const uint8_t *data, uint16_t len) {
  if (data == NULL || len < 14) {
    tm_printf("[%s] Invalid or short frame (%u bytes)\r\n", direction, (unsigned)len);
    return;
  }

  uint16_t ethertype = ((uint16_t)data[12] << 8) | data[13];
  tm_printf("[%s] Len=%u | Dst=%02X:%02X:%02X:%02X:%02X:%02X Src=%02X:%02X:%02X:%02X:%02X:%02X Type=0x%04X ",
            direction, (unsigned)len,
            data[0], data[1], data[2], data[3], data[4], data[5],
            data[6], data[7], data[8], data[9], data[10], data[11],
            (unsigned)ethertype);

  if (ethertype == 0x0806 && len >= 42) {
    /* ARP Frame */
    uint16_t op = ((uint16_t)data[20] << 8) | data[21];
    tm_printf("[ARP %s: Who has %u.%u.%u.%u? Tell %u.%u.%u.%u]\r\n",
              (op == 1) ? "REQUEST" : (op == 2) ? "REPLY" : "UNKNOWN",
              data[38], data[39], data[40], data[41],
              data[28], data[29], data[30], data[31]);
  } else if (ethertype == 0x0800 && len >= 34) {
    /* IPv4 Frame */
    uint8_t proto = data[23];
    uint8_t src_ip[4] = {data[26], data[27], data[28], data[29]};
    uint8_t dst_ip[4] = {data[30], data[31], data[32], data[33]};

    if (proto == 1 && len >= 38) {
      /* ICMP */
      uint8_t icmp_type = data[34];
      uint8_t icmp_code = data[35];
      uint16_t icmp_id = ((uint16_t)data[38] << 8) | data[39];
      uint16_t icmp_seq = ((uint16_t)data[40] << 8) | data[41];
      tm_printf("[IPv4 ICMP %s: %u.%u.%u.%u -> %u.%u.%u.%u | Code=%u ID=0x%04X Seq=%u]\r\n",
                (icmp_type == 8) ? "ECHO REQUEST (PING!)"
                : (icmp_type == 0) ? "ECHO REPLY (PING RESPONSE!)"
                : "OTHER",
                src_ip[0], src_ip[1], src_ip[2], src_ip[3],
                dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3],
                (unsigned)icmp_code, (unsigned)icmp_id, (unsigned)icmp_seq);
    } else if (proto == 17 && len >= 42) {
      /* UDP */
      uint16_t sport = ((uint16_t)data[34] << 8) | data[35];
      uint16_t dport = ((uint16_t)data[36] << 8) | data[37];
      tm_printf("[IPv4 UDP: %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u]\r\n",
                src_ip[0], src_ip[1], src_ip[2], src_ip[3], (unsigned)sport,
                dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3], (unsigned)dport);
    } else {
      tm_printf("[IPv4 Proto=%u: %u.%u.%u.%u -> %u.%u.%u.%u]\r\n",
                (unsigned)proto,
                src_ip[0], src_ip[1], src_ip[2], src_ip[3],
                dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3]);
    }
  } else {
    tm_printf("\r\n");
  }
}

/**
 * @brief  Dump TX Descriptor words and DMA hardware pointers.
 */
static void dump_tx_desc_state(const char *moment) {
  uint32_t cur = heth1.TxDescList[0].CurTxDesc;
  uint32_t prev = (cur + ETH_TX_DESC_CNT - 1) % ETH_TX_DESC_CNT;
  ETH_DMADescTypeDef *desc_prev = (ETH_DMADescTypeDef *)heth1.TxDescList[0].TxDesc[prev];

  tm_printf("[TX DESC %s] CurRingIdx=%lu (PrevIdx=%lu)\r\n", moment, (unsigned long)cur, (unsigned long)prev);
  if (desc_prev != NULL) {
    tm_printf("   [Submitted Desc #%lu @ 0x%08lX]: DESC0=0x%08lX DESC1=0x%08lX DESC2=0x%08lX DESC3=0x%08lX (OWN=%s)\r\n",
              (unsigned long)prev, (unsigned long)(uintptr_t)desc_prev,
              (unsigned long)desc_prev->DESC0, (unsigned long)desc_prev->DESC1,
              (unsigned long)desc_prev->DESC2, (unsigned long)desc_prev->DESC3,
              (desc_prev->DESC3 & ETH_DMATXNDESCWBF_OWN) ? "1 [DMA STILL OWNS]" : "0 [CPU OWNS / TX COMPLETE]");
    if (!(desc_prev->DESC3 & ETH_DMATXNDESCWBF_OWN)) {
      if (desc_prev->DESC3 & ETH_DMATXNDESCWBF_ES) {
        tm_printf("      WARNING! TX Desc Error Summary (ES=1): DB=%lu ED=%lu LCO=%lu EC=%lu UF=%lu FF=%lu LCA=%lu NC=%lu\r\n",
                  (unsigned long)((desc_prev->DESC3 & ETH_DMATXNDESCWBF_DB) != 0),
                  (unsigned long)((desc_prev->DESC3 & ETH_DMATXNDESCWBF_ED) != 0),
                  (unsigned long)((desc_prev->DESC3 & ETH_DMATXNDESCWBF_LCO) != 0),
                  (unsigned long)((desc_prev->DESC3 & ETH_DMATXNDESCWBF_EC) != 0),
                  (unsigned long)((desc_prev->DESC3 & ETH_DMATXNDESCWBF_UF) != 0),
                  (unsigned long)((desc_prev->DESC3 & ETH_DMATXNDESCWBF_FF) != 0),
                  (unsigned long)((desc_prev->DESC3 & ETH_DMATXNDESCWBF_LCA) != 0),
                  (unsigned long)((desc_prev->DESC3 & ETH_DMATXNDESCWBF_NC) != 0));
      }
    }
  }

  if (heth1.Instance != NULL) {
    uint32_t dmacsr = heth1.Instance->DMA_CH[0].DMACSR;
    tm_printf("   [DMA HW] DMACCATXDR=0x%08lX | DMACCATXBR=0x%08lX | DMACSR=0x%08lX (TI=%lu, TBU=%lu, RI=%lu, RBU=%lu, FBE=%lu)\r\n",
              (unsigned long)heth1.Instance->DMA_CH[0].DMACCATXDR,
              (unsigned long)heth1.Instance->DMA_CH[0].DMACCATXBR,
              (unsigned long)dmacsr,
              (unsigned long)((dmacsr & ETH_DMACxSR_TI) != 0),
              (unsigned long)((dmacsr & ETH_DMACxSR_TBU) != 0),
              (unsigned long)((dmacsr & ETH_DMACxSR_RI) != 0),
              (unsigned long)((dmacsr & ETH_DMACxSR_RBU) != 0),
              (unsigned long)((dmacsr & ETH_DMACxSR_FBE) != 0));
  }
}

/* ========================================================================= */
/* TX PATH DEEP DIAGNOSTICS                                                  */
/* ========================================================================= */

/**
 * @brief  Compare MACCR speed/duplex config against PHY negotiated values.
 *         Detects the #1 cause of "TX frames disappear": speed mismatch.
 */
static void dump_tx_speed_mismatch_check(void) {
  if (heth1.Instance == NULL) return;

  uint32_t maccr = heth1.Instance->MACCR;
  uint32_t mac_ps  = (maccr & ETH_MACCR_PS)  ? 1 : 0;  /* Port Select: 1=10/100, 0=1000 */
  uint32_t mac_fes = (maccr & ETH_MACCR_FES) ? 1 : 0;  /* Fast Ethernet Speed: 1=100, 0=10 */
  uint32_t mac_dm  = (maccr & ETH_MACCR_DM)  ? 1 : 0;  /* Duplex Mode: 1=Full, 0=Half */
  uint32_t mac_te  = (maccr & ETH_MACCR_TE)  ? 1 : 0;  /* Transmitter Enable */
  uint32_t mac_re  = (maccr & ETH_MACCR_RE)  ? 1 : 0;  /* Receiver Enable */

  const char *mac_speed_str;
  uint32_t mac_speed_mbps;
  if (!mac_ps) {
    mac_speed_str = "1000 Mbps"; mac_speed_mbps = 1000;
  } else if (mac_fes) {
    mac_speed_str = "100 Mbps"; mac_speed_mbps = 100;
  } else {
    mac_speed_str = "10 Mbps"; mac_speed_mbps = 10;
  }

  /* Read PHY resolved speed from RTL8211F page 0xa43 reg 0x1A */
  uint32_t physr = 0;
  HAL_ETH_WritePHYRegister(&heth1, 1, 31, 0x0a43);
  HAL_ETH_ReadPHYRegister(&heth1, 1, 0x1A, &physr);
  HAL_ETH_WritePHYRegister(&heth1, 1, 31, 0x0000);

  uint32_t phy_speed_bits = (physr >> 4) & 0x3;
  uint32_t phy_duplex = (physr >> 3) & 0x1;
  uint32_t phy_link = (physr >> 2) & 0x1;

  uint32_t phy_speed_mbps;
  const char *phy_speed_str;
  if (phy_speed_bits == 2) { phy_speed_mbps = 1000; phy_speed_str = "1000 Mbps"; }
  else if (phy_speed_bits == 1) { phy_speed_mbps = 100; phy_speed_str = "100 Mbps"; }
  else { phy_speed_mbps = 10; phy_speed_str = "10 Mbps"; }

  tm_printf("\r\n[TX DIAG] ===== SPEED/DUPLEX MISMATCH CHECK =====\r\n");
  tm_printf("[TX DIAG] MAC  MACCR=0x%08lX: Speed=%s, Duplex=%s, TE=%lu, RE=%lu\r\n",
            (unsigned long)maccr, mac_speed_str, mac_dm ? "Full" : "Half",
            (unsigned long)mac_te, (unsigned long)mac_re);
  tm_printf("[TX DIAG] PHY  PHYSR=0x%04lX: Speed=%s, Duplex=%s, Link=%s\r\n",
            (unsigned long)physr, phy_speed_str, phy_duplex ? "Full" : "Half",
            phy_link ? "UP" : "DOWN");

  if (mac_speed_mbps != phy_speed_mbps) {
    tm_printf("[TX DIAG] *** CRITICAL MISMATCH! MAC=%lu Mbps vs PHY=%lu Mbps ***\r\n",
              (unsigned long)mac_speed_mbps, (unsigned long)phy_speed_mbps);
    tm_printf("[TX DIAG] *** This WILL cause all TX frames to be silently dropped! ***\r\n");
  } else {
    tm_printf("[TX DIAG] Speed matches: %lu Mbps -- OK\r\n", (unsigned long)mac_speed_mbps);
  }

  if (mac_dm != phy_duplex) {
    tm_printf("[TX DIAG] *** DUPLEX MISMATCH! MAC=%s vs PHY=%s ***\r\n",
              mac_dm ? "Full" : "Half", phy_duplex ? "Full" : "Half");
  } else {
    tm_printf("[TX DIAG] Duplex matches: %s -- OK\r\n", mac_dm ? "Full" : "Half");
  }

  if (!mac_te) {
    tm_printf("[TX DIAG] *** CRITICAL: MAC Transmitter is DISABLED (TE=0)! ***\r\n");
  }
}

/**
 * @brief  Read MAC MMC hardware TX/RX frame counters.
 *         These count frames at the MAC level, independent of software.
 *         If MMCTPCGR == 0 but software sent many frames, frames never left the MAC.
 */
static void dump_mmc_hw_counters(void) {
  if (heth1.Instance == NULL) return;

  /* First, reset counters on first call so we get fresh values */
  uint32_t tx_good    = heth1.Instance->MMCTPCGR;     /* TX packet count good (offset 0x768) */
  uint32_t tx_scol    = heth1.Instance->MMCTSCGPR;     /* TX single collision (offset 0x74C) */
  uint32_t tx_mcol    = heth1.Instance->MMCTMCGPR;     /* TX multiple collision (offset 0x750) */
  uint32_t rx_crc_err = heth1.Instance->MMCRCRCEPR;    /* RX CRC error (offset 0x794) */
  uint32_t rx_align   = heth1.Instance->MMCRAEPR;      /* RX alignment error (offset 0x798) */
  uint32_t rx_uni     = heth1.Instance->MMCRUPGR;      /* RX unicast good (offset 0x7C4) */

  tm_printf("[TX DIAG] ===== MAC MMC HARDWARE COUNTERS =====\r\n");
  tm_printf("[TX DIAG] TX Good Packets (HW counter) : %lu\r\n", (unsigned long)tx_good);
  tm_printf("[TX DIAG] TX Single Collision           : %lu\r\n", (unsigned long)tx_scol);
  tm_printf("[TX DIAG] TX Multiple Collision          : %lu\r\n", (unsigned long)tx_mcol);
  tm_printf("[TX DIAG] RX CRC Errors                  : %lu\r\n", (unsigned long)rx_crc_err);
  tm_printf("[TX DIAG] RX Alignment Errors            : %lu\r\n", (unsigned long)rx_align);
  tm_printf("[TX DIAG] RX Unicast Good                : %lu\r\n", (unsigned long)rx_uni);
  tm_printf("[TX DIAG] Software TX count              : %lu\r\n", (unsigned long)g_tx_pkt_count);

  if (g_tx_pkt_count > 0 && tx_good == 0) {
    tm_printf("[TX DIAG] *** CRITICAL: Software sent %lu frames but MAC HW counter is 0! ***\r\n",
              (unsigned long)g_tx_pkt_count);
    tm_printf("[TX DIAG] *** Frames are being DMA'd but NOT leaving the MAC! ***\r\n");
  }
}

/**
 * @brief  Read MTL TX Queue debug register to see if frames are stuck.
 */
static void dump_mtl_tx_debug(void) {
  if (heth1.Instance == NULL) return;

  uint32_t mtltxqdr = heth1.Instance->MTL_QUEUE[0].MTLTXQDR;
  uint32_t mtltxqomr = heth1.Instance->MTL_QUEUE[0].MTLTXQOMR;
  uint32_t mtltxqur = heth1.Instance->MTL_QUEUE[0].MTLTXQUR;

  /* Decode MTLTXQDR fields (STM32 SYNOPSYS DWC ETH IP) */
  uint32_t txqsts  = (mtltxqdr >> 4) & 0x1;   /* TX Queue Not Empty Status */
  uint32_t trcsts  = (mtltxqdr >> 1) & 0x3;   /* TX Read Controller Status */
  uint32_t twcsts  = mtltxqdr & 0x1;           /* TX Write Controller Status */
  uint32_t ptxq    = (mtltxqdr >> 16) & 0x7;   /* Packets in TX Queue */
  uint32_t stxstsf = (mtltxqdr >> 20) & 0x7;   /* Status FIFO Full/Space */

  const char *trcsts_str;
  switch (trcsts) {
    case 0: trcsts_str = "Idle"; break;
    case 1: trcsts_str = "Read (transferring to MAC)"; break;
    case 2: trcsts_str = "Waiting for status"; break;
    case 3: trcsts_str = "Flushing"; break;
    default: trcsts_str = "Unknown"; break;
  }

  tm_printf("[TX DIAG] ===== MTL TX QUEUE 0 DEBUG =====\r\n");
  tm_printf("[TX DIAG] MTLTXQDR=0x%08lX: PktsInQueue=%lu, QueueNotEmpty=%lu, ReadCtrl=%s, WriteActive=%lu\r\n",
            (unsigned long)mtltxqdr, (unsigned long)ptxq, (unsigned long)txqsts,
            trcsts_str, (unsigned long)twcsts);
  tm_printf("[TX DIAG] MTLTXQOMR=0x%08lX (TxQ Operating Mode), MTLTXQUR=0x%08lX (Underflow count)\r\n",
            (unsigned long)mtltxqomr, (unsigned long)mtltxqur);

  if (mtltxqur != 0) {
    tm_printf("[TX DIAG] *** WARNING: TX Queue Underflow detected! Count=%lu ***\r\n",
              (unsigned long)mtltxqur);
  }
}

/**
 * @brief  Read RTL8211F RGMII TX/RX delay configuration.
 *         Page 0xd08, Register 0x11 controls RGMII delays.
 *         Incorrect TX delay = frames garbled at PHY output.
 */
static void dump_rtl8211f_rgmii_delay(void) {
  uint32_t page_d08_reg11 = 0;
  uint32_t page_d08_reg15 = 0;

  /* Switch to page 0xd08 */
  HAL_ETH_WritePHYRegister(&heth1, 1, 31, 0x0d08);
  HAL_ETH_ReadPHYRegister(&heth1, 1, 0x11, &page_d08_reg11);
  HAL_ETH_ReadPHYRegister(&heth1, 1, 0x15, &page_d08_reg15);
  /* Switch back to page 0 */
  HAL_ETH_WritePHYRegister(&heth1, 1, 31, 0x0000);

  uint32_t txdly = (page_d08_reg11 >> 8) & 0x1;  /* Bit 8: TX Delay Enable */
  uint32_t rxdly = (page_d08_reg11 >> 3) & 0x1;  /* Bit 3: RX Delay Enable */

  tm_printf("[TX DIAG] ===== RTL8211F RGMII DELAY CONFIG =====\r\n");
  tm_printf("[TX DIAG] Page 0xd08 Reg 0x11 = 0x%04lX\r\n", (unsigned long)page_d08_reg11);
  tm_printf("[TX DIAG]   TX Delay (bit 8): %s\r\n", txdly ? "ENABLED (2ns added)" : "DISABLED");
  tm_printf("[TX DIAG]   RX Delay (bit 3): %s\r\n", rxdly ? "ENABLED (2ns added)" : "DISABLED");
  tm_printf("[TX DIAG] Page 0xd08 Reg 0x15 = 0x%04lX (RGMII timing fine-tune)\r\n",
            (unsigned long)page_d08_reg15);

  /* For RGMII, typically TX delay should be ENABLED by the PHY when the MAC
     doesn't add it. If TX delay is disabled and MAC doesn't add delay,
     data will be clocked incorrectly at the switch/router. */
  if (!txdly) {
    tm_printf("[TX DIAG] *** NOTE: TX Delay is DISABLED. If MAC doesn't add RGMII TX delay, ***\r\n");
    tm_printf("[TX DIAG] *** frames will be garbled at the PHY output! ***\r\n");
  }
}

/**
 * @brief  Master TX diagnostic function - calls all sub-diagnostics.
 *         Call this periodically or after ARP reply to isolate TX issues.
 */
void ethernetif_dump_tx_diagnostics(void) {
  tm_printf("\r\n################################################################\r\n");
  tm_printf("###          TX PATH DEEP DIAGNOSTICS REPORT                ###\r\n");
  tm_printf("################################################################\r\n");
  dump_tx_speed_mismatch_check();
  dump_mmc_hw_counters();
  dump_mtl_tx_debug();
  dump_rtl8211f_rgmii_delay();
  tm_printf("################################################################\r\n\r\n");
}

/**
 * @brief Dump RX Descriptor state and DMA RX status.
 */
void dump_rx_desc_state(const char *moment) {
  uint32_t cur = heth1.RxDescList[0].RxDescIdx;
  ETH_DMADescTypeDef *desc = (ETH_DMADescTypeDef *)heth1.RxDescList[0].RxDesc[cur];

  if (desc != NULL) {
    tm_printf("[RX DESC %s] Idx=%lu Addr=0x%08lX | DESC0=0x%08lX DESC1=0x%08lX DESC2=0x%08lX DESC3=0x%08lX\r\n",
              moment, (unsigned long)cur, (unsigned long)(uintptr_t)desc,
              (unsigned long)desc->DESC0, (unsigned long)desc->DESC1,
              (unsigned long)desc->DESC2, (unsigned long)desc->DESC3);
    tm_printf("            OWN=%s | FD=%lu | LD=%lu | PL=%lu bytes\r\n",
              (desc->DESC3 & ETH_DMARXNDESCWBF_OWN) ? "1 [DMA OWNS - Waiting for frame]" : "0 [CPU OWNS - Frame received!]",
              (unsigned long)((desc->DESC3 & ETH_DMARXNDESCWBF_FD) != 0),
              (unsigned long)((desc->DESC3 & ETH_DMARXNDESCWBF_LD) != 0),
              (unsigned long)(desc->DESC3 & ETH_DMARXNDESCWBF_PL));
  }

  if (heth1.Instance != NULL) {
    tm_printf("            DMACCARXDR=0x%08lX | DMACCARXBR=0x%08lX | DMACSR=0x%08lX\r\n",
              (unsigned long)heth1.Instance->DMA_CH[0].DMACCARXDR,
              (unsigned long)heth1.Instance->DMA_CH[0].DMACCARXBR,
              (unsigned long)heth1.Instance->DMA_CH[0].DMACSR);
  }
}

/**
 * @brief Checks PHY link state, auto-negotiates speed/duplex, and adjusts MACCR dynamically.
 * @retval 1 if link is UP, 0 if link is DOWN
 */
int ethernetif_check_link_and_speed(struct netif *netif) {
  static uint32_t s_last_link = 0xFF;
  static uint32_t s_last_speed = 0xFF;
  static uint32_t s_last_duplex = 0xFF;

  uint32_t bmsr = 0, anar = 0, anlpar = 0, gbcr = 0, gbsr = 0, physr = 0;
  HAL_ETH_ReadPHYRegister(&heth1, 1, 1, &bmsr); /* Clear latch-low */
  HAL_ETH_ReadPHYRegister(&heth1, 1, 1, &bmsr); /* Real-time link */
  HAL_ETH_ReadPHYRegister(&heth1, 1, 4, &anar);
  HAL_ETH_ReadPHYRegister(&heth1, 1, 5, &anlpar);
  HAL_ETH_ReadPHYRegister(&heth1, 1, 9, &gbcr);
  HAL_ETH_ReadPHYRegister(&heth1, 1, 10, &gbsr);

  /* RTL8211F Page 0x0a43 Register 0x1A read */
  HAL_ETH_WritePHYRegister(&heth1, 1, 31, 0x0a43);
  HAL_ETH_ReadPHYRegister(&heth1, 1, 0x1A, &physr);
  HAL_ETH_WritePHYRegister(&heth1, 1, 31, 0x0000);

  uint32_t link_up = (bmsr & 0x0004) ? 1 : 0;
  uint32_t speed_mbps = 0;
  uint32_t is_full_duplex = 1;

  if (link_up) {
    /* 1. Try RTL8211F specific status register (Page 0xa43, reg 0x1A) */
    uint32_t rtl_speed = (physr >> 4) & 0x3;
    uint32_t rtl_duplex = (physr >> 3) & 0x1;
    uint32_t rtl_link = (physr >> 2) & 0x1;

    if (rtl_link) {
      if (rtl_speed == 2) speed_mbps = 1000;
      else if (rtl_speed == 1) speed_mbps = 100;
      else if (rtl_speed == 0) speed_mbps = 10;
      is_full_duplex = rtl_duplex;
    } else {
      /* 2. Standard IEEE 802.3 resolution */
      if ((gbsr & 0x0800) && (gbcr & 0x0200)) {
        speed_mbps = 1000;
        is_full_duplex = 1;
      } else if ((gbsr & 0x0400) && (gbcr & 0x0100)) {
        speed_mbps = 1000;
        is_full_duplex = 0;
      } else if ((anar & 0x0100) && (anlpar & 0x0100)) {
        speed_mbps = 100;
        is_full_duplex = 1;
      } else if ((anar & 0x0080) && (anlpar & 0x0080)) {
        speed_mbps = 100;
        is_full_duplex = 0;
      } else if ((anar & 0x0040) && (anlpar & 0x0040)) {
        speed_mbps = 10;
        is_full_duplex = 1;
      } else {
        speed_mbps = 10;
        is_full_duplex = 0;
      }
    }
  }

  /* Check if state changed */
  if (link_up != s_last_link || speed_mbps != s_last_speed || is_full_duplex != s_last_duplex) {
    s_last_link = link_up;
    s_last_speed = speed_mbps;
    s_last_duplex = is_full_duplex;

    if (link_up) {
      /* Update MACCR based on negotiated speed */
      if (speed_mbps == 1000) {
        CLEAR_BIT(heth1.Instance->MACCR, ETH_MACCR_PS | ETH_MACCR_FES);
      } else if (speed_mbps == 100) {
        SET_BIT(heth1.Instance->MACCR, ETH_MACCR_PS | ETH_MACCR_FES);
      } else {
        SET_BIT(heth1.Instance->MACCR, ETH_MACCR_PS);
        CLEAR_BIT(heth1.Instance->MACCR, ETH_MACCR_FES);
      }

      if (is_full_duplex) {
        SET_BIT(heth1.Instance->MACCR, ETH_MACCR_DM);
      } else {
        CLEAR_BIT(heth1.Instance->MACCR, ETH_MACCR_DM);
      }

      tm_printf("\r\n=============================================================\r\n");
      tm_printf("[ETH LINK STATE] >>> LINK UP: %lu Mbps, %s Duplex <<<\r\n",
                (unsigned long)speed_mbps, is_full_duplex ? "Full" : "Half");
      tm_printf("                 MACCR updated to: 0x%08lX (PS=%lu, FES=%lu, DM=%lu)\r\n",
                (unsigned long)heth1.Instance->MACCR,
                (unsigned long)((heth1.Instance->MACCR & ETH_MACCR_PS) != 0),
                (unsigned long)((heth1.Instance->MACCR & ETH_MACCR_FES) != 0),
                (unsigned long)((heth1.Instance->MACCR & ETH_MACCR_DM) != 0));
      tm_printf("                 BMSR=0x%04lX, ANLPAR=0x%04lX, GBSR=0x%04lX, RTL_PHYSR=0x%04lX\r\n",
                (unsigned long)bmsr, (unsigned long)anlpar, (unsigned long)gbsr, (unsigned long)physr);
      tm_printf("=============================================================\r\n\r\n");

      if (netif != NULL && !netif_is_link_up(netif)) {
        netif_set_link_up(netif);
      }
    } else {
      tm_printf("\r\n[ETH LINK STATE] >>> LINK DOWN: Cable unplugged or no link signal! <<<\r\n");
      tm_printf("                 BMSR=0x%04lX\r\n", (unsigned long)bmsr);
      if (netif != NULL && netif_is_link_up(netif)) {
        netif_set_link_down(netif);
      }
    }
  }

  return (int)link_up;
}

/**
 * @brief  Full diagnostic report on MPU, memory layout, ETH registers, and PHY.
 */
void ethernetif_dump_diagnostics(void) {
  tm_printf(
      "\r\n=============================================================\r\n");
  tm_printf(
      "           ETHERNET & MPU HARDWARE DIAGNOSTICS               \r\n");
  tm_printf(
      "=============================================================\r\n");

  /* 1. MPU Registers */
  uint32_t mpu_ctrl = MPU->CTRL;
  tm_printf("[MPU] CTRL: 0x%08lX (MPU Enabled: %s, Privileged Default: %s)\r\n",
            (unsigned long)mpu_ctrl,
            (mpu_ctrl & MPU_CTRL_ENABLE_Msk) ? "YES" : "NO",
            (mpu_ctrl & MPU_CTRL_PRIVDEFENA_Msk) ? "YES" : "NO");

  MPU->RNR = 0;
  uint32_t rbar = MPU->RBAR;
  uint32_t rlar = MPU->RLAR;
  uint32_t mair0 = MPU->MAIR0;
  uint32_t base_addr = rbar & ~0x1FUL;
  uint32_t limit_addr = (rlar & ~0x1FUL) | 0x1FUL;
  tm_printf("[MPU] Region 0: RBAR=0x%08lX (Base=0x%08lX)\r\n",
            (unsigned long)rbar, (unsigned long)base_addr);
  tm_printf(
      "                RLAR=0x%08lX (Limit=0x%08lX, AttrIndex=%lu, %s)\r\n",
      (unsigned long)rlar, (unsigned long)limit_addr,
      (unsigned long)((rlar >> 1) & 0x7),
      (rlar & 0x1) ? "ENABLED" : "DISABLED");
  tm_printf("[MPU] MAIR0   : 0x%08lX (Attr0 Byte=0x%02X)\r\n",
            (unsigned long)mair0, (unsigned)(mair0 & 0xFF));

  /* 2. Descriptor & Buffer Locations */
  void *tx_desc_addr = (void *)heth1.Init.TxDesc[0];
  void *rx_desc_addr = (void *)heth1.Init.RxDesc[0];
  tm_printf("[MEM] TxDesc[0] Addr : 0x%08lX\r\n",
            (unsigned long)(uintptr_t)tx_desc_addr);
  tm_printf("[MEM] RxDesc[0] Addr : 0x%08lX\r\n",
            (unsigned long)(uintptr_t)rx_desc_addr);

  uint32_t tx_val = (uint32_t)(uintptr_t)tx_desc_addr;
  if ((rlar & 0x1) && tx_val >= base_addr && tx_val <= limit_addr) {
    tm_printf(
        "[MEM CHECK] TxDesc is INSIDE Non-Cacheable MPU Region 0 -> OK!\r\n");
  } else {
    tm_printf("[MEM WARNING!] TxDesc (0x%08lX) is OUTSIDE MPU Region 0 "
              "[0x%08lX - 0x%08lX]!\r\n",
              (unsigned long)tx_val, (unsigned long)base_addr,
              (unsigned long)limit_addr);
  }

  /* 3. Ethernet MAC and DMA Hardware Registers */
  if (heth1.Instance != NULL) {
    tm_printf("[ETH HW] MAC Registers:\r\n");
    tm_printf("         MACCR      (MAC Configuration) : 0x%08lX (PS=%lu, FES=%lu, DM=%lu, TE=%lu, RE=%lu)\r\n",
              (unsigned long)heth1.Instance->MACCR,
              (unsigned long)((heth1.Instance->MACCR & ETH_MACCR_PS) != 0),
              (unsigned long)((heth1.Instance->MACCR & ETH_MACCR_FES) != 0),
              (unsigned long)((heth1.Instance->MACCR & ETH_MACCR_DM) != 0),
              (unsigned long)((heth1.Instance->MACCR & ETH_MACCR_TE) != 0),
              (unsigned long)((heth1.Instance->MACCR & ETH_MACCR_RE) != 0));
    tm_printf("         MACPFR     (Packet Filter)     : 0x%08lX (RA=%lu, PR=%lu, PM=%lu, DBF=%lu)\r\n",
              (unsigned long)heth1.Instance->MACPFR,
              (unsigned long)((heth1.Instance->MACPFR & ETH_MACPFR_RA) != 0),
              (unsigned long)((heth1.Instance->MACPFR & ETH_MACPFR_PR) != 0),
              (unsigned long)((heth1.Instance->MACPFR & ETH_MACPFR_PM) != 0),
              (unsigned long)((heth1.Instance->MACPFR & ETH_MACPFR_DBF) != 0));
    tm_printf("         MACA0HR:LR (Hardware MAC Addr) : %02lX:%02lX:%02lX:%02lX:%02lX:%02lX\r\n",
              (unsigned long)(heth1.Instance->MACA0LR & 0xFF),
              (unsigned long)((heth1.Instance->MACA0LR >> 8) & 0xFF),
              (unsigned long)((heth1.Instance->MACA0LR >> 16) & 0xFF),
              (unsigned long)((heth1.Instance->MACA0LR >> 24) & 0xFF),
              (unsigned long)(heth1.Instance->MACA0HR & 0xFF),
              (unsigned long)((heth1.Instance->MACA0HR >> 8) & 0xFF));

    tm_printf("[ETH HW] DMA Channel 0 Registers:\r\n");
    tm_printf("         DMACTXDLAR (Tx Desc List Base) : 0x%08lX\r\n",
              (unsigned long)heth1.Instance->DMA_CH[0].DMACTXDLAR);
    tm_printf("         DMACRXDLAR (Rx Desc List Base) : 0x%08lX\r\n",
              (unsigned long)heth1.Instance->DMA_CH[0].DMACRXDLAR);
    tm_printf("         DMACTXCR   (Tx Control)        : 0x%08lX (Tx %s)\r\n",
              (unsigned long)heth1.Instance->DMA_CH[0].DMACTXCR,
              (heth1.Instance->DMA_CH[0].DMACTXCR & 0x1) ? "STARTED" : "STOPPED");
    tm_printf("         DMACRXCR   (Rx Control)        : 0x%08lX (Rx %s)\r\n",
              (unsigned long)heth1.Instance->DMA_CH[0].DMACRXCR,
              (heth1.Instance->DMA_CH[0].DMACRXCR & 0x1) ? "STARTED" : "STOPPED");
    tm_printf("         DMACSR     (DMA Status)        : 0x%08lX\r\n",
              (unsigned long)heth1.Instance->DMA_CH[0].DMACSR);
    tm_printf("         DMACCATXDR (Current Tx Desc)   : 0x%08lX\r\n",
              (unsigned long)heth1.Instance->DMA_CH[0].DMACCATXDR);
    tm_printf("         DMACCARXDR (Current Rx Desc)   : 0x%08lX\r\n",
              (unsigned long)heth1.Instance->DMA_CH[0].DMACCARXDR);
  }

  /* 4. Realtek RTL8211 PHY Registers on Address 1 */
  uint32_t bmcr = 0, bmsr = 0, phyid1 = 0, phyid2 = 0, anar = 0, anlpar = 0, gbcr = 0, gbsr = 0, physr = 0;
  HAL_ETH_ReadPHYRegister(&heth1, 1, 0, &bmcr);
  HAL_ETH_ReadPHYRegister(&heth1, 1, 1, &bmsr); /* First read clears latch-low */
  HAL_ETH_ReadPHYRegister(&heth1, 1, 1, &bmsr); /* Second read gives current real-time link */
  HAL_ETH_ReadPHYRegister(&heth1, 1, 2, &phyid1);
  HAL_ETH_ReadPHYRegister(&heth1, 1, 3, &phyid2);
  HAL_ETH_ReadPHYRegister(&heth1, 1, 4, &anar);
  HAL_ETH_ReadPHYRegister(&heth1, 1, 5, &anlpar);
  HAL_ETH_ReadPHYRegister(&heth1, 1, 9, &gbcr);
  HAL_ETH_ReadPHYRegister(&heth1, 1, 10, &gbsr);

  HAL_ETH_WritePHYRegister(&heth1, 1, 31, 0x0a43);
  HAL_ETH_ReadPHYRegister(&heth1, 1, 0x1A, &physr);
  HAL_ETH_WritePHYRegister(&heth1, 1, 31, 0x0000);

  uint32_t speed_bits = (physr >> 4) & 0x3;
  const char *speed_str = (speed_bits == 2)   ? "1000 Mbps (Gigabit)"
                          : (speed_bits == 1) ? "100 Mbps (Fast Ethernet)"
                          : (speed_bits == 0) ? "10 Mbps"
                                              : "Auto-detecting";
  uint32_t duplex_bit = (physr >> 3) & 0x1;

  tm_printf("[ETH PHY] Addr 1: BMCR=0x%04X, BMSR=0x%04X, ANLPAR=0x%04X, GBSR=0x%04X\r\n",
            (unsigned)bmcr, (unsigned)bmsr, (unsigned)anlpar, (unsigned)gbsr);
  tm_printf("          PHY_ID=0x%04X:0x%04X, RTL_PAGE_A43_PHYSR=0x%04X\r\n",
            (unsigned)phyid1, (unsigned)phyid2, (unsigned)physr);
  tm_printf("          Link Status : %s\r\n",
            (bmsr & 0x0004) ? "LINK UP (Cable connected!)" : "LINK DOWN");
  tm_printf("          Auto-Neg    : %s\r\n",
            (bmsr & 0x0020) ? "COMPLETED" : "IN PROGRESS");
  tm_printf("          Negotiated  : %s, %s Duplex\r\n", speed_str,
            duplex_bit ? "Full" : "Half");
  tm_printf(
      "=============================================================\r\n\r\n");
}

/* ========================================================================= */
/* RX & TX BUFFER ALLOCATION                                                 */
/* ========================================================================= */
#define ETH_RX_BUFFER_CNT (ETH_RX_DESC_CNT * 2)
#define ETH_RX_BUFFER_SIZE 1536

/* Buffers mapped directly into Non-Cacheable MPU Region 0 (0x34100000 - 0x3410FFFF) */
#define RX_BUFFERS_BASE_ADDR 0x34101000UL
#define TX_BUFFERS_BASE_ADDR 0x34104000UL

static uint8_t (*RxBuffers)[ETH_RX_BUFFER_SIZE] =
    (uint8_t (*)[ETH_RX_BUFFER_SIZE])RX_BUFFERS_BASE_ADDR;
static uint32_t rx_alloc_idx = 0;

/**
 * @brief  HAL ETH Rx Allocate Callback.
 */
void HAL_ETH_RxAllocateCallback(uint8_t **buff) {
  *buff = RxBuffers[rx_alloc_idx];
  rx_alloc_idx = (rx_alloc_idx + 1) % ETH_RX_BUFFER_CNT;
}

/**
 * @brief  HAL ETH Rx Link Callback.
 */
void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff,
                            uint16_t Length) {
  (void)Length;
  if (*pStart == NULL) {
    *pStart = buff;
  }
  *pEnd = buff;
}

#include "lwip/sys.h"
static uint8_t (*TxBuffers)[1536] = (uint8_t (*)[1536])TX_BUFFERS_BASE_ADDR;
static ETH_BufferTypeDef Txbuffer[ETH_TX_DESC_CNT];
static sys_mutex_t tx_mutex;
static uint8_t tx_mutex_init = 0;
static uint32_t tx_idx = 0;

/* ========================================================================= */
/* 1. LOW LEVEL OUTPUT (TRANSMIT)                                            */
/* ========================================================================= */
static err_t low_level_output(struct netif *netif, struct pbuf *p) {
  (void)netif;

  if (p == NULL || p->tot_len > 1514) {
    return ERR_BUF;
  }

  sys_mutex_lock(&tx_mutex);

  /* Check if current descriptor is currently owned by DMA */
  ETH_DMADescTypeDef *desc = (ETH_DMADescTypeDef *)heth1.TxDescList[0].TxDesc[tx_idx];
  uint32_t wait_count = 0;
  while ((desc->DESC3 & ETH_DMATXNDESCWBF_OWN) != 0) {
    if (++wait_count > 2000) {
      /* Timeout: drop packet rather than freezing CPU */
      sys_mutex_unlock(&tx_mutex);
      return ERR_IF;
    }
  }

  /* Copy pbuf payload directly into contiguous DMA TX buffer */
  pbuf_copy_partial(p, TxBuffers[tx_idx], p->tot_len, 0);

  Txbuffer[tx_idx].buffer = TxBuffers[tx_idx];
  Txbuffer[tx_idx].len = p->tot_len;
  Txbuffer[tx_idx].next = NULL;

  ETH_TxPacketConfigTypeDef TxConfig = {0};
  TxConfig.Length = p->tot_len;
  TxConfig.TxBuffer = &Txbuffer[tx_idx];
  TxConfig.Attributes = ETH_TX_PACKETS_FEATURES_CRCPAD;
  TxConfig.CRCPadCtrl = ETH_CRC_PAD_INSERT;

  /* Use non-blocking transmit (returns immediately after submitting descriptor to DMA) */
  HAL_StatusTypeDef status = HAL_ETH_Transmit_IT(&heth1, &TxConfig);

  if (status != HAL_OK) {
    sys_mutex_unlock(&tx_mutex);
    return ERR_IF;
  }

  tx_idx = (tx_idx + 1) % ETH_TX_DESC_CNT;
  g_tx_pkt_count++;
  sys_mutex_unlock(&tx_mutex);
  return ERR_OK;
}

/**
 * @brief  Sends a raw Ethernet frame directly via DMA.
 */
int8_t ethernetif_send_raw(const uint8_t *data, uint16_t len) {
  static uint32_t s_raw_log_cnt = 0;

  if (data == NULL || len == 0 || len > 1514) {
    return -1;
  }

  sys_mutex_lock(&tx_mutex);

  memcpy(TxBuffers[tx_idx], data, len);

  Txbuffer[tx_idx].buffer = TxBuffers[tx_idx];
  Txbuffer[tx_idx].len = len;
  Txbuffer[tx_idx].next = NULL;

  ETH_TxPacketConfigTypeDef TxConfig = {0};
  TxConfig.Length = len;
  TxConfig.TxBuffer = &Txbuffer[tx_idx];
  TxConfig.Attributes = ETH_TX_PACKETS_FEATURES_CRCPAD;
  TxConfig.CRCPadCtrl = ETH_CRC_PAD_INSERT;

  HAL_StatusTypeDef status = HAL_ETH_Transmit(&heth1, &TxConfig, 100);

  if (status != HAL_OK) {
    sys_mutex_unlock(&tx_mutex);
    return -1;
  }

  tx_idx = (tx_idx + 1) % ETH_TX_DESC_CNT;
  g_tx_pkt_count++;
  sys_mutex_unlock(&tx_mutex);
  return 0;
}

/* ========================================================================= */
/* 2. LOW LEVEL INPUT (RECEIVE)                                              */
/* ========================================================================= */
static struct pbuf *low_level_input(struct netif *netif) {
  (void)netif;
  struct pbuf *p = NULL;
  void *appBuff = NULL;

  /* Check if packet was received by DMA */
  if (HAL_ETH_ReadData(&heth1, &appBuff) == HAL_OK) {
    uint32_t framelength = heth1.RxDescList[heth1.RxOpCH].RxDataLength;

    if (framelength > 0 && framelength <= 1514 && appBuff != NULL) {
      g_rx_pkt_count++;

      /* Allocate LwIP memory */
      p = pbuf_alloc(PBUF_RAW, (u16_t)framelength, PBUF_POOL);
      if (p != NULL) {
        pbuf_take(p, appBuff, (u16_t)framelength);
      } else {
        tm_printf("[ETH RX ERROR] Failed to allocate LwIP pbuf for %lu bytes!\r\n",
                  (unsigned long)framelength);
      }
    }
  }

  return p;
}

/* ========================================================================= */
/* 3. LOW LEVEL INITIALIZATION                                               */
/* ========================================================================= */
static void low_level_init(struct netif *netif) {
  netif->hwaddr_len = ETH_HWADDR_LEN;

  if (!tx_mutex_init) {
    sys_mutex_new(&tx_mutex);
    tx_mutex_init = 1;
  }

  netif->hwaddr[0] = heth1.Init.MACAddr[0];
  netif->hwaddr[1] = heth1.Init.MACAddr[1];
  netif->hwaddr[2] = heth1.Init.MACAddr[2];
  netif->hwaddr[3] = heth1.Init.MACAddr[3];
  netif->hwaddr[4] = heth1.Init.MACAddr[4];
  netif->hwaddr[5] = heth1.Init.MACAddr[5];

  netif->mtu = 1500;
  netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;

  /* Disable RGMII TX Delay in the RTL8211F PHY to fix clock misalignment.
   * Page 0xd08, Reg 0x11, Bit 8 = 0 (TX delay disable), Bit 3 = 1 (RX delay enable).
   * Writing 0x0009 to register 0x11 sets this configuration. */
  HAL_ETH_WritePHYRegister(&heth1, 1, 31, 0x0d08);      /* Switch to page 0xd08 */
  HAL_ETH_WritePHYRegister(&heth1, 1, 0x11, 0x0009);    /* Clear bit 8, keep bit 3 */
  HAL_ETH_WritePHYRegister(&heth1, 1, 31, 0x0000);      /* Switch back to page 0 */

  /* Start Ethernet DMA engine */
  HAL_ETH_Start(&heth1);

  /* Configure STM32N6 internal RGMII TX Clock delay (2.75 ns) 
   * The user requested a 2.75ns lock for the final test. */
  GPIO_DelayTypeDef DelayInit = {0};
  DelayInit.Delay = GPIO_DELAY_PS_2750;
  DelayInit.Path = GPIO_PATH_OUT;
  HAL_GPIO_SetDelay(GPIOF, GPIO_PIN_0, &DelayInit);

  /* Configure MAC Packet Filter:
   * Disable Promiscuous mode, Receive All, and Pass All Multicast so the hardware 
   * automatically filters out background network noise (Broadcast storms).
   * Ensure Disable Broadcast (DBF) is CLEARED so broadcast ARP is NEVER dropped. */
  MODIFY_REG(heth1.Instance->MACPFR,
             ETH_MACPFR_DBF | ETH_MACPFR_RA | ETH_MACPFR_PR | ETH_MACPFR_PM,
             0);

  /* Wait up to 2.5 seconds for PHY Auto-Negotiation and Link UP */
  uint32_t bmsr = 0;
  for (uint32_t i = 0; i < 25; i++) {
    HAL_ETH_ReadPHYRegister(&heth1, 1, 1, &bmsr); /* Clear latch-low */
    HAL_ETH_ReadPHYRegister(&heth1, 1, 1, &bmsr); /* Read current status */
    if (bmsr & 0x0004) {
      break;
    }
    HAL_Delay(100);
  }

  /* Detect negotiated speed and update MACCR dynamically */
  ethernetif_check_link_and_speed(netif);

  /* Print full diagnostic report */
  ethernetif_dump_diagnostics();
}

/* ========================================================================= */
/* 4. LWIP STACK FEEDER                                                      */
/* ========================================================================= */
void ethernetif_input(struct netif *netif) {
  struct pbuf *p;

  /* Drain all received packets waiting in Ethernet DMA descriptors */
  do {
    p = low_level_input(netif);
    if (p != NULL) {
      err_t err = netif->input(p, netif);
      if (err != ERR_OK) {
        tm_printf("[LwIP INPUT ERROR] netif->input failed with error %d\r\n", (int)err);
        pbuf_free(p);
      }
    }
  } while (p != NULL);
}

/* ========================================================================= */
/* 5. LWIP INTERFACE INITIALIZATION                                          */
/* ========================================================================= */
err_t ethernetif_init(struct netif *netif) {
  LWIP_ASSERT("netif != NULL", (netif != NULL));

#if LWIP_NETIF_HOSTNAME
  netif->hostname = "stm32n6_tron";
#endif

  netif->name[0] = IFNAME0;
  netif->name[1] = IFNAME1;

  netif->output = etharp_output;
  netif->linkoutput = low_level_output;

  low_level_init(netif);

  return ERR_OK;
}
