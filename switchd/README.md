# nos-switchd — Control Plane Daemon

Replaces Cumulus `switchd`. The central process that bridges Linux kernel networking state
and the BCM56846 ASIC via libbcm56846.

## Responsibilities

1. Create TUN devices (`swp1..swp52`, plus breakout ports) at startup
2. Initialize the ASIC via libbcm56846
3. Listen for netlink events and program the ASIC accordingly
4. Bridge packet I/O between TUN file descriptors and the ASIC DMA engine
5. Poll physical link state and synthesize link events for FRR

## Key Flows

### Startup
```
1. Read ports.conf → build port-to-BCM-port map
2. Open /dev/net/tun × N ports, ioctl TUNSETIFF for each swp interface
3. bcm56846_attach(0), bcm56846_init(0, "/etc/nos/config.bcm")
4. bcm56846_rx_start(0) with callback = rx_deliver_to_tun()
5. Start netlink listener thread
6. Start link-state polling thread (200 ms interval)
7. Start TX polling thread (epoll on TUN fds)
```

### Netlink → ASIC

**Netlink subscription**: `RTMGRP_LINK | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE | RTMGRP_NEIGH | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR`

| Event | Handler | ASIC Op |
|-------|---------|---------|
| RTM_NEWLINK (up) | `handle_link_up()` | `bcm56846_port_enable_set(port, 1)` |
| RTM_NEWLINK (down) | `handle_link_down()` | `bcm56846_port_enable_set(port, 0)` |
| RTM_NEWADDR | `handle_new_addr()` | `bcm56846_l3_intf_create()` → write `EGR_L3_INTF` (SA_MAC + VLAN) |
| RTM_DELADDR | `handle_del_addr()` | `bcm56846_l3_intf_destroy()` (if refcount = 0) |
| RTM_NEWROUTE | `handle_new_route()` | `bcm56846_l3_egress_create()` + `bcm56846_l3_route_add()` |
| RTM_DELROUTE | `handle_del_route()` | `bcm56846_l3_route_delete()` |
| RTM_NEWNEIGH | `handle_new_neigh()` | `bcm56846_l2_addr_add()` + `bcm56846_l3_host_add()` |
| RTM_DELNEIGH | `handle_del_neigh()` | `bcm56846_l2_addr_delete()` |

> **RTM_NEWADDR is critical**: Without handling this event, `EGR_L3_INTF` entries are never
> created. Every egress next-hop object references an `EGR_L3_INTF` entry that contains the
> outgoing interface's source MAC and VLAN. Omitting `RTMGRP_IPV4_IFADDR` from the netlink
> subscription causes L3 routing to silently fail even when routes are present in the kernel FIB.

### Link State Polling

RTM_NEWLINK fires on admin-state changes (`ip link set swp1 up/down`) but NOT on physical link
events (cable plug/unplug). A separate thread polls the ASIC for physical link status:

```
link_poll_thread():
  while (running):
    for each port in port_list:
      new_state = bcm56846_port_link_status_get(unit, port)
      if new_state != last_state[port]:
        last_state[port] = new_state
        if new_state == LINK_DOWN:
          /* Set TUN carrier down: SIOCSIFFLAGS IFF_DOWN on swpN */
          /* Flush neighbors: RTM_DELNEIGH for all neighbors on port */
          /* FRR detects carrier down via netlink and withdraws routes */
        else:
          /* Set TUN carrier up: SIOCSIFFLAGS IFF_UP on swpN */
    sleep(200ms)
```

Without this polling loop, BFD/BGP hold timers are the only failover mechanism, which
takes seconds rather than milliseconds.

### Packet TX (CPU → Port)
```
epoll on TUN fds → read(tun_fd, buf, MTU) → bcm56846_tx(unit, port, buf, len)
```

### Packet RX (Port → CPU)
```
bcm56846_rx_start() callback → write(tun_fd[ingress_port], pkt, len)
```

## Thread Architecture

```
nos-switchd
├── main thread        — SDK init, TUN creation, signal handling
├── netlink thread     — poll(netlink_fd), RTM_* dispatch, SDK calls (serialized via mutex)
├── link-poll thread   — 200ms poll, ASIC link status, carrier update + neighbor flush
├── tx thread          — epoll(TUN fds), bcm56846_tx()
└── rx thread          — bcm56846_rx_start() callback → TUN write
```

## Config Files

- `/etc/nos/ports.conf` — port mode (10G/40G/4x10G)
- `/etc/nos/porttab` — swpN ↔ xeM mapping
- `/etc/nos/config.bcm` — merged ASIC config (includes all 52 portmap_N.0 entries — required)
- `/etc/nos/rc.forwarding` — punt behavior, ECMP hash config
- `/etc/nos/rc.datapath_0` — static datapath pipeline config (from live switch capture)
- `/etc/nos/led0.hex`, `/etc/nos/led1.hex` — ASIC LED microcode programs

## RE References

- `../../docs/reverse-engineering/PACKET_IO_VERIFIED.md` — TUN mechanism confirmed
- `../../docs/reverse-engineering/netlink-handlers.md` — handler logic
- `../../docs/reverse-engineering/netlink-message-flow.md` — message flow
- `../../docs/reverse-engineering/api-patterns.md` — bcm_* → opennsl_* mapping (our API is similar)
- `../../docs/reverse-engineering/COMPLETE_INTERFACE_ANALYSIS.md` — TUN fd layout
- `../../docs/reverse-engineering/QSFP_BREAKOUT_CONFIGURATION.md` — breakout port numbering
- `../../docs/reverse-engineering/SDK_AND_ASIC_CONFIG_FROM_SWITCH.md` — portmap entries, rc.datapath_0
