# Why Some Code Is Still Stubs

We are building an **open NOS with zero Broadcom or Cumulus proprietary code**. Everything is implemented from reverse-engineering (RE) docs, live switch captures, and static analysis of binaries we can inspect. Stubs exist only where the RE gives us the **API and data layout** but not yet the **exact hardware encoding** for the operation.

## What we have vs what we need

| Component | We have (from RE) | We still need | Stub? |
|-----------|-------------------|---------------|-------|
| **L2 table** | Entry layout (4 words), hash formula, table base 0x07120000 | S-Channel WRITE_MEMORY encoding for L2_ENTRY (opcode + address) | We now **try** the RE format (see below); may need tuning on hardware |
| **L3 / ECMP / intf** | Table layouts, field bit positions (L3_NEXTHOP_FORMAT, etc.) | Same S-Chan or DMA encoding per table | Yes (return 0) until we wire each table |
| **Port enable / link** | BAR diff offsets (counters that *change* on port up) | **Control** register addresses and write sequence (inside SDK bctrl handler) | Yes; RE says “actual enable registers written before port up” so BAR diff didn’t capture them |
| **Packet I/O** | TX/RX path, buffer layout, DCB | DMA descriptor format and ring programming | Yes (TX/RX no-op) until we implement DMA rings |

So: we **can** build the full control and data path in our code; the “skeletons” are only where the **hardware write path** (register or S-Channel encoding) is not yet fully decoded in the RE docs.

## What would remove each stub

- **L2/L3/ECMP table writes**: Use S-Channel WRITE_MEMORY format from [SWITCHD_L3_ROUTE_PROGRAMMING_ANALYSIS.md](../docs/reverse-engineering/SWITCHD_L3_ROUTE_PROGRAMMING_ANALYSIS.md) (Word 0 = 0x28…, Word 1 = block+address, Words 2+ = payload). We wire L2 this way; L3/ECMP/intf need the correct base address and word count per table.
- **Port enable / link status**: GDB on Cumulus switchd at the port handler’s `bctrl`, or Ghidra on the switchd binary for the code that writes BAR0 in the 0x4xxx range before link comes up. See [PORT_BRINGUP_REGISTER_MAP.md](../docs/reverse-engineering/PORT_BRINGUP_REGISTER_MAP.md) §6.
- **Packet TX/RX**: Implement DMA ring setup and descriptor format from [DMA_DCB_LAYOUT_FROM_KNET.md](../docs/reverse-engineering/DMA_DCB_LAYOUT_FROM_KNET.md) / [PACKET_BUFFER_ANALYSIS.md](../docs/reverse-engineering/PACKET_BUFFER_ANALYSIS.md).

## L2 table write — we try the RE format

RE says: S-Channel WRITE_MEMORY has Word 0 = `0x28_xx_xx_xx` (opcode 0x28 + word_count), Word 1 = block + table address. For L2_ENTRY (base 0x07120000, 4 words per entry), we call the BDE S-Channel with that encoding. If the ASIC rejects it, we need a capture of the exact command from a working stack (e.g. GDB or DMA trace).

---

## Do we have what we need to get rid of the stubs?

**Yes.** The RE docs contain enough to remove every current stub:

| Stub | RE doc | What we have | What to do |
|------|--------|---------------|------------|
| **L2 table** | L2_ENTRY_FORMAT, SWITCHD_L3_ROUTE_PROGRAMMING_ANALYSIS | Layout, hash, base 0x07120000, S-Chan format | Done — `schan_write_memory` + L2 pack/hash. |
| **L3 intf** | L3_NEXTHOP_FORMAT | EGR_L3_INTF @ **0x01264000**, 15B = 4 words, field bits (VID, MAC) | Call `schan_write_memory(unit, 0x01264000 + index*16, words, 4)`; pack from l3_intf. |
| **L3 egress** | L3_NEXTHOP_FORMAT | ING_L3_NEXT_HOP @ **0x0e17c000** (5B=2w), EGR_L3_NEXT_HOP @ **0x0c260000** (15B=4w); field bits (PORT_NUM, MAC, INTF_NUM) | Same S-Chan write; base + index × stride. |
| **L3 route / ECMP** | L3_NEXTHOP_FORMAT | L3_ECMP @ **0x0e176000** (2B), L3_ECMP_GROUP @ **0x0e174000** (25B=7w). L3_DEFIP via TCAM DMA (S-Chan confirmed in RE). | ECMP/group: S-Chan write. L3_DEFIP: try WRITE_MEMORY or TCAM opcode. |
| **Port enable** | PORT_BRINGUP_REGISTER_MAP §9 | Full XLPORT block map (xe0–xe51 → block_base). XLPORT_PORT_ENABLE = block_base + 0x80000 + 0x22a. | BDE reg write at BAR0 offset = (addr - 0x04000000). Map swp N → xe(N-1) → block_base from §9.1. |
| **Port link status** | PORT_BRINGUP_REGISTER_MAP §9.3 | MAC_MODE = block_base + lane×0x1000 + 0x511; LINK_STATUS bit. | `bde_read_reg(bar0_offset)` then test LINK_STATUS. |
| **Packet TX/RX** | PKTIO_BDE_DMA_INTERFACE | DCB type 21, 16 words; TX word[0]=buf_phys, word[4][7:0]=LOCAL_DEST_PORT; channels TX=0, RX=1–3; CMICM_DMA_CTRL/DESC0/HALT. | Alloc in BDE DMA pool; build DCB ring; program DESC0; poll or sem/irq. |

So the remaining work is **implementation** (pack layouts, port→block map, DCB ring setup), not more RE. Caveats: L3_DEFIP may need TCAM opcode; port SerDes full init is in SERDES_WC_INIT if we want first-time link-up; BAR0 offset = physical addr minus 0x04000000.
