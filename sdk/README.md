# libbcm56846 — Custom BCM56846 SDK

Our replacement for the Broadcom proprietary SDK. Written entirely from reverse-engineered data.
No Broadcom SDK source code is used or derived here.

## What This Is

A C library providing the minimum API surface needed to:
1. Initialize the BCM56846 (Trident+) ASIC
2. Bring up ports (XLPORT/MAC + Warpcore WC-B0 SerDes)
3. Program L2 forwarding tables (L2_ENTRY, L2_USER_ENTRY)
4. Program L3 routing tables (L3_DEFIP, ECMP, nexthop chain)
5. Send and receive packets via DMA (for CPU/control plane traffic)
6. Configure VLANs

## API Overview

```c
/* Init */
int bcm56846_attach(int unit);
int bcm56846_init(int unit, const char *config_path);
void bcm56846_detach(int unit);

/* Port */
int bcm56846_port_enable_set(int unit, int port, int enable);
int bcm56846_port_speed_set(int unit, int port, int speed_mbps);
int bcm56846_port_link_status_get(int unit, int port, int *link_up);

/* SerDes (WARPcore WC-B0) */
int bcm56846_serdes_init_10g(int unit, int port);       /* 10G SFI init */
int bcm56846_serdes_link_get(int unit, int port, int *link_up); /* CL45 PCS link */

/* L2 */
int bcm56846_l2_addr_add(int unit, const bcm56846_l2_addr_t *addr);
int bcm56846_l2_addr_delete(int unit, const uint8_t mac[6], uint16_t vid);
int bcm56846_l2_addr_get(int unit, const uint8_t mac[6], uint16_t vid, bcm56846_l2_addr_t *out);

/* L2 TCAM (L2_USER_ENTRY) */
int bcm56846_l2_user_entry_add(int unit, const bcm56846_l2_user_addr_t *addr, int *index);
int bcm56846_l2_user_entry_delete(int unit, int index);

/* L3 Interface */
int bcm56846_l3_intf_create(int unit, const uint8_t mac[6], uint16_t vid, int *intf_id);
int bcm56846_l3_intf_destroy(int unit, int intf_id);

/* L3 Egress */
int bcm56846_l3_egress_create(int unit, const bcm56846_l3_egress_t *egress, int *egress_id);
int bcm56846_l3_egress_destroy(int unit, int egress_id);

/* L3 Routes */
int bcm56846_l3_route_add(int unit, const bcm56846_l3_route_t *route);
int bcm56846_l3_route_delete(int unit, const bcm56846_l3_route_t *route);
int bcm56846_l3_host_add(int unit, const bcm56846_l3_host_t *host);

/* ECMP */
int bcm56846_l3_ecmp_create(int unit, const int *egress_ids, int count, int *ecmp_id);
int bcm56846_l3_ecmp_destroy(int unit, int ecmp_id);

/* VLAN */
int bcm56846_vlan_create(int unit, uint16_t vid);
int bcm56846_vlan_port_add(int unit, uint16_t vid, int port, int tagged);
int bcm56846_vlan_destroy(int unit, uint16_t vid);

/* Stats (XLMAC counters) */
int bcm56846_stat_get(int unit, int port, bcm56846_stat_t stat, uint64_t *value);

/* Packet I/O */
int bcm56846_tx(int unit, int port, const void *pkt, int len);
int bcm56846_rx_register(int unit, bcm56846_rx_cb_t cb, void *cookie);
int bcm56846_rx_start(int unit);
int bcm56846_rx_stop(int unit);
```

## SCHAN Transport Architecture

All SDK modules use `sbus.h` for ASIC register and table access via S-Channel:

- **`sbus_reg_read/write(addr, val)`** — 32-bit register access
- **`sbus_reg_read64/write64(addr, data)`** — 64-bit register access (XMAC)
- **`sbus_mem_read/write(base, index, words, nwords)`** — indexed table access (L2, L3, VLAN, ECMP, etc.)
- **`cdk_port_addr(base, port)`** — compute per-port SBUS address from CDK base + port index

The `sbus.c` layer constructs proper SCHAN headers (opcode, dstblk, datalen) and dispatches via the BDE kernel module's SCHAN_OP ioctl. This replaces the older `schan.c` which put raw addresses as SCHAN headers (broken).

## Directory Structure

```
sdk/
├── include/
│   ├── bcm56846.h          # Public API header
│   ├── bcm56846_regs.h     # CMIC/MIIM/SBUS register offsets (BAR0)
│   ├── bcm56846_types.h    # Common types (L2, L3, stat enums)
│   ├── bde_ioctl.h         # BDE ioctl interface definitions
│   └── sbus.h              # SCHAN transport API (all modules use this)
└── src/
    ├── attach.c        # Device attach (PCI, BAR0)
    ├── bde_ioctl.c     # BDE ioctl wrappers (read/write reg, schan_op)
    ├── config.c        # config.bcm parser
    ├── init.c          # ASIC init (SBUS ring map, XLPORT reset, LINK40G)
    ├── init_datapath.c # Full datapath init (8 phases: buffers→priority→hash→COS→THDO→sched→XMAC→VLAN)
    ├── soc.c           # SOC script runner (rc.soc replay)
    ├── sbus.c          # SCHAN transport (proper header construction, CDK addressing)
    ├── schan.c         # Legacy S-Channel (deprecated — broken header format)
    ├── reg.c           # Register read/write helpers
    ├── port.c          # Port enable, XLPORT, link status (uses sbus.h)
    ├── serdes.c        # WARPcore WC-B0 SerDes (MIIM, AER, CL45, 10G SFI init)
    ├── l2.c            # L2_ENTRY + L2_USER_ENTRY table programming (uses sbus.h)
    ├── l3.c            # L3 intf, egress, route, host (uses sbus.h)
    ├── ecmp.c          # L3_ECMP + L3_ECMP_GROUP (uses sbus.h)
    ├── vlan.c          # VLAN table programming (uses sbus.h)
    ├── pktio.c         # DMA ring TX/RX (DCB21, CMICe at 0x100)
    └── stats.c         # XLMAC counter reads (uses sbus.h)
```

## RE References (build order)

1. S-Channel: `../../docs/reverse-engineering/SCHAN_FORMAT_ANALYSIS.md`, `WRITE_MECHANISM_ANALYSIS.md`
2. Init:      `../../docs/reverse-engineering/initialization-sequence.md`, `SDK_AND_ASIC_CONFIG_FROM_SWITCH.md`
3. Port:      `../../docs/reverse-engineering/PORT_BRINGUP_REGISTER_MAP.md`, `SERDES_WC_INIT.md`
4. L2:        `../../docs/reverse-engineering/L2_ENTRY_FORMAT.md`, `L2_WRITE_PATH_COMPLETE.md`
5. L3:        `../../docs/reverse-engineering/L3_NEXTHOP_FORMAT.md`, `L3_ECMP_VLAN_WRITE_PATH.md`
6. Pkt I/O:   `../../docs/reverse-engineering/PACKET_BUFFER_ANALYSIS.md`, `DMA_DCB_LAYOUT_FROM_KNET.md`
