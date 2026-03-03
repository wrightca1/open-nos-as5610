/*
 * schan_diag.c — BCM56846 S-Channel transport diagnostic
 *
 * Answers three questions:
 *   1. Are ring maps programmed?
 *   2. Does SCHAN_CTRL accept START and what bits are set after completion?
 *   3. Does ANY opcode/address combination get a DONE response?
 *
 * Build on switch (as root):
 *   gcc -O1 -o /tmp/schan_diag /tmp/schan_diag.c && /tmp/schan_diag
 *
 * Key result interpretation:
 *   DONE bit set          → SCHAN transport works; that opcode/addr is valid
 *   NACK bit set (bit 3)  → command dispatched but target didn't respond
 *                           (block in reset, or address doesn't exist)
 *   ERROR bit set (bit 2) → command dispatched but ASIC returned error
 *   ctrl stays = 0x1      → START written but nothing happened (CMC wrong?)
 *   Ring maps = 0x0       → chip_init() never ran; fix startup sequence first
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

struct reg   { uint32_t offset; uint32_t value; };
struct schan { uint32_t cmd[8]; uint32_t data[16]; int32_t len; int32_t status; };

#define BM 'B'
#define RR _IOWR(BM, 1, struct reg)
#define WW _IOW(BM,  2, struct reg)
#define SC _IOWR(BM, 4, struct schan)

static int fd;

static uint32_t rr(uint32_t o)
{
	struct reg r = { o, 0 };
	if (ioctl(fd, RR, &r) < 0) return 0xDEAD0000 | (o & 0xffff);
	return r.value;
}
static void ww(uint32_t o, uint32_t v)
{
	struct reg r = { o, v };
	ioctl(fd, WW, &r);
}

static const char *ctrl_str(uint32_t c)
{
	static char buf[80];
	if (c == 0) return "IDLE";
	buf[0] = '\0';
	if (c & 1) strcat(buf, "START ");
	if (c & 2) strcat(buf, "DONE ");
	if (c & 4) strcat(buf, "ERROR ");
	if (c & 8) strcat(buf, "NACK ");
	if (c & ~0xf) strcat(buf, "+OTHER");
	return buf;
}

/* Issue SCHAN via the nos-bde ioctl (always uses CMC2 in kernel BDE) */
static void schan_ioctl(uint32_t w0, uint32_t addr, const char *label)
{
	struct schan s;
	memset(&s, 0, sizeof(s));
	s.cmd[0] = w0;
	s.cmd[1] = addr;
	s.len    = 2;
	s.status = -1;
	int r = ioctl(fd, SC, &s);
	uint32_t ctrl = rr(0x33000);
	printf("  %-44s ioctl=%2d st=%2d ctrl=0x%08x [%s]\n",
	       label, r, s.status, ctrl, ctrl_str(ctrl));
	if (s.status == 0)
		printf("    -> data[0]=0x%08x data[1]=0x%08x\n",
		       s.data[0], s.data[1]);
}

/*
 * Raw SCHAN test: write directly to BAR0, bypassing ioctl.
 * Used to test CMC0 (0x31000) which the ioctl never uses.
 */
static void schan_raw(uint32_t ctrl_reg, uint32_t msg_base,
                      uint32_t w0, uint32_t addr, const char *label)
{
	/* abort/clear any stale state */
	uint32_t prev = rr(ctrl_reg);
	if (prev) { ww(ctrl_reg, prev | 4); usleep(2000); ww(ctrl_reg, 0); usleep(500); }

	ww(msg_base + 0x00, w0);
	ww(msg_base + 0x04, addr);
	ww(ctrl_reg, 0);
	usleep(100);
	ww(ctrl_reg, 1); /* START */

	uint32_t ctrl = 0;
	for (int i = 0; i < 500; i++) {  /* 500 × 100us = 50ms max */
		usleep(100);
		ctrl = rr(ctrl_reg);
		if (ctrl & 0xe) break; /* DONE or any error bit */
	}
	ww(ctrl_reg, 0); /* clear */
	uint32_t resp = rr(msg_base);
	printf("  %-44s ctrl=0x%08x [%s] resp=0x%08x\n",
	       label, ctrl, ctrl_str(ctrl), resp);
}

int main(void)
{
	fd = open("/dev/nos-bde", O_RDWR);
	if (fd < 0) { perror("open /dev/nos-bde"); return 1; }

	printf("=== BCM56846 SCHAN Diagnostic ===\n\n");

	/* ---- 1. Identity + ring map state ---- */
	printf("--- 1. CMIC identity and ring map state ---\n");
	printf("  DEV_REV_ID  (0x178): 0x%08x  (expect 0x0002b846)\n", rr(0x178));
	printf("  SBUS_TIMEOUT(0x200): 0x%08x  (expect 0x000007d0)\n", rr(0x200));
	printf("  RING_MAP_0  (0x204): 0x%08x  (expect 0x43052100)\n", rr(0x204));
	printf("  RING_MAP_1  (0x208): 0x%08x  (expect 0x33333343)\n", rr(0x208));
	printf("  RING_MAP_2  (0x20c): 0x%08x  (expect 0x44444333)\n", rr(0x20c));
	printf("  RING_MAP_3  (0x210): 0x%08x  (expect 0x00034444)\n", rr(0x210));
	printf("  RING_MAP_4  (0x214): 0x%08x  (expect 0x00000000)\n", rr(0x214));
	printf("\n");

	/* ---- 2. CMC initial state ---- */
	printf("--- 2. CMC SCHAN_CTRL initial state ---\n");
	printf("  CMC0 0x31000: 0x%08x [%s]\n", rr(0x31000), ctrl_str(rr(0x31000)));
	printf("  CMC2 0x33000: 0x%08x [%s]\n", rr(0x33000), ctrl_str(rr(0x33000)));
	printf("\n");

	/* ---- 3. Force-program ring maps then recheck ---- */
	printf("--- 3. Force-program ring maps and verify ---\n");
	ww(0x200, 0x7d0);
	ww(0x204, 0x43052100); ww(0x208, 0x33333343);
	ww(0x20c, 0x44444333); ww(0x210, 0x00034444);
	ww(0x214, 0);          ww(0x218, 0);
	ww(0x21c, 0);          ww(0x220, 0);
	usleep(1000);
	printf("  RING_MAP_0 readback: 0x%08x\n", rr(0x204));
	printf("  RING_MAP_1 readback: 0x%08x\n", rr(0x208));
	printf("\n");

	/* ---- 4. SCHAN via ioctl (kernel BDE, CMC2) ---- */
	printf("--- 4. SCHAN tests via kernel BDE ioctl (CMC2 = 0x33000) ---\n");
	/*
	 * Opcode encoding options:
	 *   Our current:  0x2a = top byte (bits[31:24]), len in bits[7:0]
	 *   BCM std opcode in bits[31:26]:
	 *     0x07 (MEM_READ)  << 26 = 0x1c000000
	 *     0x08 (MEM_WRITE) << 26 = 0x20000000
	 *     0x0b (REG_READ)  << 26 = 0x2c000000
	 *     0x0c (REG_WRITE) << 26 = 0x30000000
	 *
	 * Address 0x00000000 = whatever is at SOC addr 0 (may or may not respond)
	 * Address 0x02000000 = rough area of ING_CONFIG / global pipe regs
	 * Address 0x00000218 = CMIC_MISC_CONTROL (known to exist, but BAR0 reg not SOC)
	 */
	schan_ioctl(0x2a000001, 0x00000000, "0x2a(our-READ) len=1 addr=0");
	schan_ioctl(0x2a000001, 0x02000000, "0x2a(our-READ) len=1 addr=0x2000000");
	schan_ioctl(0x2a000002, 0x00000000, "0x2a(our-READ) len=2 addr=0");
	schan_ioctl(0x1c000001, 0x00000000, "opcode=0x07(MEM_READ)  addr=0");
	schan_ioctl(0x1c000001, 0x02000000, "opcode=0x07(MEM_READ)  addr=0x2000000");
	schan_ioctl(0x2c000001, 0x00000000, "opcode=0x0b(REG_READ)  addr=0");
	schan_ioctl(0x2c000001, 0x02000000, "opcode=0x0b(REG_READ)  addr=0x2000000");
	printf("\n");

	/* ---- 5. Raw CMC0 tests (bypass ioctl) ---- */
	printf("--- 5. Raw SCHAN tests via BAR0: CMC0 (0x31000) ---\n");
	schan_raw(0x31000, 0x3100c, 0x2a000001, 0x00000000, "CMC0 0x2a addr=0");
	schan_raw(0x31000, 0x3100c, 0x1c000001, 0x00000000, "CMC0 opcode=0x07 addr=0");
	schan_raw(0x31000, 0x3100c, 0x2c000001, 0x00000000, "CMC0 opcode=0x0b addr=0");
	schan_raw(0x31000, 0x3100c, 0x2a000001, 0x02000000, "CMC0 0x2a addr=0x2000000");
	printf("\n");

	/* ---- 6. Raw CMC2 tests ---- */
	printf("--- 6. Raw SCHAN tests via BAR0: CMC2 (0x33000) ---\n");
	schan_raw(0x33000, 0x3300c, 0x2a000001, 0x00000000, "CMC2 0x2a addr=0");
	schan_raw(0x33000, 0x3300c, 0x1c000001, 0x00000000, "CMC2 opcode=0x07 addr=0");
	schan_raw(0x33000, 0x3300c, 0x2c000001, 0x00000000, "CMC2 opcode=0x0b addr=0");
	schan_raw(0x33000, 0x3300c, 0x2a000001, 0x02000000, "CMC2 0x2a addr=0x2000000");
	printf("\n");

	/* ---- 7. Final state ---- */
	printf("--- 7. Final CMC state ---\n");
	printf("  CMC0 0x31000: 0x%08x\n", rr(0x31000));
	printf("  CMC2 0x33000: 0x%08x\n", rr(0x33000));

	close(fd);
	return 0;
}
