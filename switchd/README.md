# nos-switchd — Control Plane Daemon

Replaces Cumulus `switchd`. The central process that bridges Linux kernel networking state
and the BCM56846 ASIC via libbcm56846.

## Responsibilities

1. Create TUN devices (`swp1..swp52`, plus breakout ports) at startup
2. Initialize the ASIC via libbcm56846
3. Listen for netlink events and program the ASIC accordingly
4. Bridge packet I/O between TUN file descriptors and the ASIC DMA engine

## Key Flows

### Startup
```
1. Read ports.conf → build port-to-BCM-port map
2. Open /dev/net/tun × N ports, ioctl TUNSETIFF for each swp interface
3. bcm56846_attach(0), bcm56846_init(0, "/etc/nos/config.bcm")
4. bcm56846_rx_start(0) with callback = rx_deliver_to_tun()
5. Start netlink listener thread
6. Start TX polling thread (epoll on TUN fds)
```

### Netlink → ASIC

| Event | Handler | ASIC Op |
|-------|---------|---------|
| RTM_NEWLINK (up) | `handle_link_up()` | `bcm56846_port_enable_set(port, 1)` |
| RTM_NEWLINK (down) | `handle_link_down()` | `bcm56846_port_enable_set(port, 0)` |
| RTM_NEWROUTE | `handle_new_route()` | `bcm56846_l3_egress_create()` + `bcm56846_l3_route_add()` |
| RTM_DELROUTE | `handle_del_route()` | `bcm56846_l3_route_delete()` |
| RTM_NEWNEIGH | `handle_new_neigh()` | `bcm56846_l2_addr_add()` + `bcm56846_l3_host_add()` |
| RTM_DELNEIGH | `handle_del_neigh()` | `bcm56846_l2_addr_delete()` |

### Packet TX (CPU → Port)
```
epoll on TUN fds → read(tun_fd, buf, MTU) → bcm56846_tx(unit, port, buf, len)
```

### Packet RX (Port → CPU)
```
bcm56846_rx_start() callback → write(tun_fd[ingress_port], pkt, len)
```

## Config Files

- `/etc/nos/ports.conf` — port mode (10G/40G/4x10G)
- `/etc/nos/porttab` — swpN ↔ xeM mapping
- `/etc/nos/config.bcm` — merged ASIC config
- `/etc/nos/rc.forwarding` — punt behavior, ECMP hash config

## RE References

- `../edgecore-5610-re/PACKET_IO_VERIFIED.md` — TUN mechanism confirmed
- `../edgecore-5610-re/netlink-handlers.md` — handler logic
- `../edgecore-5610-re/netlink-message-flow.md` — message flow
- `../edgecore-5610-re/api-patterns.md` — bcm_* → opennsl_* mapping (our API is similar)
- `../edgecore-5610-re/COMPLETE_INTERFACE_ANALYSIS.md` — TUN fd layout
- `../edgecore-5610-re/QSFP_BREAKOUT_CONFIGURATION.md` — breakout port numbering
