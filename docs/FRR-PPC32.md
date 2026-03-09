# FRR (FRRouting) for PPC32

Phase 4 of the plan requires FRR (BGP, OSPF, static routes) running on the switch. Routes installed by FRR into the kernel are picked up by nos-switchd via netlink and programmed into the ASIC.

## Using Debian package (recommended)

The rootfs build (`rootfs/build.sh`) attempts to install FRR from the Debian jessie archive (note: FRR is not available in jessie, so this will fail and must be cross-compiled separately):

```bash
chroot /mnt/rootfs apt-get install -y frr ifupdown2 lldpd
```

Debian jessie does **not** have FRR packages (FRR postdates jessie). The `apt install frr` call in `build.sh` is expected to fail. FRR must be cross-compiled from source (see below).

## Building FRR from source (PPC32)

Since Debian jessie does not ship FRR, cross-compilation is required:

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
