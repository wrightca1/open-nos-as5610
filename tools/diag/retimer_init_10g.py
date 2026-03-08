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

# CORRECT bus mapping: port N = bus 21 + N
# Port 1 = bus 22, Port 2 = bus 23, Port 3 = bus 24, Port 4 = bus 25
PORT_BUSES = {1: 22, 2: 23, 3: 24, 4: 25}

print("=" * 70)
print("DS100DF410 Init on CORRECT Per-Port Buses")
print("=" * 70)

# Step 1: Check SFP on correct buses (read past at24 driver)
print("\n--- Step 1: SFP Check (correct buses) ---")
active_ports = []
for port, bus in sorted(PORT_BUSES.items()):
    try:
        # Try reading SFP vendor from A0 EEPROM
        vendor = ""
        for j in range(16):
            c = i2c_read(bus, 0x50, 20 + j)
            if 32 <= c <= 126:
                vendor = vendor + chr(c)
        vendor = vendor.strip()
        # DDM power
        try:
            rxp_h = i2c_read(bus, 0x51, 104)
            rxp_l = i2c_read(bus, 0x51, 105)
            rxp_uw = ((rxp_h << 8) | rxp_l) / 10.0
        except:
            rxp_uw = 0
        print("  Port%d (bus%d): %s  RxP=%.1fuW" % (port, bus, vendor, rxp_uw))
        active_ports.append((port, bus))
    except:
        # at24 driver may be bound - try through intermediate bus
        print("  Port%d (bus%d): at24 bound or no SFP" % (port, bus))
        # Try to check if retimer exists
        try:
            i2c_write(bus, RETIMER_ADDR, 0xFF, 0x00)
            dev_id = i2c_read(bus, RETIMER_ADDR, 0x01)
            print("    retimer responds: dev_id=0x%02x" % dev_id)
            active_ports.append((port, bus))
        except:
            print("    retimer NOT found")

# Step 2: Read retimer state on correct buses
print("\n--- Step 2: Retimer state on correct buses ---")
for port, bus in active_ports:
    print("\n  Port%d (bus%d):" % (port, bus))
    try:
        # Select shared registers
        i2c_write(bus, RETIMER_ADDR, 0xFF, 0x00)
        print("    Shared: dev_id=0x%02x" % i2c_read(bus, RETIMER_ADDR, 0x01))

        # Select channel 0
        i2c_write(bus, RETIMER_ADDR, 0xFF, 0x04)
        status = i2c_read(bus, RETIMER_ADDR, 0x02)
        sig = i2c_read(bus, RETIMER_ADDR, 0x01)
        mux_dfe = i2c_read(bus, RETIMER_ADDR, 0x1E)
        lock_rate = i2c_read(bus, RETIMER_ADDR, 0x2F)
        adapt = i2c_read(bus, RETIMER_ADDR, 0x31)
        cdr_lock = (status >> 4) & 1
        out_mux = (mux_dfe >> 5) & 7
        dfe_pd = (mux_dfe >> 3) & 1
        print("    Ch0: CDR_STAT=0x%02x(lock=%d) SIG=0x%02x MUX/DFE=0x%02x(mux=%d,dfepd=%d) LOCK_RATE=0x%02x ADAPT=0x%02x" % (
            status, cdr_lock, sig, mux_dfe, out_mux, dfe_pd, lock_rate, adapt))
    except Exception as e:
        print("    ERROR: %s" % str(e))

# Step 3: Initialize retimers for 10G
print("\n--- Step 3: Initialize retimers for 10G SFI ---")
for port, bus in active_ports:
    print("\n  Port%d (bus%d):" % (port, bus))
    try:
        # Broadcast to all channels
        i2c_write(bus, RETIMER_ADDR, 0xFF, 0x0C)

        # Channel reset
        i2c_write(bus, RETIMER_ADDR, 0x00, 0x04)
        time.sleep(0.02)

        # Lock rate for 10.3125 Gbps
        i2c_write(bus, RETIMER_ADDR, 0x2F, 0xC0)

        # VCO divider = 1
        i2c_write(bus, RETIMER_ADDR, 0x18, 0x00)

        # LPF DAC for 10G
        i2c_write(bus, RETIMER_ADDR, 0x1F, 0x52)

        # VCO CAP for 10G
        i2c_write(bus, RETIMER_ADDR, 0x08, 0x0C)

        # Override enables
        i2c_write(bus, RETIMER_ADDR, 0x09, 0xEC)

        # Adapt mode 2 (CTLE+DFE)
        i2c_write(bus, RETIMER_ADDR, 0x31, 0x40)

        # Unmute output, enable DFE
        i2c_write(bus, RETIMER_ADDR, 0x1E, 0x00)

        # VOD
        i2c_write(bus, RETIMER_ADDR, 0x2D, 0x81)

        # No de-emphasis
        i2c_write(bus, RETIMER_ADDR, 0x15, 0x00)

        # Disable SBT
        i2c_write(bus, RETIMER_ADDR, 0x0C, 0x00)

        # Signal detect thresholds (lower)
        i2c_write(bus, RETIMER_ADDR, 0x0E, 0x00)
        i2c_write(bus, RETIMER_ADDR, 0x0F, 0x00)

        # CDR reset + release
        i2c_write(bus, RETIMER_ADDR, 0x0A, 0x1C)
        time.sleep(0.02)
        i2c_write(bus, RETIMER_ADDR, 0x0A, 0x10)

        # Verify channel 0
        i2c_write(bus, RETIMER_ADDR, 0xFF, 0x04)
        lr = i2c_read(bus, RETIMER_ADDR, 0x2F)
        md = i2c_read(bus, RETIMER_ADDR, 0x1E)
        ad = i2c_read(bus, RETIMER_ADDR, 0x31)
        print("    Init done. LOCK_RATE=0x%02x MUX_DFE=0x%02x ADAPT=0x%02x" % (lr, md, ad))

    except Exception as e:
        print("    ERROR: %s" % str(e))

# Step 4: CDR lock poll
print("\n--- Step 4: CDR Lock Poll ---")
for attempt in range(15):
    time.sleep(0.5)
    found_lock = False
    for port, bus in active_ports:
        try:
            i2c_write(bus, RETIMER_ADDR, 0xFF, 0x04)
            status = i2c_read(bus, RETIMER_ADDR, 0x02)
            sig = i2c_read(bus, RETIMER_ADDR, 0x01)
            cdr_lock = (status >> 4) & 1
            sbt = (status >> 3) & 1
            extra = ""
            if cdr_lock:
                extra = " ** CDR LOCKED **"
                found_lock = True
            if attempt < 5 or cdr_lock:
                print("  [%2d] Port%d: CDR=0x%02x(lock=%d,sbt=%d) SIG=0x%02x%s" % (
                    attempt+1, port, status, cdr_lock, sbt, sig, extra))
        except:
            pass
    if found_lock:
        break

# Step 5: Check WARPcore SIGDET
print("\n--- Step 5: WARPcore SIGDET ---")
gp0 = wc_read(PHY, 0x81D0, 0, BUS_MDIO)
sigdet = (gp0 >> 8) & 0xF
sync = gp0 & 0xF
pll = (wc_read(PHY, 0x8000, 1, BUS_MDIO) >> 11) & 1
print("  GP0=0x%04x SIGDET=0x%x SYNC=0x%x PLL=%d" % (gp0, sigdet, sync, pll))

miim_write(PHY, 0x1F, 0, BUS_MDIO)
bde_fd.close()
print("\nDone.")
