/* BCM56846 / Trident+ register offsets from BAR0 (RE docs) */
#ifndef BCM56846_REGS_H
#define BCM56846_REGS_H

#define CMIC_CMC0_SCHAN_CTRL      0x32800
#define CMICM_CMC_BASE            0x31000
#define CMICM_DMA_CTRL(ch)        (0x31140 + 4 * (ch))
#define CMICM_DMA_DESC0(ch)       (0x31158 + 4 * (ch))
#define CMICM_DMA_HALT_ADDR(ch)   (0x31120 + 4 * (ch))
#define CMICM_DMA_STAT            0x31150
#define CMIC_MIIM_PARAM           0x00000158
#define CMIC_MIIM_ADDRESS         0x000004a0

#endif
