# FRR (FRRouting) for PPC32

Phase 4 of the plan requires FRR (BGP, OSPF, static routes) running on the switch. Routes installed by FRR into the kernel are picked up by nos-switchd via netlink and programmed into the ASIC.

## Using Debian package (recommended)

The rootfs build (`rootfs/build.sh`) installs FRR from the Debian Bookworm archive:

```bash
chroot /mnt/rootfs apt-get install -y frr ifupdown2 lldpd
```

Debian provides PPC32 packages for `frr`, so no cross-build is required when building the rootfs on an x86 host with `qemu-user-static`. The resulting rootfs will have `zebra`, `bgpd`, `ospfd`, etc. in `/usr/lib/frr/` or `/usr/sbin/`.

## Building FRR from source (PPC32)

If you need a specific FRR version or Debian package is unavailable:

1. Clone FRR: `git clone https://github.com/FRRouting/frr.git && cd frr`
2. Install build deps (on build host): `apt-get install autoconf automake libtool make gcc-powerpc-linux-gnu libc6-dev-powerpc-cross libpam0g-dev libjson-c-dev libsystemd-dev`
3. Bootstrap and configure for powerpc:
   ```bash
   ./bootstrap.sh
   ./configure --host=powerpc-linux-gnu --prefix=/usr --sysconfdir=/etc/frr --sbindir=/usr/sbin
   make -j$(nproc)
   make DESTDIR=/mnt/rootfs install
   ```
4. Add to rootfs overlay: `etc/frr/frr.conf`, `etc/frr/bgpd.conf`, etc., and enable `frr.service`.

## Verification

After boot on target: start nos-switchd, then start FRR. Add a BGP neighbor or static route; `ip route` should show the route and nos-switchd will program the ASIC. Use `ping` across ports to verify hardware forwarding.
