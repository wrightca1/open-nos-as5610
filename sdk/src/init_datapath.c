/*
 * BCM56846 (Trident+) datapath initialization.
 *
 * Translates etc/nos/rc.datapath_0 (Cumulus-generated) into native C code
 * using CDK register addresses from OpenMDK bcm56840_a0_defs.h and the
 * SBUS SCHAN messaging layer (sbus.c).
 *
 * This configures: ingress buffers, priority mapping, flow control,
 * ECMP hash (RTAG7), CPU control, egress COS/buffers, THDO thresholds,
 * and scheduling (S2/S3/ES).  Without this, the ASIC drops all packets.
 *
 * Port numbering (BCM56846 Trident+):
 *   Port 0       = CPU
 *   Ports 1-48   = xe0-xe47 (10G SFP+)
 *   Ports 49-52  = xe48-xe51 (40G QSFP)
 */
#include "sbus.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ===== Port range constants ===== */
#define PORT_CPU        0
#define PORT_MIN        0
#define PORT_MAX        52   /* last front-panel port */
#define PORT_XE_MIN     1    /* first 10G port */
#define PORT_XE_MAX     48   /* last 10G port */
#define PORT_40G_MIN    49   /* first 40G port */
#define PORT_40G_MAX    52   /* last 40G port */
#define PORT_TAB_MAX    66   /* PORT_TABm entries 0-66 */

/* ===== CDK Register Addresses (from bcm56840_a0_defs.h) ===== */

/* Ingress buffer management */
#define BUFFER_CELL_LIMIT_SPr           0x0238010au
#define BUFFER_CELL_LIMIT_SP_SHAREDr    0x0238010eu
#define CELL_RESET_LIMIT_OFFSET_SPr     0x02380114u
#define PG_MIN_CELLr                    0x02300050u
#define PG_HDRM_LIMIT_CELLr            0x02300060u
#define PG_SHARED_LIMIT_CELLr           0x02300023u
#define GLOBAL_HDRM_LIMITr              0x02380002u
#define USE_SP_SHAREDr                  0x02380132u
#define COLOR_AWAREr                    0x02380131u
#define PORT_PG_SPIDr                   0x02300073u
#define PORT_PRI_GRP0r                  0x02300070u
#define PORT_PRI_GRP1r                  0x02300071u
#define PORT_PRI_XON_ENABLEr            0x02300072u
#define PORT_MAX_PKT_SIZEr              0x02300022u
#define PORT_MAX_SHARED_CELLr           0x02300021u
#define PORT_MIN_CELLr                  0x02300020u
#define PORT_MIN_PG_ENABLEr             0x02300137u
#define PORT_SHARED_MAX_PG_ENABLEr      0x02300136u

/* Priority mapping */
#define EGR_VLAN_CONTROL_1r             0x01200606u
#define ING_UNTAGGED_PHBm               0x0c172000u
#define ING_PRI_CNG_MAPm                0x0c170000u

/* Flow control */
#define PRIO2COS_LLFC0r                 0x15380000u
#define XMAC_PFC_CTRLr                  0x0050060eu
#define XLPORT_CONFIGr                  0x00500200u

/* ECMP/Hash */
#define HASH_CONTROLr                   0x05180640u
#define RTAG7_HASH_SEED_Ar              0x05180615u
#define RTAG7_HASH_ECMPr                0x0b180600u
#define RTAG7_HASH_CONTROL_3r           0x0518061au
#define RTAG7_HASH_FIELD_BMAP_1r        0x0518060cu
#define RTAG7_HASH_FIELD_BMAP_2r        0x0518060du
#define RTAG7_IPV4_TCP_UDP_HASH_FIELD_BMAP_1r  0x0518061bu
#define RTAG7_IPV4_TCP_UDP_HASH_FIELD_BMAP_2r  0x0518061cu
#define RTAG7_IPV6_TCP_UDP_HASH_FIELD_BMAP_1r  0x0518061du
#define RTAG7_IPV6_TCP_UDP_HASH_FIELD_BMAP_2r  0x0518061eu

/* CPU control */
#define CPU_CONTROL_1r                  0x0c180603u
#define AUX_ARB_CONTROLr                0x00180700u

/* Port table */
#define PORT_TABm                       0x01160000u

/* Egress COS */
#define ING_COS_MODEr                   0x0f100677u
#define COS_MODE_Xr                     0x1f380032u
#define COS_MODE_Yr                     0x1f380034u
#define COS_MAP_SELm                    0x0f17b000u
#define PORT_COS_MAPm                   0x0f173800u
#define CPU_COS_MAPm                    0x0f174000u
#define ES_QUEUE_TO_PRIOr               0x06380080u
#define COSMASKr                        0x06300020u
#define ES_TDM_CONFIGr                  0x06380022u

/* Egress buffer */
#define OP_QUEUE_CONFIG_CELLr           0x03300100u
#define OP_QUEUE_CONFIG1_CELLr          0x03300140u
#define OP_QUEUE_RESET_OFFSET_CELLr     0x03300200u
#define OP_PORT_CONFIG_CELLr            0x03300020u
#define OP_PORT_CONFIG1_CELLr           0x03300028u
#define OP_UC_PORT_CONFIG_CELLr         0x03300024u
#define OP_UC_PORT_CONFIG1_CELLr        0x03300029u
#define OP_BUFFER_SHARED_LIMIT_CELLr    0x03380004u
#define OP_BUFFER_SHARED_LIMIT_RESUME_CELLr 0x03380080u
#define OP_VOQ_PORT_CONFIGr             0x03380014u
#define OVQ_FLOWCONTROL_THRESHOLDr      0x1f380008u

/* THDO tables (memory) */
#define THDO_CONFIG_0Am                 0x03300800u
#define THDO_CONFIG_0Bm                 0x03301000u
#define MMU_THDO_CONFIG_SP_0m           0x0330c000u
#define MMU_THDO_CONFIG_SP_1m           0x0330c800u
#define MMU_THDO_QDRPRST_0m            0x03319000u
#define MMU_THDO_QDRPRST_1m            0x03319800u
#define MMU_THDO_QDRPRST_SP_0m         0x0331b000u
#define MMU_THDO_QDRPRST_SP_1m         0x0331b800u

/* Port pipeline enable */
#define EPC_LINK_BMAPm                  0x0f178800u
#define ING_EN_EFILTER_BITMAPm         0x0f178000u
#define EGR_PORTm                       0x0126e000u
#define XMAC_TX_CTRLr                   0x00500604u
#define XMAC_RX_CTRLr                   0x00500606u
#define IFP_METER_PARITY_CONTROLr       0x0a400000u

/* Scheduling */
#define S3_CONFIGr                      0x19300000u
#define S3_CONFIG_MCr                   0x19300001u
#define S3_COSWEIGHTSr                  0x19300010u
#define S3_MINSPCONFIGr                 0x193000c0u
#define S2_CONFIGr                      0x1a300000u
#define S2_COSWEIGHTSr                  0x1a300010u
#define S2_MINSPCONFIGr                 0x1a3000c0u
#define S2_S3_ROUTINGr                  0x1a3000e0u
#define ESCONFIGr                       0x06300000u
#define COSWEIGHTSr                     0x06300100u
#define MINSPCONFIGr                    0x06300050u

/* ===== Field bit positions ===== */

/* PORT_PRI_GRP0r: PRIx_GRP fields (3 bits each) */
#define PRI0_GRP_SHIFT   0
#define PRI2_GRP_SHIFT   6
#define PRI7_GRP_SHIFT   21

/* PORT_PG_SPIDr: PGx_SPID fields (2 bits each) */
#define PG2_SPID_SHIFT   4
#define PG7_SPID_SHIFT   14

/* PG_SHARED_LIMIT_CELLr */
#define PG_SHARED_LIMIT_SHIFT  0
#define PG_SHARED_LIMIT_MASK   0xFFFFu
#define PG_SHARED_DYNAMIC_BIT  16

/* OP_QUEUE_CONFIG_CELLr */
#define Q_SHARED_LIMIT_CELL_SHIFT 0
#define Q_SHARED_LIMIT_CELL_MASK  0xFFFFu
#define Q_MIN_CELL_SHIFT          16
#define Q_MIN_CELL_MASK           0xFFFF0000u

/* OP_QUEUE_CONFIG1_CELLr */
#define Q_COLOR_ENABLE_CELL_BIT   0
#define Q_LIMIT_DYNAMIC_CELL_BIT  2
#define Q_LIMIT_ENABLE_CELL_BIT   3
#define Q_SPID_SHIFT              5
#define Q_SPID_MASK               (0x3u << 5)

/* OP_UC_PORT_CONFIG1_CELLr */
#define UC_COS2_SPID_SHIFT  5
#define UC_COS7_SPID_SHIFT  15

/* EGR_VLAN_CONTROL_1r */
#define REMARK_OUTER_DOT1P_BIT 11

/* HASH_CONTROLr */
#define ECMP_HASH_USE_RTAG7_BIT      23
#define USE_TCP_UDP_PORTS_BIT        22
#define L3_HASH_SELECT_SHIFT         18
#define L3_HASH_SELECT_MASK          (0x7u << 18)
#define NON_UC_TRUNK_HASH_USE_RTAG7_BIT 24

/* CPU_CONTROL_1r */
#define L3_MTU_FAIL_TOCPU_BIT   22
#define L3_SLOWPATH_TOCPU_BIT   20
#define V4L3DSTMISS_TOCPU_BIT   10
#define V6L3DSTMISS_TOCPU_BIT   9

/* AUX_ARB_CONTROLr */
#define L2_MOD_FIFO_ENABLE_L2_DELETE_BIT 6

/* ING_PRI_CNG_MAPm: PRI (mask 0xf, shift 2), CNG (mask 0x3, shift 0) */
#define PRI_CNG_PRI_SHIFT  2
#define PRI_CNG_PRI_MASK   0xFu

/* PORT_COS_MAPm: UC_COS1 (shift 0, mask 0xf), HG_COS (shift 4, mask 0x7f), MC_COS1 (shift 11, mask 0x7) */
#define COS_MAP_UC_SHIFT   0
#define COS_MAP_HG_SHIFT   4
#define COS_MAP_MC_SHIFT   11

/* CPU_COS_MAPm: VALID(bit0), INT_PRI_KEY(shift1,mask0xf), INT_PRI_MASK(shift5,mask0xf), COS(word1 or shift>32?) */
/* The explore agent said: VALID bit0, INT_PRI_KEY shift1 mask0xf, INT_PRI_MASK shift4 mask0xf, COS shift7 */
#define CPU_COS_VALID_BIT     0
#define CPU_COS_PRI_KEY_SHIFT 1
#define CPU_COS_PRI_MASK_SHIFT 5
#define CPU_COS_COS_SHIFT     9

/* S3_CONFIGr */
#define S3_SCHEDULING_SELECT_MASK 0xFFu
#define S3_ROUTE_UC_TO_S2_BIT     8

/* S2_CONFIGr */
#define S2_SCHEDULING_SELECT_MASK 0x3Fu

/* ESCONFIGr */
#define ES_SCHEDULING_SELECT_MASK 0x3u

/* S2_S3_ROUTINGr: 64-bit, S3_GROUP_NO_Ix (5 bits each) */
#define S3_GRP_BITS   5
#define S3_GRP_MASK   0x1Fu

/* XLPORT_CONFIGr */
#define XLPORT_LLFC_EN_BIT      14
#define XLPORT_XPAUSE_RX_EN_BIT 16
#define XLPORT_XPAUSE_TX_EN_BIT 17
#define XLPORT_PFC_ENABLE_BIT   18

/* COSMASKr */
#define COSMASKRXEN_BIT  7

/* ES_TDM_CONFIGr */
#define EN_CPU_SLOT_SHARING_BIT  24

/* OVQ_FLOWCONTROL_THRESHOLDr */
#define OVQ_FC_ENABLE_BIT  28

/*
 * Compute XLPORT SBUS address for a front-panel port register.
 * xe: 0-based xe port index (xe0=0, xe51=51)
 * reg_offset: register offset within XLPORT (e.g. 0x604 for XMAC_TX_CTRL)
 * Returns full CDK-style SBUS address with correct block and lane encoding.
 */
static uint32_t xlport_reg_addr(int xe, int reg_offset)
{
	static const uint32_t base[16] = {
		0x40a00000u, 0x40b00000u, 0x00b00000u, 0x00c00000u,
		0x00d00000u, 0x00e00000u, 0x00f00000u, 0x40000000u,
		0x40100000u, 0x40200000u, 0x40300000u, 0x40400000u,
		0x40600000u, 0x40500000u, 0x40900000u, 0x40800000u,
	};
	static const int permute[4] = { 1, 0, 3, 2 };
	int blk, lane;

	if (xe < 0 || xe > 51)
		return 0;
	if (xe < 48) {
		blk = xe / 4;
		lane = xe % 4;
		if (blk == 5 || blk == 6) /* xe20-27: PCB lane swap */
			lane = permute[lane];
	} else {
		blk = 12 + (xe - 48);
		lane = 0;
	}
	return base[blk] + (uint32_t)lane * 0x1000u + (uint32_t)reg_offset;
}

/* XMAC register offsets within XLPORT block */
#define XMAC_TX_CTRL_OFF  0x604
#define XMAC_RX_CTRL_OFF  0x606

/* ===== Helper: write per-port register to all ports ===== */
static int reg_write_allports(uint32_t base, uint32_t value)
{
	int p, rc = 0;
	for (p = PORT_MIN; p <= PORT_MAX; p++) {
		if (sbus_reg_write(cdk_port_addr(base, p), value) < 0)
			rc = -1;
	}
	return rc;
}

/* Helper: modify per-port register for all ports */
static int reg_modify_allports(uint32_t base, uint32_t mask, uint32_t value)
{
	int p, rc = 0;
	for (p = PORT_MIN; p <= PORT_MAX; p++) {
		if (sbus_reg_modify(cdk_port_addr(base, p), mask, value) < 0)
			rc = -1;
	}
	return rc;
}

/* Helper: write 64-bit per-port register to all ports */
static int reg_write64_allports(uint32_t base, const uint32_t *data)
{
	int p, rc = 0;
	for (p = PORT_MIN; p <= PORT_MAX; p++) {
		if (sbus_reg_write64(cdk_port_addr(base, p), data) < 0)
			rc = -1;
	}
	return rc;
}

/*
 * ===================================================================
 * Phase 1: Ingress Buffer Management (rc.datapath_0 lines 87-119)
 * ===================================================================
 */
static int init_ingress_buffers(void)
{
	int p;

	fprintf(stderr, "[datapath] phase 1: ingress buffers\n");

	/* color_aware = 0 */
	sbus_reg_write(COLOR_AWAREr, 0);

	/* port_pg_spid: pg2_spid=2, pg7_spid=1, rest=0 */
	reg_write_allports(PORT_PG_SPIDr, (2u << PG2_SPID_SHIFT) | (1u << PG7_SPID_SHIFT));

	/* buffer_cell_limit_sp[0..3]: service pool limits */
	sbus_reg_write(BUFFER_CELL_LIMIT_SPr + 0, 0);
	sbus_reg_write(BUFFER_CELL_LIMIT_SPr + 1, 1382);
	sbus_reg_write(BUFFER_CELL_LIMIT_SPr + 2, 921);
	sbus_reg_write(BUFFER_CELL_LIMIT_SPr + 3, 0);

	/* cell_reset_limit_offset_sp[0..3] */
	sbus_reg_write(CELL_RESET_LIMIT_OFFSET_SPr + 0, 0);
	sbus_reg_write(CELL_RESET_LIMIT_OFFSET_SPr + 1, 100);
	sbus_reg_write(CELL_RESET_LIMIT_OFFSET_SPr + 2, 100);
	sbus_reg_write(CELL_RESET_LIMIT_OFFSET_SPr + 3, 0);

	/* buffer_cell_limit_sp_shared = 22742 */
	sbus_reg_write(BUFFER_CELL_LIMIT_SP_SHAREDr, 22742);

	/* Per-port: port_shared_max_pg_enable, port_max_shared_cell, port_min_pg_enable = 0 */
	reg_write_allports(PORT_SHARED_MAX_PG_ENABLEr, 0);
	reg_write_allports(PORT_MAX_SHARED_CELLr, 0);
	reg_write_allports(PORT_MIN_PG_ENABLEr, 0);

	/* port_min_cell = 0 (all ports) */
	reg_write_allports(PORT_MIN_CELLr, 0);

	/* pg_min_cell = 0, pg_hdrm_limit_cell = 0 (defaults for all PGs, all ports) */
	for (p = PORT_MIN; p <= PORT_MAX; p++) {
		int pg;
		for (pg = 0; pg < 8; pg++) {
			sbus_reg_write(cdk_port_addr(PG_MIN_CELLr + pg, p), 0);
			sbus_reg_write(cdk_port_addr(PG_HDRM_LIMIT_CELLr + pg, p), 0);
		}
	}

	/*
	 * PG0 min cell limits:
	 *   cpu0 = 45, xe0-xe47 = 288, xe48-xe51 = 1152
	 */
	sbus_reg_write(cdk_port_addr(PG_MIN_CELLr, PORT_CPU), 45);
	for (p = PORT_XE_MIN; p <= PORT_XE_MAX; p++)
		sbus_reg_write(cdk_port_addr(PG_MIN_CELLr, p), 288);
	for (p = PORT_40G_MIN; p <= PORT_40G_MAX; p++)
		sbus_reg_write(cdk_port_addr(PG_MIN_CELLr, p), 1152);

	/* PG0 shared limit = 4548 (all ports, no dynamic) */
	reg_write_allports(PG_SHARED_LIMIT_CELLr, 4548);

	/*
	 * PG2 min cell limits:
	 *   cpu0 = 45, xe48-xe51 = 4, xe0-xe47 = 1
	 */
	sbus_reg_write(cdk_port_addr(PG_MIN_CELLr + 2, PORT_CPU), 45);
	for (p = PORT_XE_MIN; p <= PORT_XE_MAX; p++)
		sbus_reg_write(cdk_port_addr(PG_MIN_CELLr + 2, p), 1);
	for (p = PORT_40G_MIN; p <= PORT_40G_MAX; p++)
		sbus_reg_write(cdk_port_addr(PG_MIN_CELLr + 2, p), 4);

	/* PG2 shared limit = 909 */
	reg_write_allports(PG_SHARED_LIMIT_CELLr + 2, 909);

	/*
	 * PG7 min cell limits:
	 *   cpu0 = 45, xe48-xe51 = 4, xe0-xe47 = 1
	 */
	sbus_reg_write(cdk_port_addr(PG_MIN_CELLr + 7, PORT_CPU), 45);
	for (p = PORT_XE_MIN; p <= PORT_XE_MAX; p++)
		sbus_reg_write(cdk_port_addr(PG_MIN_CELLr + 7, p), 1);
	for (p = PORT_40G_MIN; p <= PORT_40G_MAX; p++)
		sbus_reg_write(cdk_port_addr(PG_MIN_CELLr + 7, p), 4);

	/* PG7 shared limit = 10006 */
	reg_write_allports(PG_SHARED_LIMIT_CELLr + 7, 10006);

	/* use_sp_shared = 0x7 (SPs 0-2 shared) */
	sbus_reg_write(USE_SP_SHAREDr, 0x7);

	/* global_hdrm_limit = 2340 */
	sbus_reg_write(GLOBAL_HDRM_LIMITr, 2340);

	/* port_max_pkt_size = 45 (all ports) */
	reg_write_allports(PORT_MAX_PKT_SIZEr, 45);

	fprintf(stderr, "[datapath] phase 1: done\n");
	return 0;
}

/*
 * ===================================================================
 * Phase 2: Priority Mapping + Flow Control (rc.datapath_0 lines 67-85)
 * ===================================================================
 */
static int init_priority_mapping(void)
{
	int i, p;
	uint32_t data[2];

	fprintf(stderr, "[datapath] phase 2: priority mapping + flow control\n");

	/* modreg egr_vlan_control_1 remark_outer_dot1p=0 */
	sbus_reg_modify(EGR_VLAN_CONTROL_1r,
			1u << REMARK_OUTER_DOT1P_BIT, 0);

	/* write ing_untagged_phb 0 64: all entries = 0 (pri=0, cng=0) */
	memset(data, 0, sizeof(data));
	for (i = 0; i < 64; i++)
		sbus_mem_write(ING_UNTAGGED_PHBm, i, data, 1);

	/* write ing_pri_cng_map 0 1024: all entries = 0 */
	for (i = 0; i < 1024; i++)
		sbus_mem_write(ING_PRI_CNG_MAPm, i, data, 1);

	/*
	 * modify ing_pri_cng_map: set PRI field for indices 0-15
	 * Each modify covers 2 consecutive entries with same PRI value.
	 * PRI field: mask 0xf, shift 2; CNG field: mask 0x3, shift 0
	 */
	{
		static const struct { int idx; int count; uint32_t pri; } pri_map[] = {
			{  0, 2, 0 },
			{  2, 2, 1 },
			{  4, 2, 2 },
			{  6, 2, 4 },
			{  8, 2, 4 },
			{ 10, 2, 5 },
			{ 12, 2, 6 },
			{ 14, 2, 7 },
		};
		for (i = 0; i < 8; i++) {
			int j;
			uint32_t val = (pri_map[i].pri << PRI_CNG_PRI_SHIFT);
			for (j = 0; j < pri_map[i].count; j++) {
				data[0] = val;
				sbus_mem_write(ING_PRI_CNG_MAPm,
					       pri_map[i].idx + j, data, 1);
			}
		}
	}

	/*
	 * port_pri_grp0: pri7_grp=7, pri2_grp=2, rest=0
	 * Value = (7 << 21) | (2 << 6) = 0x00E00080
	 */
	reg_write_allports(PORT_PRI_GRP0r, (7u << PRI7_GRP_SHIFT) | (2u << PRI2_GRP_SHIFT));

	/* port_pri_grp1: all 0 */
	reg_write_allports(PORT_PRI_GRP1r, 0);

	/* ----- Flow control ----- */

	/* port_pri_xon_enable.$allports = 0 */
	reg_write_allports(PORT_PRI_XON_ENABLEr, 0);

	/* prio2cos_llfc0 = 0 (mc_cos0_5_bmp=0, uc_cos0_10_bmp=0) */
	sbus_reg_write(PRIO2COS_LLFC0r, 0);

	/*
	 * modreg xmac_pfc_ctrl.$allports: tx_pfc_en=0, rx_pfc_en=0, pfc_stats_en=0
	 * XMAC_PFC_CTRL is per-XLPORT. CDK address 0x0050060e, block 5.
	 * All PFC fields cleared to 0 (disabled) — this is the reset default,
	 * but rc.datapath_0 writes it explicitly. Best-effort via CDK path.
	 */
	reg_modify_allports(XMAC_PFC_CTRLr, 0x7u, 0);

	/*
	 * modreg xlport_config: xpause_rx_en=1, xpause_tx_en=0, pfc_enable=0, llfc_en=0
	 * rc.datapath_0 line 85 (no $allports — single register write).
	 */
	{
		uint32_t mask = (1u << XLPORT_LLFC_EN_BIT) |
				(1u << XLPORT_XPAUSE_RX_EN_BIT) |
				(1u << XLPORT_XPAUSE_TX_EN_BIT) |
				(1u << XLPORT_PFC_ENABLE_BIT);
		uint32_t val  = (1u << XLPORT_XPAUSE_RX_EN_BIT);
		sbus_reg_modify(XLPORT_CONFIGr, mask, val);
	}

	fprintf(stderr, "[datapath] phase 2: done\n");
	return 0;
}

/*
 * ===================================================================
 * Phase 3: ECMP Hash + CPU Control (rc.datapath_0 lines 125-210)
 * ===================================================================
 */
static int init_forwarding(void)
{
	int i;

	fprintf(stderr, "[datapath] phase 3: ECMP hash + CPU control\n");

	/*
	 * RTAG7 hash field bitmaps.
	 * 0b0111101111100 = 0xF7C (bins 2,3,4,5,6,8,9,10,11)
	 * 0b0111100111100 = 0xF3C (bins 2,3,4,5,8,9,10,11 — no L4 dst)
	 * 0b0111100011100 = 0xF1C (bins 2,3,4,8,9,10,11 — no L4 ports)
	 */
	sbus_reg_modify(RTAG7_IPV4_TCP_UDP_HASH_FIELD_BMAP_2r, 0x1FFFu, 0x0F7Cu);
	sbus_reg_modify(RTAG7_IPV6_TCP_UDP_HASH_FIELD_BMAP_2r, 0x1FFFu, 0x0F7Cu);
	sbus_reg_modify(RTAG7_IPV4_TCP_UDP_HASH_FIELD_BMAP_1r, 0x1FFFu, 0x0F3Cu);
	sbus_reg_modify(RTAG7_IPV6_TCP_UDP_HASH_FIELD_BMAP_1r, 0x1FFFu, 0x0F3Cu);
	sbus_reg_modify(RTAG7_HASH_FIELD_BMAP_1r, 0x1FFFu, 0x0F1Cu);
	sbus_reg_modify(RTAG7_HASH_FIELD_BMAP_2r, 0x1FFFu, 0x0F1Cu);

	/* RTAG7_HASH_CONTROL_3: hash_a0_function_select = 9 (CRC16 CCITT) */
	sbus_reg_modify(RTAG7_HASH_CONTROL_3r, 0xFu, 9);

	/* rtag7_hash_seed_a = 42 */
	sbus_reg_write(RTAG7_HASH_SEED_Ar, 42);

	/* rtag7_hash_ecmp(0) = 0, rtag7_hash_ecmp(1) = 0 */
	sbus_reg_write(RTAG7_HASH_ECMPr + 0, 0);
	sbus_reg_write(RTAG7_HASH_ECMPr + 1, 0);

	/*
	 * hash_control: combined RMW for all fields
	 *   ecmp_hash_use_rtag7 = 1 (bit 23)
	 *   use_tcp_udp_ports = 1 (bit 22)
	 *   l3_hash_select = 4 (bits 20:18)
	 *   non_uc_trunk_hash_use_rtag7 = 1 (bit 24)
	 */
	{
		uint32_t mask = (1u << ECMP_HASH_USE_RTAG7_BIT) |
				(1u << USE_TCP_UDP_PORTS_BIT) |
				L3_HASH_SELECT_MASK |
				(1u << NON_UC_TRUNK_HASH_USE_RTAG7_BIT);
		uint32_t val  = (1u << ECMP_HASH_USE_RTAG7_BIT) |
				(1u << USE_TCP_UDP_PORTS_BIT) |
				(4u << L3_HASH_SELECT_SHIFT) |
				(1u << NON_UC_TRUNK_HASH_USE_RTAG7_BIT);
		sbus_reg_modify(HASH_CONTROLr, mask, val);
	}

	/*
	 * cpu_control_1: combined RMW
	 *   l3_mtu_fail_tocpu = 1 (bit 22)
	 *   l3_slowpath_tocpu = 1 (bit 20)
	 *   v4l3dstmiss_tocpu = 1 (bit 10)
	 *   v6l3dstmiss_tocpu = 1 (bit 9)
	 */
	{
		uint32_t mask = (1u << L3_MTU_FAIL_TOCPU_BIT) |
				(1u << L3_SLOWPATH_TOCPU_BIT) |
				(1u << V4L3DSTMISS_TOCPU_BIT) |
				(1u << V6L3DSTMISS_TOCPU_BIT);
		sbus_reg_modify(CPU_CONTROL_1r, mask, mask);
	}

	/* aux_arb_control: l2_mod_fifo_enable_l2_delete = 0 (bit 6) */
	sbus_reg_modify(AUX_ARB_CONTROLr,
			1u << L2_MOD_FIFO_ENABLE_L2_DELETE_BIT, 0);

	/*
	 * modify port 0 67: PORT_TABm entries 0-66
	 *   port_pri=0, pri_mapping=0, trust_incoming_vid=0, vt_enable=0
	 *
	 * PORT_TABm: 10 words (39 bytes) per entry.
	 * Field layout (from CDK bcm56840_a0_defs.h):
	 *   PORT_PRI:          word[0] bits[12:10] (3 bits)
	 *   VT_ENABLE:         word[0] bit[2]
	 *   TRUST_INCOMING_VID: word[3] bit[8]
	 *   PRI_MAPPING:       bits[160:137] = word[4] bits[31:9] + word[5] bit[0]
	 *
	 * All set to 0. Read 6 words, clear the bits, write back 6 words.
	 * (Partial write is safe; words 6-9 are untouched.)
	 */
	for (i = 0; i < 67; i++) {
		uint32_t entry[6] = {0};

		if (sbus_mem_read(PORT_TABm, i, entry, 6) < 0) {
			/* If read fails, write zeros for the fields we care about */
			memset(entry, 0, sizeof(entry));
		}

		entry[0] &= ~((0x7u << 10) | (1u << 2));  /* PORT_PRI, VT_ENABLE */
		entry[3] &= ~(1u << 8);                    /* TRUST_INCOMING_VID */
		entry[4] &= ~0xFFFFFE00u;                  /* PRI_MAPPING low 23 bits */
		entry[5] &= ~0x1u;                         /* PRI_MAPPING high bit */

		/* PORT_VID = 1 (bits [37:26]): default VLAN for untagged ingress.
		 * word[0] bits[31:26] = lower 6 bits, word[1] bits[5:0] = upper 6 bits.
		 * VID=1: set bit 26 in word[0], clear bits[31:27] and word[1][5:0]. */
		entry[0] = (entry[0] & ~0xFC000000u) | (1u << 26);
		entry[1] &= ~0x3Fu;

		sbus_mem_write(PORT_TABm, i, entry, 6);
	}

	fprintf(stderr, "[datapath] phase 3: done\n");
	return 0;
}

/*
 * ===================================================================
 * Phase 4: Egress COS + Egress Buffer Management (rc.datapath_0 lines 213-282)
 * ===================================================================
 */
static int init_egress_config(void)
{
	int i, p;
	uint32_t data[2];

	fprintf(stderr, "[datapath] phase 4: egress COS + egress buffers\n");

	/* ing_cos_mode = 0, cos_mode_x = 0, cos_mode_y = 0 */
	sbus_reg_write(ING_COS_MODEr, 0);
	sbus_reg_write(COS_MODE_Xr, 0);
	sbus_reg_write(COS_MODE_Yr, 0);

	/* write cos_map_sel 0 67: all entries = 0 */
	memset(data, 0, sizeof(data));
	for (i = 0; i < 67; i++)
		sbus_mem_write(COS_MAP_SELm, i, data, 1);

	/*
	 * write cos_map 0 64: all entries uc_cos1=0, hg_cos=0, mc_cos1=0
	 * Then override specific entries for COS 0-7.
	 */
	for (i = 0; i < 64; i++)
		sbus_mem_write(PORT_COS_MAPm, i, data, 1);

	/* COS map entries: index = priority, value = packed fields */
	{
		static const struct {
			int idx; uint32_t uc; uint32_t hg; uint32_t mc;
		} cos_entries[] = {
			{ 0, 0, 0, 0 },
			{ 1, 1, 1, 0 },
			{ 2, 2, 2, 2 },
			{ 4, 4, 4, 0 },
			{ 5, 5, 5, 0 },
			{ 6, 6, 6, 0 },
			{ 7, 7, 7, 1 },
		};
		for (i = 0; i < 7; i++) {
			data[0] = (cos_entries[i].uc << COS_MAP_UC_SHIFT) |
				  (cos_entries[i].hg << COS_MAP_HG_SHIFT) |
				  (cos_entries[i].mc << COS_MAP_MC_SHIFT);
			sbus_mem_write(PORT_COS_MAPm, cos_entries[i].idx,
				       data, 1);
		}
	}

	/*
	 * CPU_COS_MAP: entries 120-127 (highest priority first)
	 * Fields: VALID(bit0), INT_PRI_KEY(shift1), INT_PRI_MASK(shift5), COS(shift9)
	 */
	{
		static const struct {
			int idx; uint32_t pri_key; uint32_t cos;
		} cpu_cos[] = {
			{ 127, 0, 0 },
			{ 126, 1, 1 },
			{ 125, 2, 2 },
			{ 124, 4, 4 },
			{ 123, 4, 4 },
			{ 122, 5, 5 },
			{ 121, 6, 6 },
			{ 120, 7, 7 },
		};
		for (i = 0; i < 8; i++) {
			data[0] = (1u << CPU_COS_VALID_BIT) |
				  (cpu_cos[i].pri_key << CPU_COS_PRI_KEY_SHIFT) |
				  (0xFu << CPU_COS_PRI_MASK_SHIFT) |
				  (cpu_cos[i].cos << CPU_COS_COS_SHIFT);
			sbus_mem_write(CPU_COS_MAPm, cpu_cos[i].idx, data, 1);
		}
	}

	/*
	 * es_queue_to_prio: prio_0=0, prio_1=1, ..., prio_6=6
	 * Each field: 3 bits, shift = prio * 3
	 */
	{
		uint32_t val = 0;
		for (i = 0; i < 7; i++)
			val |= ((uint32_t)i << (i * 3));
		reg_write_allports(ES_QUEUE_TO_PRIOr, val);
	}

	/* ----- Overflow queue ----- */
	sbus_reg_write(OP_VOQ_PORT_CONFIGr, 0);
	sbus_reg_modify(OVQ_FLOWCONTROL_THRESHOLDr,
			1u << OVQ_FC_ENABLE_BIT, 0);

	/* ----- Egress buffer management ----- */

	/* Defaults: all queue/port config to 0 */
	reg_write_allports(OP_QUEUE_CONFIG_CELLr, 0);
	reg_write_allports(OP_QUEUE_CONFIG1_CELLr, 0);
	reg_write_allports(OP_QUEUE_RESET_OFFSET_CELLr, 3);
	reg_write_allports(OP_PORT_CONFIG_CELLr, 0);
	reg_write_allports(OP_PORT_CONFIG1_CELLr, 0);

	/*
	 * Queue[0] for all ports: q_spid=0, q_limit_enable=1, q_min=921, q_shared=2073
	 */
	for (p = PORT_MIN; p <= PORT_MAX; p++) {
		uint32_t addr;

		/* op_queue_config1_cell[0]: q_spid=0, q_limit_enable_cell=1 */
		addr = cdk_port_addr(OP_QUEUE_CONFIG1_CELLr + 0, p);
		sbus_reg_modify(addr,
				Q_SPID_MASK | (1u << Q_LIMIT_ENABLE_CELL_BIT),
				(0u << Q_SPID_SHIFT) | (1u << Q_LIMIT_ENABLE_CELL_BIT));

		/* op_queue_config_cell[0]: q_shared=2073, q_min=921 */
		addr = cdk_port_addr(OP_QUEUE_CONFIG_CELLr + 0, p);
		sbus_reg_modify(addr,
				Q_SHARED_LIMIT_CELL_MASK | Q_MIN_CELL_MASK,
				(2073u << Q_SHARED_LIMIT_CELL_SHIFT) |
				(921u << Q_MIN_CELL_SHIFT));
	}

	/* Queue[1] SPID=1, Queue[2] SPID=2 (all ports) */
	for (p = PORT_MIN; p <= PORT_MAX; p++) {
		sbus_reg_modify(cdk_port_addr(OP_QUEUE_CONFIG1_CELLr + 1, p),
				Q_SPID_MASK, (1u << Q_SPID_SHIFT));
		sbus_reg_modify(cdk_port_addr(OP_QUEUE_CONFIG1_CELLr + 2, p),
				Q_SPID_MASK, (2u << Q_SPID_SHIFT));
	}

	/*
	 * CPU port queue overrides (port 0):
	 * Queues 0-7: specific SPID and limits
	 * Queues 32-34: special CPU queues
	 */
	{
		static const struct {
			int qi; uint32_t spid; int enable; int min; int shared;
		} cpu_queues[] = {
			{  0, 0, 1,  307, 2073 },
			{  1, 0, 1,  307, 2073 },
			{  2, 2, 0,    0,    0 },
			{  3, 0, 1,  307, 2073 },
			{  4, 0, 1,  307, 2073 },
			{  5, 0, 1,  307, 2073 },
			{  6, 0, 1,  307, 2073 },
			{  7, 1, 0,    0,    0 },
			{ 32, 0, 1,  100,    0 },
			{ 33, 0, 1,    1,    0 },
			{ 34, 0, 1,  500,    0 },
		};
		for (i = 0; i < 11; i++) {
			uint32_t addr1, addr0;
			int qi = cpu_queues[i].qi;

			/* config1: SPID + limit_enable */
			addr1 = cdk_port_addr(OP_QUEUE_CONFIG1_CELLr + qi, PORT_CPU);
			sbus_reg_modify(addr1,
					Q_SPID_MASK | (1u << Q_LIMIT_ENABLE_CELL_BIT),
					(cpu_queues[i].spid << Q_SPID_SHIFT) |
					((uint32_t)cpu_queues[i].enable << Q_LIMIT_ENABLE_CELL_BIT));

			/* config: min + shared (only if non-zero) */
			if (cpu_queues[i].min || cpu_queues[i].shared) {
				addr0 = cdk_port_addr(OP_QUEUE_CONFIG_CELLr + qi, PORT_CPU);
				sbus_reg_modify(addr0,
						Q_SHARED_LIMIT_CELL_MASK | Q_MIN_CELL_MASK,
						((uint32_t)cpu_queues[i].shared << Q_SHARED_LIMIT_CELL_SHIFT) |
						((uint32_t)cpu_queues[i].min << Q_MIN_CELL_SHIFT));
			}
		}
	}

	/* UC port config: all 0 defaults, then cos2_spid=2, cos7_spid=1 */
	reg_write_allports(OP_UC_PORT_CONFIG_CELLr, 0);
	reg_write_allports(OP_UC_PORT_CONFIG1_CELLr,
			   (2u << UC_COS2_SPID_SHIFT) | (1u << UC_COS7_SPID_SHIFT));

	fprintf(stderr, "[datapath] phase 4: done\n");
	return 0;
}

/*
 * ===================================================================
 * Phase 5: THDO Threshold Tables (rc.datapath_0 lines 273-339)
 * ===================================================================
 */

/* Helper: zero a memory table range */
static void thdo_zero_table(uint32_t addr, int count, int nwords)
{
	uint32_t data[6] = {0};
	int i;

	if (nwords > 6) nwords = 6;
	for (i = 0; i < count; i++)
		sbus_mem_write(addr, i, data, nwords);
}

/*
 * Helper: write THDO config entry (words 0-1 only).
 *   word[0] = (q_min << 16) | q_shared
 *   word[1] = q_limit_enable | (q_limit_dynamic << 1) | (q_color_enable << 3)
 */
static void thdo_write_config(uint32_t addr, int index,
			      uint32_t q_min, uint32_t q_shared,
			      int enable, int dynamic, int color)
{
	uint32_t data[2];
	data[0] = (q_min << 16) | (q_shared & 0xFFFFu);
	data[1] = (uint32_t)enable |
		  ((uint32_t)dynamic << 1) |
		  ((uint32_t)color << 3);
	sbus_mem_write(addr, index, data, 2);
}

/* Helper: write QDRPRST entry (1 word: qdrp_reset in bits 12:0) */
static void thdo_write_qdrprst(uint32_t addr, int index, uint32_t reset_val)
{
	uint32_t data[1];
	data[0] = reset_val & 0x1FFFu;
	sbus_mem_write(addr, index, data, 1);
}

static int init_thdo_tables(void)
{
	int cos, idx;

	fprintf(stderr, "[datapath] phase 5: THDO threshold tables\n");

	/* Zero all THDO tables */
	thdo_zero_table(THDO_CONFIG_0Am,         296, 2);
	thdo_zero_table(THDO_CONFIG_0Bm,         296, 2);
	thdo_zero_table(MMU_THDO_CONFIG_SP_0m,    40, 2);
	thdo_zero_table(MMU_THDO_CONFIG_SP_1m,    40, 2);
	thdo_zero_table(MMU_THDO_QDRPRST_0m,    296, 1);
	thdo_zero_table(MMU_THDO_QDRPRST_1m,    296, 1);
	thdo_zero_table(MMU_THDO_QDRPRST_SP_0m,  40, 1);
	thdo_zero_table(MMU_THDO_QDRPRST_SP_1m,  40, 1);

	/*
	 * Program UC queues: COS 0,1,3,4,5,6 (skip 2 and 7 = "unlimited")
	 * for I = COS, COS+10, COS+20, ..., up to max index
	 *   q_min=384, q_shared=3110, q_limit_enable=1
	 * Stride = 10 queues per port
	 */
	{
		static const int uc_cos[] = { 0, 1, 3, 4, 5, 6 };
		int c;

		for (c = 0; c < 6; c++) {
			cos = uc_cos[c];

			/* Main tables: 296 entries, stride 10 */
			for (idx = cos; idx < 280; idx += 10) {
				thdo_write_config(THDO_CONFIG_0Am, idx,
						  384, 3110, 1, 0, 0);
				thdo_write_config(THDO_CONFIG_0Bm, idx,
						  384, 3110, 1, 0, 0);
			}

			/* SP tables: 40 entries, stride 10 */
			for (idx = cos; idx < 40; idx += 10) {
				thdo_write_config(MMU_THDO_CONFIG_SP_0m, idx,
						  384, 3110, 1, 0, 0);
				thdo_write_config(MMU_THDO_CONFIG_SP_1m, idx,
						  384, 3110, 1, 0, 0);
			}

			/* QDRPRST: qdrp_reset = 3449 */
			for (idx = cos; idx < 280; idx += 10) {
				thdo_write_qdrprst(MMU_THDO_QDRPRST_0m, idx, 3449);
				thdo_write_qdrprst(MMU_THDO_QDRPRST_1m, idx, 3449);
			}
			for (idx = cos; idx < 40; idx += 10) {
				thdo_write_qdrprst(MMU_THDO_QDRPRST_SP_0m, idx, 3449);
				thdo_write_qdrprst(MMU_THDO_QDRPRST_SP_1m, idx, 3449);
			}
		}
	}

	/* Egress buffer shared limits per service pool */
	sbus_reg_write(OP_BUFFER_SHARED_LIMIT_CELLr + 0, 20736);
	sbus_reg_write(OP_BUFFER_SHARED_LIMIT_RESUME_CELLr + 0, 20636);
	sbus_reg_write(OP_BUFFER_SHARED_LIMIT_CELLr + 1, 41472);
	sbus_reg_write(OP_BUFFER_SHARED_LIMIT_RESUME_CELLr + 1, 41372);
	sbus_reg_write(OP_BUFFER_SHARED_LIMIT_CELLr + 2, 41472);
	sbus_reg_write(OP_BUFFER_SHARED_LIMIT_RESUME_CELLr + 2, 41372);
	sbus_reg_write(OP_BUFFER_SHARED_LIMIT_CELLr + 3, 135);
	sbus_reg_write(OP_BUFFER_SHARED_LIMIT_RESUME_CELLr + 3, 35);

	fprintf(stderr, "[datapath] phase 5: done\n");
	return 0;
}

/*
 * ===================================================================
 * Phase 6: Scheduling (rc.datapath_0 lines 341-371)
 * ===================================================================
 */
static int init_scheduling(void)
{
	int p;

	fprintf(stderr, "[datapath] phase 6: scheduling\n");

	/*
	 * S3: scheduling_select=0xff, route_uc_to_s2=1
	 * Value = (1 << 8) | 0xff = 0x1ff
	 */
	reg_write_allports(S3_CONFIGr,
			   (1u << S3_ROUTE_UC_TO_S2_BIT) | S3_SCHEDULING_SELECT_MASK);

	/* s3_minspconfig = 0 */
	sbus_reg_write(S3_MINSPCONFIGr, 0);

	/* s3_cosweights.$allports = 16 */
	reg_write_allports(S3_COSWEIGHTSr, 16);

	/* s3_config_mc.$allports: use_mc_group = 0 */
	reg_write_allports(S3_CONFIG_MCr, 0);

	/* S2: scheduling_select = 0x3f */
	reg_write_allports(S2_CONFIGr, S2_SCHEDULING_SELECT_MASK);

	/* s2_cosweights.$allports = 0 (default, overridden per COS below) */
	reg_write_allports(S2_COSWEIGHTSr, 0);

	/* s2_minspconfig = 0 */
	sbus_reg_write(S2_MINSPCONFIGr, 0);

	/*
	 * S2_S3_ROUTING: 64-bit register. Set all groups to 0x1f initially,
	 * then override specific groups for instances 0, 1, 2.
	 *
	 * All 0x1f: word[0]=0xFFFFFFFF, word[1]=0x1FFF
	 */
	{
		uint32_t all_1f[2] = { 0xFFFFFFFFu, 0x00001FFFu };
		int inst;

		/* Set all instances (0-2) to all groups = 0x1f */
		for (inst = 0; inst < 3; inst++)
			reg_write64_allports(S2_S3_ROUTINGr + inst, all_1f);
	}

	/*
	 * S2_S3_ROUTING instance 0:
	 *   I0=4, I1=5, I2=7, I3=8, I4=9, I5=10, I6=0, I7=0x1f, I8=0x1f
	 *
	 * word[0] = (4<<0)|(5<<5)|(7<<10)|(8<<15)|(9<<20)|(10<<25)|((0&3)<<30)
	 * word[1] = ((0>>2)&7) | (0x1f<<3) | (0x1f<<8)
	 */
	{
		uint32_t r0[2];
		r0[0] = (4u << 0) | (5u << 5) | (7u << 10) | (8u << 15) |
			(9u << 20) | (10u << 25) | ((0u & 3u) << 30);
		r0[1] = ((0u >> 2) & 7u) | (0x1Fu << 3) | (0x1Fu << 8);
		reg_write64_allports(S2_S3_ROUTINGr + 0, r0);
	}

	/*
	 * S2_S3_ROUTING instance 1: I0=6, I1=2, rest=0x1f
	 */
	{
		uint32_t r1[2];
		r1[0] = (6u << 0) | (2u << 5) | (0x1Fu << 10) | (0x1Fu << 15) |
			(0x1Fu << 20) | (0x1Fu << 25) | ((0x1Fu & 3u) << 30);
		r1[1] = ((0x1Fu >> 2) & 7u) | (0x1Fu << 3) | (0x1Fu << 8);
		reg_write64_allports(S2_S3_ROUTINGr + 1, r1);
	}

	/*
	 * S2_S3_ROUTING instance 2: I0=11, I1=1, rest=0x1f
	 */
	{
		uint32_t r2[2];
		r2[0] = (11u << 0) | (1u << 5) | (0x1Fu << 10) | (0x1Fu << 15) |
			(0x1Fu << 20) | (0x1Fu << 25) | ((0x1Fu & 3u) << 30);
		r2[1] = ((0x1Fu >> 2) & 7u) | (0x1Fu << 3) | (0x1Fu << 8);
		reg_write64_allports(S2_S3_ROUTINGr + 2, r2);
	}

	/*
	 * S2 cosweights per COS (override defaults):
	 *   COS 0: 16, COS 1: 0, COS 2: 32, COS 4: 16, COS 5: 16,
	 *   COS 6: 32, COS 7: 16, COS 8: 16, COS 9: 16, COS 10: 16, COS 11: 0
	 */
	{
		static const struct { int cos; uint32_t weight; } s2w[] = {
			{  0, 16 }, {  1,  0 }, {  2, 32 }, {  4, 16 },
			{  5, 16 }, {  6, 32 }, {  7, 16 }, {  8, 16 },
			{  9, 16 }, { 10, 16 }, { 11,  0 },
		};
		int i;
		for (i = 0; i < 11; i++)
			reg_write_allports(S2_COSWEIGHTSr + s2w[i].cos,
					   s2w[i].weight);
	}

	/* ES: scheduling_select = 0x3 */
	reg_write_allports(ESCONFIGr, ES_SCHEDULING_SELECT_MASK);

	/* cosweights: COS0=2, COS1=16, COS2=32, COS3=0 */
	reg_write_allports(COSWEIGHTSr + 0, 2);
	reg_write_allports(COSWEIGHTSr + 1, 16);
	reg_write_allports(COSWEIGHTSr + 2, 32);
	reg_write_allports(COSWEIGHTSr + 3, 0);

	/* minspconfig = 0 */
	for (p = PORT_MIN; p <= PORT_MAX; p++)
		sbus_reg_write(cdk_port_addr(MINSPCONFIGr, p), 0);

	/* cosmask: cosmaskrxen = 1 (bit 7) */
	for (p = PORT_MIN; p <= PORT_MAX; p++)
		sbus_reg_modify(cdk_port_addr(COSMASKr, p),
				1u << COSMASKRXEN_BIT, 1u << COSMASKRXEN_BIT);

	/* es_tdm_config: en_cpu_slot_sharing = 0 (bit 24) */
	sbus_reg_modify(ES_TDM_CONFIGr, 1u << EN_CPU_SLOT_SHARING_BIT, 0);

	fprintf(stderr, "[datapath] phase 6: done\n");
	return 0;
}

/*
 * ===================================================================
 * Phase 6.5: XLPORT xport_reset — WARPcore PHY power-up
 *
 * CRITICAL: Without this, ALL XMAC registers return NAK (status=-5).
 * The WARPcore PHY within each XLPORT block is powered down after
 * chip reset.  XLPORT config registers (XLPORT_MODE_REG etc. at
 * offset 0x80xxx) are accessible, but XMAC registers (TX_CTRL at
 * 0x604, RX_CTRL at 0x606) require the SerDes clocks to be running.
 *
 * Sequence from OpenMDK bcm56840_a0_bmd_reset.c:xport_reset():
 *   1. Set LCREF_EN=1 (select LCPLL clock source)
 *   2. Assert all resets + power down (IDDQ, PWRDWN, PWRDWN_PLL,
 *      RSTB_HW=0, RSTB_PLL=0, RSTB_MDIOREGS=0, FIFOs=0)
 *   3. Clear power-down (IDDQ=0, PWRDWN=0, PWRDWN_PLL=0), wait 100us
 *   4. Release RSTB_HW=1, wait 100us
 *   5. Release RSTB_MDIOREGS=1
 *   6. Release RSTB_PLL=1
 *   7. Wait for TX PLL lock
 *   8. Enable TX FIFOs (TXD1G_FIFO_RSTB=0xf, TXD10G_FIFO_RSTB=1)
 *
 * XLPORT_XGXS_CTRL_REGr field layout (32 bits, CDK offset 0x80237):
 *   [0]    PWRDWN_PLL       (1=power down PLL)
 *   [1]    PWRDWN           (1=power down SerDes)
 *   [8:2]  reserved
 *   [9]    IDDQ             (1=shutdown analog clocks)
 *   [10]   PLLBYP           (1=bypass PLL)
 *   [11]   LCREF_EN         (1=use LCPLL clock)
 *   [12]   RSTB_HW          (0=assert hard reset, 1=release)
 *   [13]   RSTB_MDIOREGS    (0=assert MDIO reset, 1=release)
 *   [14]   RSTB_PLL         (0=assert PLL reset, 1=release)
 *   [18:15] TXD1G_FIFO_RSTB (0=reset, 0xF=all lanes released)
 *   [19]   TXD10G_FIFO_RSTB (0=reset, 1=released)
 * ===================================================================
 */
#define XGXS_CTRL_OFF     0x80237u   /* XLPORT_XGXS_CTRL_REGr offset */
#define XGXS_STATUS_OFF   0x80200u   /* XLPORT_XGXS_STATUS_GEN_REGr offset */
#define XLPORT_MODE_OFF    0x80229u   /* XLPORT_MODE_REGr offset */
#define XLPORT_ENABLE_OFF  0x8022au   /* XLPORT_PORT_ENABLEr offset */
#define XLPORT_XMAC_CTRL_OFF 0x8022bu /* XLPORT_XMAC_CONTROLr offset */

/*
 * XLPORT_MODE_REGr fields:
 *   [1:0]  CORE_PORT_MODE  (0=quad 40G, 2=quad 10G)
 *   [3:2]  PHY_PORT_MODE   (0=quad 40G, 2=quad 10G)
 *   [4]    PORT0_MAC_MODE  (0=XMAC, 1=UNIMAC)
 *   [5]    PORT1_MAC_MODE
 *   [6]    PORT2_MAC_MODE
 *   [7]    PORT3_MAC_MODE
 */
#define XLPORT_MODE_10G  0x0Au  /* CORE=2, PHY=2, all MAC_MODE=0 (XMAC) */
#define XLPORT_MODE_40G  0x00u  /* CORE=0, PHY=0, all MAC_MODE=0 (XMAC) */

/* XLPORT_XGXS_CTRL_REGr fields */
#define XGXS_PWRDWN_PLL   (1u << 0)
#define XGXS_PWRDWN       (1u << 1)
#define XGXS_IDDQ         (1u << 9)
#define XGXS_LCREF_EN     (1u << 11)
#define XGXS_RSTB_HW      (1u << 12)
#define XGXS_RSTB_MDIO    (1u << 13)
#define XGXS_RSTB_PLL     (1u << 14)
#define XGXS_TXD1G_RSTB   (0xfu << 15)
#define XGXS_TXD10G_RSTB  (1u << 19)

static int init_xport_reset(void)
{
	/*
	 * First port of each XLPORT block (16 blocks total).
	 * Blocks 0-11: 4-port groups (xe0,4,8,...,44)
	 * Blocks 12-15: single-port (xe48,49,50,51)
	 */
	static const int first_xe[16] = {
		0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44,
		48, 49, 50, 51
	};
	int blk, ok = 0, fail = 0;

	fprintf(stderr, "[datapath] phase 6.5: xport_reset (WARPcore power-up)\n");

	for (blk = 0; blk < 16; blk++) {
		int xe = first_xe[blk];
		uint32_t addr = xlport_reg_addr(xe, XGXS_CTRL_OFF);
		uint32_t val;

		/*
		 * Step 1: Set LCREF_EN, assert all resets, power down.
		 */
		val = XGXS_LCREF_EN | XGXS_IDDQ | XGXS_PWRDWN | XGXS_PWRDWN_PLL;
		/* RSTB_HW=0, RSTB_PLL=0, RSTB_MDIO=0, FIFOs=0 (all in reset) */
		if (sbus_reg_write(addr, val) < 0) {
			fprintf(stderr,
				"[datapath]   xport_reset blk %d xe%d: "
				"XGXS_CTRL write FAIL\n", blk, xe);
			fail++;
			continue;
		}

		/*
		 * Step 2: Clear power-down (keep LCREF_EN, resets still asserted).
		 */
		val = XGXS_LCREF_EN;
		sbus_reg_write(addr, val);
		usleep(100); /* 100us settling time */

		/*
		 * Step 3: Release RSTB_HW (hard reset).
		 */
		val |= XGXS_RSTB_HW;
		sbus_reg_write(addr, val);
		usleep(100); /* 100us settling time */

		/*
		 * Step 4: Release RSTB_MDIOREGS and RSTB_PLL.
		 */
		val |= XGXS_RSTB_MDIO | XGXS_RSTB_PLL;
		sbus_reg_write(addr, val);

		/*
		 * Step 5: Wait briefly for PLL lock (simplified — BMD polls
		 * XLPORT_XGXS_STATUS_GEN_REGr.TXPLL_LOCK for up to 100ms).
		 */
		usleep(1000); /* 1ms — conservative */

		/*
		 * Step 6: Enable TX FIFOs.
		 */
		val |= XGXS_TXD1G_RSTB | XGXS_TXD10G_RSTB;
		sbus_reg_write(addr, val);

		/*
		 * Step 7: Set XLPORT_MODE_REG for port speed mode.
		 * Blocks 0-11: 4x10G (CORE_PORT_MODE=2, PHY_PORT_MODE=2)
		 * Blocks 12-15: 1x40G (CORE_PORT_MODE=0, PHY_PORT_MODE=0)
		 */
		{
			uint32_t mode_addr = xlport_reg_addr(xe, XLPORT_MODE_OFF);
			uint32_t mode_val = (blk < 12) ? XLPORT_MODE_10G
						       : XLPORT_MODE_40G;
			sbus_reg_write(mode_addr, mode_val);
		}

		/*
		 * Step 8: Enable ports in XLPORT_PORT_ENABLE.
		 * 4-port blocks: enable all 4 lanes (0xF)
		 * Single-port blocks (40G): enable lane 0 only (0x1)
		 */
		{
			uint32_t en_addr = xlport_reg_addr(xe, XLPORT_ENABLE_OFF);
			uint32_t en_val = (blk < 12) ? 0xFu : 0x1u;
			sbus_reg_write(en_addr, en_val);
		}

		/*
		 * Step 9: Assert then deassert XLPORT_XMAC_CONTROL reset.
		 * This brings the XMAC sub-block out of reset so its
		 * registers (TX_CTRL at 0x604, RX_CTRL at 0x606) become
		 * accessible via SCHAN.
		 */
		{
			uint32_t xmac_ctrl_addr = xlport_reg_addr(xe,
						    XLPORT_XMAC_CTRL_OFF);
			sbus_reg_write(xmac_ctrl_addr, 1u); /* assert reset */
			usleep(1000); /* 1ms */
			sbus_reg_write(xmac_ctrl_addr, 0u); /* deassert */
			usleep(100);
		}

		ok++;
	}

	fprintf(stderr,
		"[datapath] xport_reset: %d OK, %d FAIL (of 16 blocks)\n",
		ok, fail);

	/* Verify XMAC accessibility by trying to read XMAC_TX_CTRL on block 0 */
	{
		uint32_t test[2] = {0};
		uint32_t test_addr = xlport_reg_addr(0, XMAC_TX_CTRL_OFF);
		if (sbus_reg_read64(test_addr, test) == 0)
			fprintf(stderr,
				"[datapath] XMAC access check: OK "
				"(TX_CTRL=0x%08x_%08x)\n",
				test[1], test[0]);
		else
			fprintf(stderr,
				"[datapath] XMAC access check: STILL FAILING "
				"— xport_reset may need adjustment\n");
	}

	return (fail > 0) ? -1 : 0;
}

/*
 * ===================================================================
 * Phase 7: Port Pipeline Enable (EPC_LINK_BMAP, MAC TX/RX, EGR_PORT)
 *
 * Without EPC_LINK_BMAP set, the egress pipeline silently drops ALL
 * packets — even with ports enabled and buffers configured. This is
 * the gateway bitmap that allows packets to reach the MAC.
 *
 * XMAC_TX_CTRL/RX_CTRL configure CRC handling, padding, and IPG.
 * rc.soc writes xmac_tx_ctrl=0xc802 but the soc.c script runner
 * uses bde_write_reg (BAR0) which fails for SBUS addresses. We
 * write these via SCHAN here.
 * ===================================================================
 */
static int init_port_pipeline(void)
{
	int p;
	uint32_t epc_bmap[3] = {0, 0, 0};  /* 65 bits: words 0-2 */
	uint32_t efilter[3] = {0, 0, 0};

	fprintf(stderr, "[datapath] phase 7: port pipeline enable\n");

	/*
	 * EPC_LINK_BMAP: enable egress for CPU (port 0) + all front-panel ports.
	 * Bit N = port N. Ports 0-52 = bits 0-52.
	 * word[0] = bits 31:0, word[1] = bits 63:32, word[2] = bit 64
	 */
	epc_bmap[0] = 0xFFFFFFFFu;  /* ports 0-31 */
	epc_bmap[1] = 0x001FFFFFu;  /* ports 32-52 (bits 20:0) */
	epc_bmap[2] = 0;
	sbus_mem_write(EPC_LINK_BMAPm, 0, epc_bmap, 3);
	fprintf(stderr, "[datapath]   EPC_LINK_BMAP = 0x%08x_%08x (ports 0-52)\n",
		epc_bmap[1], epc_bmap[0]);

	/*
	 * ING_EN_EFILTER_BITMAP: enable egress filtering for all ports.
	 * Same bitmap format as EPC_LINK_BMAP.
	 */
	efilter[0] = 0xFFFFFFFFu;
	efilter[1] = 0x001FFFFFu;
	efilter[2] = 0;
	sbus_mem_write(ING_EN_EFILTER_BITMAPm, 0, efilter, 3);

	/*
	 * XMAC_TX_CTRL: set per-port via CDK address path.
	 * Value 0xc802 from rc.soc:
	 *   bit 1    = CRC_MODE[1:0] = 2 (replace CRC)
	 *   bit 11   = PAD_EN (padding enable) — in 0xc802 = no
	 *   bits 16:12 = AVERAGE_IPG
	 * Actually 0xc802 = 0b1100_1000_0000_0010:
	 *   [1:0] = 0x2 (CRC_MODE=2 = append CRC)
	 *   [11]  = 1   (bit 11 — some TX config)
	 *   [15:14] = 3 (0xC = TX threshold/config)
	 *
	 * Write to front-panel ports only (port 0 = CPU has no XMAC).
	 * Use XLPORT block addresses — each block has its own SBUS agent.
	 */
	{
		uint32_t tx_ctrl[2] = { 0xc802u, 0 };
		for (p = PORT_XE_MIN; p <= PORT_MAX; p++)
			sbus_reg_write64(xlport_reg_addr(p - 1, XMAC_TX_CTRL_OFF),
					 tx_ctrl);
	}

	/*
	 * XMAC_RX_CTRL: enable RX, strip CRC, set runt threshold.
	 * STRIP_CRC=1 (bit 2), RUNT_THRESHOLD=64 (bits 10:4).
	 * 64 << 4 | (1 << 2) = 0x0404
	 */
	{
		uint32_t rx_ctrl[2] = { (64u << 4) | (1u << 2), 0 };
		for (p = PORT_XE_MIN; p <= PORT_MAX; p++)
			sbus_reg_write64(xlport_reg_addr(p - 1, XMAC_RX_CTRL_OFF),
					 rx_ctrl);
	}

	/*
	 * EGR_PORT: ensure PORT_TYPE = 0 (Ethernet) for all ports.
	 * 14 words per entry; only need to clear PORT_TYPE in word 0 bits[1:0].
	 * Read 6 words, clear PORT_TYPE, write back.
	 */
	for (p = 0; p < 67; p++) {
		uint32_t egr[6] = {0};
		if (sbus_mem_read(EGR_PORTm, p, egr, 6) < 0)
			memset(egr, 0, sizeof(egr));
		egr[0] &= ~0x7u;  /* clear PORT_TYPE[1:0] + HIGIG2[2] */
		sbus_mem_write(EGR_PORTm, p, egr, 6);
	}

	/*
	 * IFP meter parity errata workaround (from rc.soc).
	 * soc.c fails to write this because it uses BAR0 for SBUS address.
	 */
	sbus_reg_write(IFP_METER_PARITY_CONTROLr, 0);

	fprintf(stderr, "[datapath] phase 7: done\n");
	return 0;
}

/*
 * ===================================================================
 * Phase 8: VLAN / STG / Default VLAN Forwarding
 *
 * Without VLAN 1, STG forwarding state, and PORT_VID=1, the ASIC
 * drops ALL ingress packets (untagged frames have no VLAN → drop).
 * ===================================================================
 */

/* CDK addresses from OpenMDK bcm56840_b0_defs.h */
#define STG_TABm            0x05176000u
#define VLAN_TABm            0x12168000u
#define EGR_VLANm            0x0d260000u

/* L2 flooding control */
#define UNKNOWN_UCAST_BLOCK_MASKm  0x0f174c00u
#define UNKNOWN_MCAST_BLOCK_MASKm  0x0f175000u

static int init_vlan_forwarding(void)
{
	/* Port bitmap: ports 0-52 (CPU + 52 front-panel) */
	const uint32_t pbm_lo = 0xFFFFFFFFu;   /* ports 0-31 */
	const uint32_t pbm_hi = 0x001FFFFFu;   /* ports 32-52 */

	fprintf(stderr, "[datapath] phase 8: VLAN/STG forwarding\n");

	/*
	 * (a) STG_TABm entry 2: all ports in FORWARDING state (0b11).
	 * 2 bits per port, 5 words (160 bits).
	 * Ports 0-52 = bits 0-105, all set to 0b11.
	 */
	{
		uint32_t stg[5] = {0};
		stg[0] = 0xFFFFFFFFu;  /* ports 0-15: all forwarding */
		stg[1] = 0xFFFFFFFFu;  /* ports 16-31 */
		stg[2] = 0xFFFFFFFFu;  /* ports 32-47 */
		stg[3] = 0x000003FFu;  /* ports 48-52 (10 bits) */
		stg[4] = 0;
		if (sbus_mem_write(STG_TABm, 2, stg, 5) < 0)
			fprintf(stderr, "[datapath]   STG_TABm write FAILED\n");
	}

	/*
	 * (b) VLAN_TABm entry 1: VLAN 1 with all ports.
	 * 10 words per entry.
	 * PORT_BITMAP bits [65:0], ING_PORT_BITMAP bits [131:66],
	 * STG=2 (w4 bits[12:4]), VALID=1 (w6 bit 13).
	 */
	{
		uint32_t vlan[10] = {0};
		uint64_t bm = ((uint64_t)pbm_hi << 32) | pbm_lo;

		/* PORT_BITMAP: w[0..2] */
		vlan[0] = pbm_lo;
		vlan[1] = pbm_hi;
		/* w[2] bits[1:0] = 0 (ports 64-65 unused) */

		/* ING_PORT_BITMAP: w[2..4] */
		vlan[2] |= (uint32_t)((bm & 0x3FFFFFFFu) << 2);
		vlan[3]  = (uint32_t)((bm >> 30) & 0xFFFFFFFFu);
		vlan[4]  = (uint32_t)((bm >> 62) & 0x3u);

		/* STG=2 at w[4] bits[12:4] */
		vlan[4] |= (2u << 4);

		/* VALID=1 at w[6] bit 13 */
		vlan[6] = (1u << 13);

		if (sbus_mem_write(VLAN_TABm, 1, vlan, 10) < 0)
			fprintf(stderr, "[datapath]   VLAN_TABm write FAILED\n");
	}

	/*
	 * (c) EGR_VLANm entry 1: egress VLAN with all ports untagged.
	 * 8 words per entry.
	 * VALID=1 (w0 bit 0), STG=2 (w0 bits[9:1]),
	 * UT_PORT_BITMAP (w3..w5), PORT_BITMAP (w5..w7).
	 */
	{
		uint32_t egr[8] = {0};
		uint64_t bm = ((uint64_t)pbm_hi << 32) | pbm_lo;

		/* VALID=1, STG=2 */
		egr[0] = 1u | (2u << 1);

		/* UT_PORT_BITMAP bits [161:96]: w[3], w[4], w[5][1:0] */
		egr[3] = pbm_lo;
		egr[4] = pbm_hi;

		/* PORT_BITMAP bits [227:162]: w[5][31:2], w[6], w[7][3:0] */
		egr[5] |= (uint32_t)((bm & 0x3FFFFFFFu) << 2);
		egr[6]  = (uint32_t)((bm >> 30) & 0xFFFFFFFFu);
		egr[7]  = (uint32_t)((bm >> 62) & 0xFu);

		if (sbus_mem_write(EGR_VLANm, 1, egr, 8) < 0)
			fprintf(stderr, "[datapath]   EGR_VLANm write FAILED\n");
	}

	/*
	 * (d) UNKNOWN_UCAST_BLOCK_MASK / UNKNOWN_MCAST_BLOCK_MASK:
	 * Zero all entries to allow flooding of unknown unicast and
	 * multicast packets to all ports in the VLAN.
	 * Bit=1 BLOCKS flooding to that port; all-zeros = flood everywhere.
	 * 67 entries (indexed 0-66), 3 words each (66-bit port bitmap).
	 */
	{
		uint32_t zero[3] = {0, 0, 0};
		int i;
		for (i = 0; i < 67; i++) {
			sbus_mem_write(UNKNOWN_UCAST_BLOCK_MASKm, i, zero, 3);
			sbus_mem_write(UNKNOWN_MCAST_BLOCK_MASKm, i, zero, 3);
		}
	}

	fprintf(stderr, "[datapath] phase 8: done\n");
	return 0;
}

/*
 * ===================================================================
 * Main entry point — called from bcm56846_chip_init()
 * ===================================================================
 */
int bcm56846_datapath_init(void)
{
	fprintf(stderr, "[datapath] BCM56846 datapath initialization begin\n");

	if (init_ingress_buffers() < 0)
		goto fail;
	if (init_priority_mapping() < 0)
		goto fail;
	if (init_forwarding() < 0)
		goto fail;
	if (init_egress_config() < 0)
		goto fail;
	if (init_thdo_tables() < 0)
		goto fail;
	if (init_scheduling() < 0)
		goto fail;
	if (init_xport_reset() < 0)
		fprintf(stderr,
			"[datapath] WARNING: xport_reset had failures"
			" (XMAC access may fail)\n");
	if (init_port_pipeline() < 0)
		goto fail;
	if (init_vlan_forwarding() < 0)
		goto fail;

	fprintf(stderr, "[datapath] BCM56846 datapath initialization complete\n");
	return 0;

fail:
	fprintf(stderr, "[datapath] WARNING: datapath initialization failed\n");
	return -1;
}
