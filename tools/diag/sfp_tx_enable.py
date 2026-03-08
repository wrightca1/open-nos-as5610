import struct, fcntl, os, time

def i2c_read(bus, addr, reg):
    fd = os.open("/dev/i2c-%d" % bus, os.O_RDWR)
    fcntl.ioctl(fd, 0x0706, addr)
    os.write(fd, bytes([reg]))
    data = os.read(fd, 1)
    os.close(fd)
    return data[0] if data else -1

def i2c_write(bus, addr, reg, val):
    fd = os.open("/dev/i2c-%d" % bus, os.O_RDWR)
    fcntl.ioctl(fd, 0x0706, addr)
    os.write(fd, bytes([reg, val]))
    os.close(fd)

# WARPcore access
bde_fd = open("/dev/nos-bde", "rb+", buffering=0)
def rr(off):
    buf = bytearray(struct.pack("II", off, 0))
    fcntl.ioctl(bde_fd, 0xC0084201, buf)
    return struct.unpack("II", buf)[1]
def ww(off, val):
    buf = bytearray(struct.pack("II", off, val))
    fcntl.ioctl(bde_fd, 0x40084202, buf)
def miim_read(phy_id, reg, bus_id=0):
    ww(0x0050, 16); ww(0x0050, 18)
    ww(0x04A0, reg & 0x1F)
    param = ((phy_id & 0x1F) << 16) | (1 << 25) | ((bus_id & 0x7) << 22)
    ww(0x0158, param)
    ww(0x0050, 0x90)
    for _ in range(2000):
        if rr(0x0050) & 0x00040000:
            data = rr(0x015C) & 0xFFFF
            ww(0x0050, 18)
            return data
    ww(0x0050, 16)
    return -1
def miim_write(phy_id, reg, val, bus_id=0):
    ww(0x0050, 17); ww(0x0050, 18)
    ww(0x04A0, reg & 0x1F)
    param = ((phy_id & 0x1F) << 16) | (val & 0xFFFF) | (1 << 25) | ((bus_id & 0x7) << 22)
    ww(0x0158, param)
    ww(0x0050, 0x91)
    for _ in range(2000):
        if rr(0x0050) & 0x00040000:
            ww(0x0050, 18)
            return 0
    ww(0x0050, 17)
    return -1
def wc_read(phy, blk, off, bus=0):
    miim_write(phy, 0x1F, 0xFFD0, bus)
    miim_write(phy, 0x1E, 0, bus)
    miim_write(phy, 0x1F, blk, bus)
    return miim_read(phy, 0x10 + (off & 0xF), bus) & 0xFFFF

ww(0x01BC, (1 << 16) | 99)
ww(0x01B8, (1 << 16) | 396)

PHY = 17
BUS_MDIO = 2
RETIMER_ADDR = 0x27

# ONLP sfpi.c TX_DISABLE mapping:
# PCA9506 @ 0x24 on mux@0x76 ch3 (= Linux i2c bus 17)
# Ports 0-7:  IOC0 (0x18) / OP0 (0x08), bits 0-7
# Ports 8-15: IOC1 (0x19) / OP1 (0x09), bits 0-7
# ...
# To enable TX: set IOC bit to 0 (output), set OP bit to 1 (HIGH = TX enabled)

# ONLP SFP signal mapping (all on mux@0x76 ch3 = bus 17):
#   SFP_PRESENT:   PCA9506 @ 0x20 (ports 0-39), 0x23 (ports 40-47)
#   TX_FAULT:      PCA9506 @ 0x21 (ports 0-39), 0x23 (ports 40-47)
#   RX_LOS:        PCA9506 @ 0x22 (ports 0-39), 0x23 (ports 40-47)
#   TX_DISABLE:    PCA9506 @ 0x24 (ports 0-39), 0x23 (ports 40-47)

GPIO_BUS = 17
TX_DIS_ADDR = 0x24
PRESENT_ADDR = 0x20
TXFAULT_ADDR = 0x21

print("=" * 70)
print("SFP TX Enable + Retimer Init + SIGDET Check")
print("=" * 70)

# Step 1: Check current GPIO state
print("\n--- Step 1: Current GPIO state (bus %d) ---" % GPIO_BUS)
# SFP Present (0x20, active LOW = module present)
pres = i2c_read(GPIO_BUS, PRESENT_ADDR, 0x00)
print("  SFP_PRESENT (0x20 IP0) = 0x%02x" % pres)
for p in range(8):
    present = "PRESENT" if not ((pres >> p) & 1) else "empty"
    print("    Port%d: %s" % (p+1, present))

# TX Fault (0x21, active HIGH = fault)
txf = i2c_read(GPIO_BUS, TXFAULT_ADDR, 0x00)
print("  TX_FAULT (0x21 IP0) = 0x%02x" % txf)

# RX_LOS - check if 0x22 exists
try:
    rxl = i2c_read(GPIO_BUS, 0x22, 0x00)
    print("  RX_LOS (0x22 IP0) = 0x%02x" % rxl)
except:
    print("  RX_LOS (0x22): not found")

# TX Disable state (0x24)
ioc0 = i2c_read(GPIO_BUS, TX_DIS_ADDR, 0x18)
op0 = i2c_read(GPIO_BUS, TX_DIS_ADDR, 0x08)
ip0 = i2c_read(GPIO_BUS, TX_DIS_ADDR, 0x00)
print("  TX_DISABLE (0x24): IOC0=0x%02x OP0=0x%02x IP0=0x%02x" % (ioc0, op0, ip0))
print("    IOC0=0xFF means ALL pins are inputs (TX_DISABLE FLOATING/UNCONTROLLED)")

# Step 2: Enable TX on ports 1-4
print("\n--- Step 2: Enable TX on ports 1-4 ---")
# Set IOC0 bits 0-3 to OUTPUT (clear bits)
ioc0_new = ioc0 & 0xF0  # Clear bits 0-3 (set as output)
i2c_write(GPIO_BUS, TX_DIS_ADDR, 0x18, ioc0_new)
print("  IOC0: 0x%02x -> 0x%02x (bits 0-3 = output)" % (ioc0, ioc0_new))

# Set OP0 bits 0-3 to HIGH (TX enabled = TX_DISABLE deasserted)
op0_cur = i2c_read(GPIO_BUS, TX_DIS_ADDR, 0x08)
op0_new = op0_cur | 0x0F  # Set bits 0-3 HIGH
i2c_write(GPIO_BUS, TX_DIS_ADDR, 0x08, op0_new)
print("  OP0: 0x%02x -> 0x%02x (bits 0-3 = HIGH = TX enabled)" % (op0_cur, op0_new))

# Verify
ioc0_rb = i2c_read(GPIO_BUS, TX_DIS_ADDR, 0x18)
op0_rb = i2c_read(GPIO_BUS, TX_DIS_ADDR, 0x08)
ip0_rb = i2c_read(GPIO_BUS, TX_DIS_ADDR, 0x00)
print("  Verify: IOC0=0x%02x OP0=0x%02x IP0=0x%02x" % (ioc0_rb, op0_rb, ip0_rb))

# Check TX_FAULT after enabling
time.sleep(0.5)
txf2 = i2c_read(GPIO_BUS, TXFAULT_ADDR, 0x00)
print("  TX_FAULT after enable: 0x%02x" % txf2)

# Step 3: Init retimers on correct buses (22-25 for ports 1-4)
print("\n--- Step 3: Init retimers (buses 22-25) ---")
PORT_BUSES = {1: 22, 2: 23, 3: 24, 4: 25}
for port, bus in sorted(PORT_BUSES.items()):
    try:
        # Broadcast to all channels
        i2c_write(bus, RETIMER_ADDR, 0xFF, 0x0C)
        # Channel reset
        i2c_write(bus, RETIMER_ADDR, 0x00, 0x04)
        time.sleep(0.02)
        # Lock rate 10.3125 Gbps
        i2c_write(bus, RETIMER_ADDR, 0x2F, 0xC0)
        # VCO divider = 1
        i2c_write(bus, RETIMER_ADDR, 0x18, 0x00)
        # LPF DAC
        i2c_write(bus, RETIMER_ADDR, 0x1F, 0x52)
        # VCO CAP
        i2c_write(bus, RETIMER_ADDR, 0x08, 0x0C)
        # Override
        i2c_write(bus, RETIMER_ADDR, 0x09, 0xEC)
        # Adapt mode 2
        i2c_write(bus, RETIMER_ADDR, 0x31, 0x40)
        # Unmute output, enable DFE
        i2c_write(bus, RETIMER_ADDR, 0x1E, 0x00)
        # VOD
        i2c_write(bus, RETIMER_ADDR, 0x2D, 0x81)
        # No de-emphasis
        i2c_write(bus, RETIMER_ADDR, 0x15, 0x00)
        # Disable SBT
        i2c_write(bus, RETIMER_ADDR, 0x0C, 0x00)
        # Lower signal detect thresholds
        i2c_write(bus, RETIMER_ADDR, 0x0E, 0x00)
        i2c_write(bus, RETIMER_ADDR, 0x0F, 0x00)
        # CDR reset + release
        i2c_write(bus, RETIMER_ADDR, 0x0A, 0x1C)
        time.sleep(0.02)
        i2c_write(bus, RETIMER_ADDR, 0x0A, 0x10)
        print("  Port%d (bus%d): init done" % (port, bus))
    except Exception as e:
        print("  Port%d (bus%d): ERROR %s" % (port, bus, str(e)))

# Step 4: CDR lock poll
print("\n--- Step 4: CDR lock poll ---")
time.sleep(1.0)
for attempt in range(20):
    time.sleep(0.5)
    any_lock = False
    for port, bus in sorted(PORT_BUSES.items()):
        try:
            i2c_write(bus, RETIMER_ADDR, 0xFF, 0x04)
            status = i2c_read(bus, RETIMER_ADDR, 0x02)
            sig = i2c_read(bus, RETIMER_ADDR, 0x01)
            cdr_lock = (status >> 4) & 1
            sbt = (status >> 3) & 1
            extra = ""
            if cdr_lock:
                heo = i2c_read(bus, RETIMER_ADDR, 0x27)
                veo = i2c_read(bus, RETIMER_ADDR, 0x28)
                extra = " ** CDR LOCKED ** HEO=%d VEO=%d" % (heo, veo)
                any_lock = True
            if attempt < 5 or cdr_lock or (attempt % 5 == 0):
                print("  [%2d] Port%d: CDR=0x%02x(lock=%d,sbt=%d) SIG=0x%02x%s" % (
                    attempt+1, port, status, cdr_lock, sbt, sig, extra))
        except:
            pass

    # Check WARPcore SIGDET
    gp0 = wc_read(PHY, 0x81D0, 0, BUS_MDIO)
    sigdet = (gp0 >> 8) & 0xF
    sync = gp0 & 0xF
    pll = (wc_read(PHY, 0x8000, 1, BUS_MDIO) >> 11) & 1
    if attempt < 5 or sigdet or (attempt % 5 == 0):
        print("  [%2d] WARPcore: SIGDET=0x%x SYNC=0x%x PLL=%d" % (
            attempt+1, sigdet, sync, pll))

    if any_lock or sigdet:
        print("  ** SIGNAL DETECTED **")
        break

# Final status
print("\n--- Final status ---")
txf_final = i2c_read(GPIO_BUS, TXFAULT_ADDR, 0x00)
try:
    rxl_final = i2c_read(GPIO_BUS, 0x22, 0x00)
except:
    rxl_final = -1
print("  TX_FAULT=0x%02x RX_LOS=0x%02x" % (txf_final, rxl_final))

# Also check SFP DDM status via EEPROM on intermediate bus
# (bus 10 PCA9548 leaks to whatever channel is selected)
for bus_int in [10]:
    try:
        # Select channel 0 on PCA9548 (port 1)
        fd = os.open("/dev/i2c-%d" % bus_int, os.O_RDWR)
        fcntl.ioctl(fd, 0x0706, 0x74)
        os.write(fd, bytes([0x01]))  # select channel 0
        os.close(fd)
        time.sleep(0.1)
        # Read SFP DDM
        sc = i2c_read(bus_int, 0x51, 110)
        rxp_h = i2c_read(bus_int, 0x51, 104)
        rxp_l = i2c_read(bus_int, 0x51, 105)
        rxp_uw = ((rxp_h << 8) | rxp_l) / 10.0
        txp_h = i2c_read(bus_int, 0x51, 102)
        txp_l = i2c_read(bus_int, 0x51, 103)
        txp_uw = ((txp_h << 8) | txp_l) / 10.0
        rxlos_sfp = sc & 1
        txflt_sfp = (sc >> 2) & 1
        print("  Port1 SFP DDM: RxP=%.1fuW TxP=%.1fuW RX_LOS=%d TX_FAULT=%d" % (
            rxp_uw, txp_uw, rxlos_sfp, txflt_sfp))
    except Exception as e:
        print("  Port1 SFP DDM: ERROR %s" % str(e))

gp0 = wc_read(PHY, 0x81D0, 0, BUS_MDIO)
sigdet = (gp0 >> 8) & 0xF
sync = gp0 & 0xF
print("  WARPcore: GP0=0x%04x SIGDET=0x%x SYNC=0x%x" % (gp0, sigdet, sync))

miim_write(PHY, 0x1F, 0, BUS_MDIO)
bde_fd.close()
print("\nDone.")
