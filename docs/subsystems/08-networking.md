# Networking

The network stack in `kernel/net/` implements Ethernet and ARP, IPv4 with ICMP echo, UDP, TCP, and BSD sockets on top of one virtio-net NIC (`eth0`) and a loopback interface (`lo`). A kernel thread, `netd`, processes every received packet and runs the protocol timers. System calls run the transmit path in the caller's thread. One mutex, `net_lock`, protects all protocol state. `tcp.c` depends only on `net.h`, so it is also built and tested natively on the host against a simulated lossy link.

## Responsibilities and scope

What the subsystem does:

- Owns packet buffers (`netbuf`), the interface list, and the receive queue that drivers feed from interrupt context.
- Frames and parses Ethernet. Resolves next hops with ARP (a 32-entry cache that queues packets while an address resolves).
- Routes IPv4 between `lo`, eth0's subnet and one default gateway. Checks and builds IP headers and drops fragments.
- Answers ICMP echo requests and gives a copy of every ICMP packet to raw sockets (this is how `ping` works).
- UDP with demultiplexing by most specific match, and per-socket datagram queues.
- TCP: the full RFC 793/9293 state machine, 32 KiB send and receive rings, out-of-order reassembly, RFC 6298 RTO with Karn's rule, fast retransmit, slow start and congestion avoidance, zero-window probing, TIME_WAIT, listen backlogs, and orphaned connections that finish closing by themselves.
- The socket system calls (`socket`, `bind`, `listen`, `accept[4]`, `connect`, `sendto`, `recvfrom`, `shutdown`, `get/setsockopt`, `getsockname`, `getpeername`), plus `read`/`write`/`close`/`ppoll`/`ioctl` through the socket file operations.
- Observability: `/proc/net/*`, monitor commands, a per-packet log, trace events, and random packet loss injection (`net.drop=`).

What it does not do:

- No IPv6, IP options, fragmentation or reassembly, forwarding, multicast, or path-MTU discovery. It never generates ICMP errors (no port or host unreachable), and ignores ICMP errors it receives.
- No DNS. `resolve_host` in `user/lib/net.c` knows only `localhost`, `host`/`gateway` (10.0.2.2) and dotted quads.
- No AF_UNIX or AF_PACKET. Raw sockets are ICMP only.
- No TCP window scaling, SACK, timestamps, delayed ACKs, Nagle, keepalive or urgent data.

Boundaries with other subsystems:

| Neighbor | Boundary |
|---|---|
| virtio-net driver (`kernel/drivers/virtio_net.c`) | Calls `netif_rx()` from its interrupt handler and supplies `eth0.xmit = net_xmit`. Uses `nb_alloc`/`nb_free` for its buffers. |
| Files and VFS | A socket is a `struct file` whose `cops` is `sock_ops` and whose `priv` is the `struct socket`. `fd_install`, `fd_get` and `file_close` come from `kernel/fs/file.c`. |
| `poll.c` | Calls `sock_poll`, which registers on the socket's wait queue and returns the cached readiness mask. |
| Scheduler and sync | `netd` is a `kthread_create` thread. Waiting uses `prepare_to_wait`/`schedule_until`/`finish_wait` (netd) and `wait_event_interruptible` (sockets). |
| Signals | `sock_send` sends `SIGPIPE` via `signal_send`. Interrupted waits return `-ERESTARTSYS`. |
| procfs, monitor, trace, GDB | Call the `*_print` functions and read the trace events described below. |

## Source map

| File | Lines | Role |
|---|---:|---|
| `kernel/include/net.h` | 248 | All in-kernel network types and prototypes. The upper part (byte order, netbufs, checksums, TCP API) is also compiled on the host with `NET_HOST_TEST`; the lower part (interfaces, sockets) is kernel only. |
| `kernel/net/net.c` | 383 | `net_lock`, interface registry, loopback interface, the receive queue and `netd`, `ip=`/`net.drop=` boot options, packet log, `/proc/net/dev` and `/proc/net/stats` printers, and the hooks `tcp.c` needs (`net_now_ms`, `net_random`, `net_log`, `tcp_wake_owner`, trace hooks). |
| `kernel/net/netbuf.c` | 86 | Packet buffers: `nb_alloc`, `nb_free`, `nb_put`, `nb_push`, `nb_pull`, `nb_clone`, and the `nb_live` leak counter. |
| `kernel/net/cksum.c` | 42 | The Internet checksum (RFC 1071) and the TCP/UDP pseudo-header checksum. |
| `kernel/net/ether.c` | 275 | Ethernet framing, the ARP table, pending packet queues and the ARP retry timer. |
| `kernel/net/ipv4.c` | 186 | Routing (`ip_route`, `ip_source_for`, `ip_is_local`), `ip_output`, `ip_input`, ICMP echo. |
| `kernel/net/udp.c` | 141 | UDP input/output, `udp_lookup`, raw ICMP delivery, the global `all_sockets` list, `/proc/net/udp`. |
| `kernel/net/tcp.c` | 1202 | TCP: control blocks, output, input state machine, timers, `/proc/net/tcp` and TCP statistics. |
| `kernel/net/socket.c` | 824 | Socket objects, readiness and waiting, port allocation, every socket system call, socket file operations and the `ifconfig` ioctls. |
| `kernel/drivers/virtio_net.c` | 123 | The eth0 driver: 64 posted receive buffers, a 64-entry transmit queue, the default QEMU addresses. |
| `lib/include/uapi/net.h` | 96 | ABI shared with user space: Linux values for `AF_*`, `SOCK_*`, `IPPROTO_*`, `SO_*`, `MSG_*`, `POLL*`, `struct sockaddr_in`, and `struct netif_req` with `NET_IOC_GETIF`/`NET_IOC_SETIF`. |
| `user/lib/net.c` | 163 | libc wrappers for the socket calls, `poll` (on `ppoll`), `select` (on `poll`), `inet_aton`/`inet_addr`/`inet_ntoa`, `resolve_host`. |
| `tests/host/test_tcp.c` | 420 | Host-native TCP test over a simulated wire with loss, duplication, jitter and latency (ASan/UBSan). |
| `user/bin/utests.c` | (part) | Socket tests over loopback: `t_tcp_loopback_echo`, `t_tcp_errors`, `t_nonblocking_sockets`, `t_udp_loopback`. |
| `tools/run_tests.py` | (part) | Scenario stages `network` and `network, 5% loss` through QEMU's user network. |
| `user/bin/{ping,nc,ifconfig,netstat,httpd,rshd,echod}.c` | | User tools: ICMP echo over a raw socket, netcat, interface show/set, `/proc/net` viewer, HTTP/1.0 server (also serves `/proc`), remote shell, TCP+UDP echo server. |

## Concepts and data structures

### Byte order

Inside the stack, addresses and ports are always in host byte order (`u32` and `u16`). They are converted only when a header is built or parsed, with `ntoh16`/`ntoh32` and the aliases `hton16`/`hton32` (`net.h`). `struct sockaddr_in` at the system call boundary is in network order (`socket.c: get_addr`, `put_addr`). `struct netif_req` for `ifconfig` is in host order.

### Packet buffers (`struct netbuf`, `netbuf.c`)

Each packet is two `kmalloc` allocations: the `struct netbuf` descriptor and a data area of `NB_SIZE` = 2048 bytes. `nb_alloc` sets `data = head + NB_HEADROOM` (128) and `len = 0`.

| Field | Meaning |
|---|---|
| `node` | Links the buffer into exactly one list at a time: the netd receive queue, an ARP entry's pending list, or a socket's `rxq`. |
| `head` | Start of the 2048-byte allocation. |
| `data`, `len` | The packet currently starts at `data` and is `len` bytes long. |
| `dev` | Receive side: the interface it arrived on (set by `netif_rx`). |
| `src_ip`, `dst_ip`, `proto` | Receive side: filled by `ip_input` from the IP header. |
| `src_port` | Receive side, UDP: the sender's port (`udp_input`). |

Operations:

- `nb_put(nb, n)` appends `n` bytes at the tail and returns their start. It returns NULL if that would pass `head + NB_SIZE`.
- `nb_push(nb, n)` prepends a header: `data -= n`. It returns NULL if that would go below `head`.
- `nb_pull(nb, n)` strips a header: `data += n`. It is clamped to `len`.
- `nb_clone` copies the data at the same offset and copies all the metadata.
- `nb_live` counts buffers allocated and not freed. It is shown in `/proc/net/stats`, and the host test checks that it ends at zero.

Most callers do not check `nb_put`/`nb_push` for NULL. That is safe only because every packet the stack builds fits in the headroom and tailroom.

Transmit layout of a full TCP segment (each layer pushes its header in front):

```
head      +82        +94        +108       +128                          +1608        +2048
 |  unused | virtio 12 | Eth 14  |  IP 20   | TCP 20 | data up to 1460 |   tailroom   |
           ^net_xmit    ^xmit()   ^ip_output ^send_segment_win builds here
```

Receive layout: `virtio_net.c: post_rx` sets `data = head + 128 - 12` and posts `NB_SIZE - 116` = 1932 bytes to the device. That way, after `nb_pull(VNET_HDR_LEN)` the Ethernet header starts at `head + 128`. `eth_input` pulls 14 bytes and `ip_input` pulls the IP header length. `ip_input` also truncates `len` to the IP total length, which drops Ethernet padding.

### Interfaces (`struct netif`, `net.c`)

| Field | Meaning |
|---|---|
| `name` | `"lo"` or `"eth0"` |
| `mac`, `has_mac` | `has_mac` is true for Ethernet. It decides whether netd calls `eth_input` or `ip_input`, and whether the packet log parses an Ethernet header. |
| `up` | Routing skips interfaces that are down. `lo` is always up. |
| `addr`, `netmask`, `gateway` | Host order. `lo` is `127.0.0.1/8`. eth0 defaults to `10.0.2.15/24` with gateway `10.0.2.2` (`virtio_net_init`), and the boot option `ip=` overrides that. |
| `mtu` | 1500 for both. |
| `xmit` | Consumes the buffer. eth0: `net_xmit` (Ethernet frame). lo: `lo_xmit` (IP packet). |
| `stats` | `rx/tx_packets`, `rx/tx_bytes`, `rx_dropped`, `rx_errors`, `tx_dropped` |

`MAX_NETIFS` is 2. `net_init` registers `loopback_if` first (index 0), then `virtio_net_init` registers eth0 (index 1) if a device exists. `netif_register` ignores registrations beyond the limit.

**Loopback.** `lo_xmit` simply calls `netif_rx(dev, nb)`. Loopback traffic therefore goes through the netd receive queue like any received packet, and is subject to `net.drop=`. The sender never processes its own packet synchronously. Anything addressed to one of our own addresses (any of 127/8, or eth0's address) is routed to `lo` (`ip_route`).

### The receive queue and netd (`net.c`)

- `rxq` is a list of netbufs, protected by the spinlock `rxq_lock` ("net-rxq") and capped at `RXQ_MAX` = 512.
- `netd_wq` is the wait queue netd sleeps on.
- `netif_rx` (any context, including interrupts) first applies the injected loss. If the queue is full it drops the packet (`rx_dropped`, a `net_drop` trace event). Otherwise it appends the packet and calls `wake_up(&netd_wq)`.
- `net_kick()` is just `wake_up(&netd_wq)`. Socket calls use it after something may have armed a timer.

### ARP table (`ether.c`)

`arp_table[ARP_ENTRIES]` (32) of `struct arp_entry`:

| Field | Meaning |
|---|---|
| `ip`, `mac`, `dev` | The mapping and the interface it belongs to |
| `state` | `ARP_FREE`, `ARP_PENDING` (request outstanding) or `ARP_RESOLVED` |
| `updated` | When it was last resolved or refreshed (`net_now_ms`) |
| `next_try`, `tries` | Retry schedule while pending |
| `pending`, `npending` | Packets waiting for the answer, at most `ARP_MAX_PENDING` (8) |

Constants: `ARP_RETRY_MS` 1000, `ARP_TRIES` 3, `ARP_TTL_MS` 10 minutes.

### IP, ICMP and UDP state

- `ipv4.c` keeps only `next_id`, the IP identification counter.
- `udp.c` holds the global `all_sockets` list (every socket of every type, in creation order). UDP and raw demultiplexing scan it linearly.
- `struct net_stats net_stats` (`net.h`) holds the counters for IP, ICMP, UDP, ARP and injected drops.

### Sockets (`struct socket`, `socket.c`)

| Field | Meaning |
|---|---|
| `type`, `proto` | `SOCK_STREAM`/`SOCK_DGRAM`/`SOCK_RAW`, and `IPPROTO_TCP`/`UDP`/`ICMP` |
| `id` | Increasing number. It appears in the file path (`socket:[N] tcp`) and in `/proc/net/udp`. |
| `laddr`, `lport`, `raddr`, `rport` | Local and remote addresses (host order) |
| `bound`, `connected`, `listening` | Lifecycle flags. For stream sockets, `connected` is set only by a blocking `connect` that succeeds or by `accept`. Nothing reads it for stream sockets. |
| `shut_rd`, `shut_wr` | Set by `shutdown`. `shut_rd` makes reads return 0 and drops new datagrams. |
| `error` | Meant to hold a pending `SO_ERROR`. **Nothing ever writes it.** TCP errors live in `tcb->error`. |
| `tcb` | Stream sockets: the TCP control block (a listener or a connection) |
| `rxq`, `rxq_len` | Datagram and raw sockets: received netbufs, at most `UDP_QUEUE_MAX` (64) |
| `wq` | One wait queue for readers, writers, accept, connect and poll |
| `node` | Link in `all_sockets` |
| `rx_bytes`, `tx_bytes`, `owner_pid` | Accounting, shown in `/proc/net/udp` |

The allocation is actually a `struct socket_ext` (private to `socket.c`), which adds two fields:

- `events` (u64): incremented by every `sock_wake`. Blocking calls sleep until it changes.
- `ready`: the cached poll mask (`POLLIN`/`POLLOUT`/`POLLERR`/`POLLHUP`), recomputed by `refresh()`.

### TCP control block (`struct tcb`, `tcp.c`)

The struct is private to `tcp.c`. Other code sees only `struct tcb *` and the functions in `net.h`.

| Group | Fields | Notes |
|---|---|---|
| Identity | `node`, `id`, `state`, `laddr/lport/raddr/rport` | `node` is in the global `tcbs` list. `id` comes from `next_id`. |
| Ownership | `owner`, `error` | `owner` is the socket (or NULL: an orphan). `error` is a negative errno: `-ECONNREFUSED`, `-ECONNRESET`, `-ETIMEDOUT` or `-ECONNABORTED`. |
| Send sequence | `iss`, `snd_una`, `snd_nxt`, `snd_max`, `snd_wnd`, `snd_wl1`, `snd_wl2` | `snd_max` is the highest sequence number ever sent. `snd_nxt` is where the next transmission starts, and moves back on timeouts and window reopenings. |
| Send ring | `sndbuf`, `snd_head`, `snd_len` | `snd_len` bytes starting at sequence `snd_una`, at ring offset `snd_head` |
| FIN bookkeeping | `fin_queued`, `fin_sent`, `fin_was_acked` | The FIN sits logically after the last data byte. |
| Congestion | `mss`, `cwnd`, `ssthresh`, `dupacks` | `mss` is the peer's MSS, default `TCP_MSS_DEFAULT` 536, clamped to [64, `TCP_MSS_LOCAL` 1460]. |
| Receive | `irs`, `rcv_nxt`, `rcvbuf`, `rcv_head`, `rcv_len`, `fin_rcvd`, `last_adv_wnd` | |
| Reassembly | `ooo[OOO_RANGES]`, `nooo` | Up to 8 sorted, disjoint `[start, end)` ranges received beyond `rcv_nxt` |
| Timers | `rexmit_at`, `tw_at`, `fw2_at` | Absolute `net_now_ms` times. 0 means not armed. |
| RTT | `rto`, `srtt`, `rttvar`, `rtt_timing`, `rtt_seq`, `rtt_start`, `retries` | Milliseconds. `srtt == 0` means "no sample yet". |
| Listen | `children`, `child_node`, `parent`, `backlog`, `nchildren` | A listener's list of children that have not been accepted yet |
| Statistics | `retransmits`, `segs_in/out`, `bytes_in/out` | Per connection. Global counters are in `tcp_stats`. |

TCP constants (`tcp.c` and `net.h`):

| Constant | Value | Use |
|---|---:|---|
| `TCP_BUF_SIZE` | 32 KiB | Size of each send ring and each receive ring |
| `TCP_MSS_DEFAULT` / `TCP_MSS_LOCAL` | 536 / 1460 | MSS before the peer's option is seen / what we advertise and the upper clamp |
| `RTO_INITIAL_MS` / `RTO_MIN_MS` / `RTO_MAX_MS` | 1000 / 200 / 30000 | RFC 6298 parameters (minimum lowered from 1 s) |
| `MAX_RETRIES` / `MAX_SYN_RETRIES` | 10 / 5 | Timeouts before `ETIMEDOUT` |
| `TIME_WAIT_MS` | 2000 | 2×MSL, deliberately short |
| `FIN_WAIT_2_MS` | 30000 | Lifetime of an orphan waiting for the peer's FIN |
| `OOO_RANGES` | 8 | Out-of-order ranges per connection |
| listen backlog | clamped to 1..64 | `tcp_listen` |
| initial `cwnd` / `ssthresh` | 10×MSS / 65535 | `init_cwnd` (RFC 6928) |
| `cwnd` cap | 4×`TCP_BUF_SIZE` = 128 KiB | `ack_advance` |

### The send ring

```
 sequence:  snd_una            snd_nxt                    snd_una+snd_len   (+1 if FIN)
             |<--- sent, unacked --->|<--- queued, not sent --->|  [FIN]
 ring:      sndbuf[snd_head] ... wraps modulo TCP_BUF_SIZE
```

`tcp_send` copies at ring offset `snd_head + snd_len`. `send_segment_win` reads payload from offset `snd_head + (seq - snd_una)`, so any sequence number from `snd_una` onward can be (re)sent straight from the ring. `ack_advance` moves `snd_head` forward and shrinks `snd_len` by the amount of data acknowledged. The FIN does not take a ring byte.

### The receive ring and out-of-order data

```
 ring:  rcv_head
         |<-- rcv_len unread, in order -->|<-- hole -->|ooo range|<-- hole -->|ooo|  ...  |
 seq:                               rcv_nxt                               rcv_nxt + rcv_space
                                     (rcv_space = TCP_BUF_SIZE - rcv_len)
```

- The advertised window is `adv_window = min(TCP_BUF_SIZE - rcv_len, 65535)`. Out-of-order bytes are written into the window space but are not subtracted from it. When in-order data arrives, `rcv_nxt` and `rcv_len` grow together, so the right edge `rcv_nxt + rcv_space` never moves left. The window only grows when the application reads.
- `ooo_store` writes an out-of-order segment straight to its final ring position, `rcv_head + rcv_len + (seq - rcv_nxt)`, after clipping it to the window. It then records the range, merging it with overlapping or adjacent ones. If 8 disjoint ranges already exist, the data is not stored ("too fragmented: let it be resent").
- `ooo_merge`, called after in-order data advances `rcv_nxt`, absorbs every range whose start is at or before `rcv_nxt`. The bytes are already in place, so this only advances `rcv_nxt`/`rcv_len`.
- `last_adv_wnd` is the window value sent in the last header (see "duplicate-ACK window" below).

## How it works

### Bring-up (`net.c: net_init`)

Called from `kinit` after the filesystems are mounted:

1. `netif_register(&loopback_if)`.
2. `virtio_net_init()`:
   - finds the device;
   - sets up queue 0 (receive, 64 entries) and queue 1 (transmit, 64);
   - reads the MAC from config space, or uses `02:00:00:00:00:01`;
   - sets the QEMU defaults (10.0.2.15/24, gateway 10.0.2.2);
   - posts 64 receive buffers;
   - registers the interrupt and registers eth0.

   Without a device it logs "no virtio network device" and the system runs with `lo` only.
3. `parse_ip_option()` applies `ip=<a.b.c.d>/<bits>[,<gateway>]` to eth0. There is no validation: missing parts parse as 0, and the prefix length defaults to 24.
4. `net.drop=<percent>` sets `net_drop_percent` and logs a warning.
5. Starts and detaches the `netd` thread. It panics if the thread cannot be created.
6. Logs one line per interface: `name: addr/netmask [gateway G] [(down)]`.

### Receive path

```
virtio irq (cpu0)                      netd thread (net_lock held)
net_irq ─► virtq_get ─► nb_pull(12)    rxq_pop (≤64 per round)
        ─► netif_rx ──► [drop?] ─► rxq ─► stats, net_pkt_log("rx")
        ─► post_rx (refill)                ├─ has_mac: eth_input ─► arp_input | ip_input
        ─► free finished tx buffers        └─ lo:      ip_input
                                           ip_input ─► icmp_input (+ raw_input copy)
                                                     ─► tcp_input
                                                     ─► udp_input
                                           then tcp_timer(), arp_timer()
```

**`netd` loop** (`net.c: netd`):

1. Take `net_lock`. Compute `timers = tcp_timers_pending() || arp_timers_pending()`. Release the lock.
2. `prepare_to_wait(&netd_wq)` marks the thread sleeping first. Then, if `rxq` is empty, `schedule_until(timers ? now + 10 ms : 0)` (0 means no timeout). `finish_wait`.
3. Take `net_lock`. Pop and process up to 64 packets (`budget`): count `rx_packets`/`rx_bytes`, log the packet, and dispatch to `eth_input` or `ip_input`. Then run `tcp_timer()` and `arp_timer()`. Release the lock.

Because the thread is marked sleeping before it checks `rxq`, a packet queued between the check and the sleep still wakes it. While any timer is pending, netd wakes every 10 ms, so that is the effective resolution of all TCP and ARP timers. When nothing is pending it sleeps until a packet or a `net_kick`.

**`eth_input`**: drops frames shorter than 14 bytes (`rx_errors`) and frames not addressed to our MAC or broadcast. It strips the header, then passes ARP to `arp_input` (the buffer is then freed) and IPv4 to `ip_input`. Other EtherTypes are dropped silently.

**`ip_input`** (`ipv4.c`):

1. Validates version 4, `hl >= 20`, `hl <= len`, `hl <= total length <= len`, and the header checksum. Failures count as `ip_bad` and `rx_errors`.
2. Drops fragments: MF set or a nonzero offset (`frag & 0x3fff`), counted as `ip_frags`.
3. Accepts the packet if the destination is local (`ip_is_local`), `255.255.255.255`, or the receiving interface's subnet broadcast. Otherwise it counts `ip_not_ours`.
4. Fills `src_ip`, `dst_ip` and `proto`, truncates `len` to the total length, and pulls the header.
5. Dispatches by protocol: 1 → a clone to `raw_input` plus the original to `icmp_input`; 6 → `tcp_input`; 17 → `udp_input`; anything else is freed.

### Transmit path

```
socket call (net_lock held) ─► tcp_send/output ─► send_segment_win ─┐
                            ─► udp_output ──────────────────────────┤
                            ─► raw: ip_output directly ─────────────┤
netd (net_lock) ─► tcp_input/tcp_timer ─► send_segment_win ─────────┤
                ─► icmp_input (echo reply, in place) ───────────────┤
                                                                    ▼
                         ip_output(src, dst, proto, nb)   [consumes nb]
                           ├─ ip_route() ─ none: ip_no_route, -ENETUNREACH
                           ├─ len + 20 > mtu: -EMSGSIZE
                           ├─ push IP header (DF, TTL 64, id++, checksum)
                           ├─ lo:   net_pkt_log("tx"), lo_xmit ─► netif_rx
                           └─ eth0: eth_output(dev, nb, next_hop, ETH_P_IP)
                                     ├─ broadcast ─► xmit(bcast)
                                     ├─ fresh ARP entry ─► xmit(mac)
                                     └─ else queue on ARP entry, send request
                                   xmit(): push Ethernet header, log, stats, dev->xmit
                                   net_xmit: push 12-byte virtio header, virtq_add, kick
```

**Routing** (`ipv4.c: ip_route`):

1. A local destination goes to `lo` (`next_hop = dst`).
2. Otherwise the first interface (not `lo`) that is up and has an address, and whose subnet contains `dst` (or `dst` is `255.255.255.255`), is used directly.
3. Otherwise the first interface (not `lo`) that is up and has a gateway is used, with `next_hop = gateway`.
4. Otherwise NULL.

`ip_source_for(dst)` picks the source address. Through `lo` it is `127.0.0.1` for 127/8 destinations, or the destination itself (talking to our own eth0 address). Otherwise it is the outgoing interface's address. TCP uses it to choose a connection's local address at `connect` time, and UDP when the socket is not bound to an address.

**`net_xmit`** (`virtio_net.c`) adds one descriptor to the transmit queue. If the ring is full, it reclaims finished frames itself and tries once more. If that fails too it counts `tx_queue_full` and returns -1, which `xmit()` counts as `tx_dropped`. Finished frames are normally freed by the interrupt handler.

### ARP (`ether.c`)

**`eth_output(dev, nb, next_hop, type)`:**

- **Broadcast** (`255.255.255.255` or the subnet broadcast): sent to `ff:ff:ff:ff:ff:ff`.
- **Resolved and younger than `ARP_TTL_MS`:** sent at once.
- **Unknown or stale:** `arp_new` takes a free slot, or evicts the least recently updated *resolved* entry. Pending entries are never evicted, so if all 32 slots are pending the packet is dropped with `-ENOBUFS`. The entry becomes `ARP_PENDING` with `tries = 1`, and a request is broadcast. A stale entry is re-requested the same way, and packets wait even though an older MAC is known.
- **Queuing:** the packet waits on the entry. If 8 are already waiting, the oldest is dropped (`arp_unresolved`).

**`arp_input`:** checks the packet is Ethernet/IPv4 with 6- and 4-byte addresses.

- If an entry for the sender's IP exists in any state, it is refreshed with the sender's MAC (`arp_resolved`), which also flushes the waiting packets. So any ARP packet from a known host refreshes it, including gratuitous ARP.
- If no entry exists, one is created only when the packet is for our address.
- A request for our address is answered with a unicast reply.

**`arp_timer`** (from netd): for each pending entry whose `next_try` has passed, it either sends another request (`tries++`, next try in 1 s) or, after 3 tries, drops the waiting packets (`arp_unresolved`, a `pr_debug` line "arp: no answer for …") and frees the entry. `arp_timers_pending` is true while any entry is pending.

### ICMP and raw sockets

`icmp_input` checks the length (at least 8) and the checksum. Echo requests (type 8, code 0) are turned into replies in place: type 0, new checksum. The reply is sent with `ip_output(from, src_ip, 1, nb)`, where `from` is our destination address, or 0 (pick automatically) if the request was broadcast. Every other ICMP type is dropped after the raw sockets have seen it.

`raw_input` gets its own clone of every valid ICMP packet from `ip_input`. It gives a further clone to each `SOCK_RAW` socket whose `proto` matches, filtered by `raddr` if the socket is connected, and then frees its copy. The queued buffer starts at the ICMP header, and `src_ip` is reported by `recvfrom`. Raw sends (`sock_send`) put the user's bytes (an ICMP header the user built, checksum included) after a kernel-built IP header, limited to 1480 bytes.

`ping` (`user/bin/ping.c`) builds echo requests with `id = getpid()` and a send timestamp, and ignores received packets that are not type-0 replies with its id. It therefore also sees its own requests when pinging `localhost`, and skips them.

### UDP (`udp.c`)

**Input:**

1. Checks `len >= 8` and `8 <= udp.len <= nb->len`, and verifies the checksum unless the field is 0. Failures count as `udp_bad_cksum`.
2. Trims the buffer to `udp.len` and records `src_port`.
3. `udp_lookup(dst_ip, dport, src_ip, sport)` scans `all_sockets` for bound `SOCK_DGRAM` sockets on that port, skipping:
   - sockets bound to another address;
   - connected sockets whose peer does not match.

   Among the rest it picks the highest score: connected +2, specific local address +1. On a tie the first socket in creation order wins.
4. No match: `udp_no_port`. No ICMP port-unreachable is sent.
5. Pulls the header and enqueues the datagram. If the queue already holds 64 datagrams or the socket is `shut_rd`, it is dropped as `udp_queue_full`. Otherwise `sock_wake` is called.

**Output** (`udp_output`):

- A payload over 1472 (`ETH_MTU - 20 - 8`) fails with `-EMSGSIZE`.
- The source is `s->laddr`, or `ip_source_for(dst)`. If that is 0, the call fails with `-ENETUNREACH`.
- The checksum is always computed, and a computed 0 is sent as `0xffff`.
- Returns the payload length, or the error from `ip_output`.

### TCP

#### Entry points (all called with `net_lock` held)

| Function | Called by | Effect |
|---|---|---|
| `tcp_connect` | `do_connect` | New tcb in `SYN_SENT`. Sends a SYN with the MSS option (1460), sets `snd_nxt = snd_max = iss+1`, arms the RTO. |
| `tcp_listen` | `do_listen` | New tcb in `LISTEN`, backlog clamped to 1..64 |
| `tcp_accept` | `do_accept4` | Unlinks and returns the first child that is not in `SYN_RCVD` (so `ESTABLISHED`, or `CLOSE_WAIT` if the peer already closed), or NULL |
| `tcp_set_owner` | `do_accept4` | Gives the accepted child its socket |
| `tcp_send` | `sock_send` | Copies into the send ring, then `output()` |
| `tcp_recv` | `sock_recv` | Copies from the receive ring, may send a window update |
| `tcp_shutdown` | `do_shutdown`, `tcp_close` | Queues the FIN |
| `tcp_close` | `sock_free` | Owner lets go (see "Ownership") |
| `tcp_poll` | `compute_ready` | Readiness bits |
| `tcp_input` | `ip_input` | Segment processing |
| `tcp_timer` | netd | Timers |

#### Output (`output`, `send_segment_win`)

`output(t)` runs only in `ESTABLISHED`, `CLOSE_WAIT`, `FIN_WAIT_1`, `CLOSING` and `LAST_ACK`. It loops until it cannot send:

1. If `fin_sent`, stop.
2. Compute:
   - `sent = snd_nxt - snd_una`;
   - `avail = snd_len - sent`;
   - `wnd = min(snd_wnd, cwnd)`;
   - `usable = snd_una + wnd - snd_nxt` (signed).
3. If `avail > 0` and `usable > 0`: send `n = min(avail, usable, mss)` bytes. PSH is set when `n == avail`. Start RTT timing if no measurement is in progress (`rtt_seq = snd_nxt`). Advance `snd_nxt` with `set_nxt` (which also raises `snd_max`), arm the RTO, and continue.
4. If everything is sent, a FIN is queued, and `usable >= 0`: send FIN|ACK at `snd_nxt`, advance by 1, set `fin_sent`, arm the RTO. The FIN is allowed with a usable window of exactly 0.
5. If data remains and `snd_wnd == 0`, arm the RTO, which acts as the persist timer.

There is no Nagle algorithm and no sender-side silly-window avoidance: any usable window, however small, is filled.

`send_segment_win(t, seq, flags, len, same_window)` builds a header:

- The ack field is `rcv_nxt` if ACK is set.
- A SYN gets the 4-byte MSS option.
- The window is `last_adv_wnd`, which is first updated to the current `adv_window` unless `same_window` is set and the current window is not smaller.
- The payload comes from the ring.
- It computes the pseudo-header checksum, updates the counters, and calls `ip_output`. Errors from `ip_output` are ignored. Only an `nb_alloc` failure (`-ENOBUFS`) is reported, and it stops `output()`.

ACK helpers:

- `send_ack(t)` is a pure ACK **at `snd_max`**.
- `send_dupack(t)` is the same but with `same_window = true`.
- `send_rst_reply(...)` answers a segment that matches no connection, following RFC 9293 3.10.7.1: if the segment had an ACK, `seq = its ack` with RST; otherwise `ack = seq + seglen` with RST|ACK.

#### Input (`tcp_input`)

1. **Validate.** Drop if the segment is shorter than 20 bytes, the pseudo-header checksum fails, or the data offset is outside `[20, len]`. Bad offsets are also counted as `bad_cksum`.
2. **Demultiplex.** `find_tcb(dst, dport, src, sport)` returns the first non-listening tcb, not in `CLOSED`, that matches the full 4-tuple. Failing that, it returns a listener on that port bound to the wildcard or to `dst`; if there are several, the one latest in the list wins. With no match: `no_listener++`, and a reset reply unless the segment is itself a RST.
3. **`LISTEN`** (`listen_input`):
   - RST: ignored.
   - ACK: answered with a RST.
   - Not a SYN: ignored.
   - `nchildren >= backlog`: dropped silently (`backlog_full`); the peer's SYN retransmission retries.
   - Otherwise a child tcb is created: linked to the parent, `irs`, `rcv_nxt = irs+1`, the peer's window and MSS, `init_cwnd`. It goes to `SYN_RCVD`, sends SYN|ACK and arms the RTO.

   Any data or FIN on the SYN is ignored.
4. **`SYN_SENT`** (`syn_sent_input`):
   - An ACK other than `iss+1`: RST reply (unless the segment is itself a RST) and drop.
   - RST with an acceptable ACK: `abort_conn(-ECONNREFUSED)`. Any other RST is ignored.
   - No SYN: ignored.
   - SYN: record `irs`, `rcv_nxt`, `snd_wnd`, `snd_wl1/wl2`, the MSS option, `init_cwnd`. With an acceptable ACK: `snd_una = ack`, stop the timer, go to `ESTABLISHED`, send an ACK, wake the owner. Without an ACK (simultaneous open): go to `SYN_RCVD` and send SYN|ACK.
5. **Acceptability** for every other state. `wnd = rcv_space`.
   - A zero-length segment is acceptable if `rcv_nxt <= seq <= rcv_nxt + wnd`. The right edge is inclusive; see bug history 1.
   - A segment with length is acceptable if `wnd > 0` and its first or last sequence number falls in `[rcv_nxt, rcv_nxt + wnd)`.
   - Special case: in `SYN_RCVD`, a retransmitted SYN (`seq == irs`) means our SYN|ACK was lost, so resend it.
   - Otherwise an unacceptable segment is logged and dropped, and answered with an ACK **only if it occupies sequence space** (`seglen > 0`) and is not a RST; see bug history 3. This also re-acknowledges retransmitted FINs and old duplicate data.
6. **RST** (acceptable anywhere in the window):
   - An embryonic child (`parent != NULL`) is simply freed.
   - Otherwise `abort_conn` with `-ECONNREFUSED` (from `SYN_RCVD`) or `-ECONNRESET`, then `maybe_free`.
7. **SYN in a synchronized state:** `abort_conn(-ECONNRESET)` with a RST sent, then free the tcb if it is a child or an orphan.
8. **ACK.** Segments without ACK stop here.
   - In `SYN_RCVD`, an ACK in `(snd_una, snd_nxt]` completes the handshake: record the window, go to `ESTABLISHED`, and wake. For a child that means the listener's socket, so `accept` can proceed. Any other ACK gets a RST reply.
   - `ack > snd_max`: send an ACK and drop.
   - `ack > snd_una`: `ack_advance` (below).
   - Otherwise, if `ack == snd_una`, data is outstanding (`snd_max != snd_una`), the segment has no payload and no FIN, and **its window equals `snd_wnd`**: this is a duplicate ACK. On the third, fast retransmit:
     - `ssthresh = max(flight/2, 2*mss)` and `cwnd = ssthresh`;
     - stop RTT timing (Karn);
     - resend `min(mss, snd_len)` bytes at `snd_una`.

     There is no fast recovery (no window inflation), and only the third duplicate triggers it.
   - **Window update** (RFC 9293 3.10.7.4): if `snd_wl1 < seq`, or `snd_wl1 == seq && snd_wl2 <= ack`, take the new window. If the window was 0 and is now open, stop probing, rewind `snd_nxt` to `snd_una` and clear `fin_sent`, so `output()` resends from the start of the window.
   - **FIN acknowledged** (`fin_was_acked`):
     - `FIN_WAIT_1` → `FIN_WAIT_2` (orphans get `fw2_at = now + 30 s`);
     - `CLOSING` → `TIME_WAIT`;
     - `LAST_ACK` → `CLOSED`, wake, `maybe_free`, and stop.
9. **Data and FIN** (`receive_data`).
10. If the tcb is not `CLOSED`, call `output()`: the ACK may have opened the window.

`ack_advance(t, ack)`:

- `acked = ack - snd_una`. If `acked > snd_len` and a FIN is queued, the FIN is acknowledged too (`fin_was_acked`).
- If timing and `ack > rtt_seq`, take an RTT sample.
- Release the acknowledged bytes from the ring and set `snd_una = ack`. If `snd_nxt` is now behind `snd_una` (the peer acknowledged data sent before a rewind), move it up.
- Grow `cwnd`: by `min(acked, mss)` in slow start (`cwnd < ssthresh`), else by `mss*mss/cwnd`. Cap it at 128 KiB.
- Reset `retries` and `dupacks`, and stop the timer. Re-arm it if anything is still outstanding (`snd_una != snd_max`).
- Wake the owner: there is room in the send buffer.

`receive_data(t, h, payload, len)`:

- Data is taken only in `ESTABLISHED`, `FIN_WAIT_1` and `FIN_WAIT_2`. In other states the data, and any FIN with it, is ignored, though the segment is still ACKed because `len > 0`.
- The already-received front is trimmed.
- If a gap remains before the data: `out_of_order++`, `ooo_store`, `send_dupack`, and return. A FIN on such a segment is lost; the peer resends it.
- In-order data: copy up to `rcv_space` bytes into the ring, advance `rcv_nxt`/`rcv_len`, `ooo_merge`, and wake. If the data did not all fit, the FIN is ignored too.
- A FIN is taken when `seq + len == rcv_nxt` and none was received before: `rcv_nxt++`, `fin_rcvd`, then:
  - `ESTABLISHED`/`SYN_RCVD` → `CLOSE_WAIT`;
  - `FIN_WAIT_1` → `TIME_WAIT` if our FIN was already acknowledged, else `CLOSING`;
  - `FIN_WAIT_2` → `TIME_WAIT`.

  Then wake.
- Finally, send an ACK only if the segment carried data or a FIN; see bug history 5. Every data segment is acknowledged immediately (no delayed ACKs).

`tcp_recv(t, buf, n, peek)`:

- Returns up to `n` buffered bytes. Without `peek`, it consumes them. If the advertised window had dropped below 8 KiB and at least 16 KiB is now free, it sends a window-update ACK right away (in `ESTABLISHED`, `FIN_WAIT_1` and `FIN_WAIT_2`). This avoids a stall where the peer waits for a window it was never told about.
- With nothing buffered it returns, in this order of priority: 0 if a FIN was received, `t->error` if set, 0 in `CLOSED`, otherwise `-EAGAIN`.

`tcp_send(t, buf, n)` fails in these cases:

| Condition | Result |
|---|---|
| `t->error` set | `-ECONNREFUSED` if the error is `ECONNREFUSED`, else `-EPIPE` |
| `SYN_SENT`/`SYN_RCVD` | `-EAGAIN` |
| FIN queued, or a state other than `ESTABLISHED`/`CLOSE_WAIT` | `-EPIPE` |
| Send ring full | `-EAGAIN` |

Otherwise it copies `min(n, room)` bytes and calls `output()`.

#### State transitions

Every transition in the code, with where it happens:

| From | To | Trigger | Where |
|---|---|---|---|
| (new) | `SYN_SENT` | active open | `tcp_connect` |
| (new) | `LISTEN` | passive open | `tcp_listen` |
| (new child) | `SYN_RCVD` | SYN to a listener | `listen_input` |
| `SYN_SENT` | `ESTABLISHED` | SYN+ACK with ack = iss+1 | `syn_sent_input` |
| `SYN_SENT` | `SYN_RCVD` | bare SYN (simultaneous open) | `syn_sent_input` |
| `SYN_SENT` | `CLOSED` | RST with an acceptable ACK (`ECONNREFUSED`) | `syn_sent_input` → `abort_conn` |
| `SYN_SENT` | `CLOSED` | more than 5 SYN retries (`ETIMEDOUT`) | `retransmit_timeout` |
| `SYN_SENT` | `CLOSED` | owner closes | `tcp_close` |
| `SYN_RCVD` | `ESTABLISHED` | ACK in `(snd_una, snd_nxt]` | `tcp_input` step 8 |
| `SYN_RCVD` | `CLOSED` | RST in window (child freed; owned: `ECONNREFUSED`) | `tcp_input` step 6 |
| `SYN_RCVD` | `CLOSED` | more than 5 SYN-ACK retries (child freed) | `retransmit_timeout` |
| `SYN_RCVD` | `FIN_WAIT_1` | owner shuts down (simultaneous-open case only) | `tcp_shutdown` |
| `ESTABLISHED` | `FIN_WAIT_1` | `shutdown(SHUT_WR)` or close | `tcp_shutdown` |
| `ESTABLISHED` | `CLOSE_WAIT` | in-order FIN | `receive_data` |
| `FIN_WAIT_1` | `FIN_WAIT_2` | our FIN acknowledged | `tcp_input` step 8 |
| `FIN_WAIT_1` | `CLOSING` | peer's FIN before ours is acknowledged | `receive_data` |
| `FIN_WAIT_1` | `TIME_WAIT` | peer's FIN with our FIN already acknowledged (in practice step 8 has already moved to `FIN_WAIT_2`) | `receive_data` |
| `FIN_WAIT_2` | `TIME_WAIT` | peer's FIN | `receive_data` |
| `FIN_WAIT_2` | `CLOSED` | orphan, 30 s without a FIN (`ETIMEDOUT`, no RST) | `tcp_timer` |
| `CLOSING` | `TIME_WAIT` | our FIN acknowledged | `tcp_input` step 8 |
| `CLOSE_WAIT` | `LAST_ACK` | `shutdown(SHUT_WR)` or close | `tcp_shutdown` |
| `LAST_ACK` | `CLOSED` | our FIN acknowledged | `tcp_input` step 8 |
| `TIME_WAIT` | `CLOSED` | `TIME_WAIT_MS` (2 s) passed | `tcp_timer` |
| `LISTEN` | `CLOSED` | owner closes; every child is reset (`ECONNABORTED`) and freed | `tcp_close` |
| any synchronized | `CLOSED` | in-window RST (`ECONNRESET`) | `tcp_input` step 6 |
| any synchronized | `CLOSED` | in-window SYN (`ECONNRESET`, RST sent) | `tcp_input` step 7 |
| any synchronized | `CLOSED` | more than 10 data/FIN retransmissions (`ETIMEDOUT`, RST sent) | `retransmit_timeout` |
| any synchronized | `CLOSED` | owner closes with unread data (`ECONNRESET`, RST sent, RFC 2525) | `tcp_close` |

`abort_conn(t, err, send_rst)` sends RST|ACK at `snd_nxt`, but only when `send_rst` is set and the state is not `CLOSED`, `LISTEN`, `SYN_SENT` or `TIME_WAIT`. It then records `err`, stops the retransmission timer, moves to `CLOSED`, and wakes the owner. Every change goes through `set_state`, which logs it with `net_log`, emits the `tcp_state` trace event, and sets `tw_at` when entering `TIME_WAIT`.

#### Timers (`tcp_timer`, `retransmit_timeout`)

`tcp_timer` runs on every netd round, which is about every 10 ms while anything is pending. It walks all tcbs:

1. `TIME_WAIT` past `tw_at`: `CLOSED`, wake, `maybe_free`.
2. An orphan in `FIN_WAIT_2` past `fw2_at`: `abort_conn(-ETIMEDOUT)` without a RST, then `maybe_free`.
3. `rexmit_at` passed: `retransmit_timeout`.

`retransmit_timeout(t, now)`:

- **Handshake states:** after more than 5 retries, `abort_conn(-ETIMEDOUT)` and free (children directly). Otherwise double the RTO (up to 30 s), resend SYN (or SYN|ACK) and re-arm.
- **Persist** (`snd_wnd == 0` and data queued): send 1 byte at `snd_una`, set `snd_nxt = snd_una + 1`, double the RTO, and re-arm. **No retry count**: a slow reader is not a failure.
- **Nothing outstanding** (`snd_una == snd_max`): return.
- **Otherwise**, after more than 10 retries, `abort_conn(-ETIMEDOUT, RST)`. Otherwise:
  - `ssthresh = max(flight/2, 2*mss)`, `cwnd = mss`;
  - double the RTO;
  - stop RTT timing (Karn);
  - clear `dupacks`;
  - **go back N**: `snd_nxt = snd_una`, `fin_sent = false`, `output()`.

  If `output()` sent nothing because the window closed meanwhile, it sends a 1-byte probe. Then it re-arms the timer from the new RTO.

`tcp_timers_pending()` is true if any tcb has `rexmit_at` set, is in `TIME_WAIT`, or has `fw2_at` set. netd uses it to choose between a 10 ms sleep and an indefinite one.

#### RTT and RTO (RFC 6298)

`rtt_sample(t, r)`:

- First sample: `srtt = r` (at least 1), `rttvar = r/2`.
- After that: `rttvar = (3*rttvar + |srtt - r|)/4`, then `srtt = (7*srtt + r)/8`.
- `RTO = srtt + max(4*rttvar, 10)`. The 10 ms term stands for the clock granularity G (netd's tick). The result is clamped to [200, 30000] ms.

All values are integer milliseconds.

Only one segment is timed at a time: `rtt_timing`/`rtt_seq`/`rtt_start` are set when `output()` sends with no measurement running, and the sample is taken when an ACK passes `rtt_seq`. Karn's rule is implemented by clearing `rtt_timing` on a timeout and on a fast retransmit. However, see Limitations: `output()` restarts timing during the go-back-N resend.

#### Congestion control

The congestion control is Reno without fast recovery:

| Event | Effect |
|---|---|
| Connection established | `cwnd = 10*mss`, `ssthresh = 65535` |
| New ACK | Slow start: `+min(acked, mss)` per ACK. Congestion avoidance: `+mss²/cwnd` per ACK. Capped at 128 KiB. |
| Third duplicate ACK | `ssthresh = max(flight/2, 2*mss)`, `cwnd = ssthresh`, resend one segment |
| Timeout | `ssthresh = max(flight/2, 2*mss)`, `cwnd = mss`, go back N |

The effective window is `min(snd_wnd, cwnd)`. Without window scaling the peer's window is at most 65535 bytes, and when the peer is this stack it is at most 32 KiB.

#### snd_max versus snd_nxt

A timeout (or a reopened window) rewinds `snd_nxt` to `snd_una`, but data up to `snd_max` may already be at the peer. The code therefore uses `snd_max` wherever "the highest sequence number we have used" matters:

- pure ACKs are sent with `seq = snd_max` (`send_ack`, `send_dupack`);
- an ACK above `snd_max` is "acks something we never sent";
- duplicate-ACK detection and the "anything outstanding" checks compare against `snd_max`;
- `ack_advance` re-arms the timer while `snd_una != snd_max`.

`snd_nxt` is used to choose what to send next, and to validate the handshake ACK in `SYN_RCVD`.

#### Ownership, orphans and freeing

- A tcb with an `owner` (its socket) is **never freed by `tcp.c`**. The owner sees `CLOSED` and `error`, and the tcb is freed when the owner calls `tcp_close`.
- `tcp_close` clears `owner`, then:

  | State | Action |
  |---|---|
  | `LISTEN` | Reset and free every child, go to `CLOSED` |
  | Unread data (and not `CLOSED`) | Abort with a RST |
  | `SYN_SENT` | Go to `CLOSED` |
  | Anything else | `tcp_shutdown` (send the FIN), and arm the 30 s `FIN_WAIT_2` timer if already there |

  It ends with `maybe_free`, which frees the tcb if it is `CLOSED`.
- An **orphan** (no owner) finishes on its own: it sends remaining data and the FIN, retransmits, waits in `TIME_WAIT`, and is freed by `maybe_free` when it reaches `CLOSED`, or at the `FIN_WAIT_2` timeout or the retry limit. Orphans still accept data into their receive ring in `FIN_WAIT_1`/`FIN_WAIT_2`, where nobody reads it.
- **Children of a listener** have no owner but have a `parent`. `wake()` on a child wakes the parent's owner, which is what lets `accept` see newly established connections. Embryonic children are freed directly by `tcb_free` on RST, an in-window SYN, or a handshake timeout. `tcb_free` unlinks them from the parent and decrements `nchildren`.
- `tcp_port_in_use` ignores `TIME_WAIT` and `CLOSED` tcbs, so a port can be rebound while an old connection is in `TIME_WAIT`. This is the same as having `SO_REUSEADDR` always on, which is why `setsockopt(SO_REUSEADDR)` is a no-op.

#### Bug history recorded in the code

Each of these was found with `tests/host/test_tcp.c` or the SMP stress test, fixed, and is now described in a comment:

1. **Window edge** (`tcp_input`, step 1). With a full window in flight in both directions, a pure ACK's `seq` is the sender's `snd_nxt`, which equals `rcv_nxt + wnd` at the receiver. RFC 793's strict `<` rejects it, so neither side ever saw the other's ACKs. Zero-length segments now accept `seq <= rcv_nxt + wnd`, as Linux's `tcp_sequence()` does.
2. **`snd_max`** (`send_ack`). After a timeout rewinds `snd_nxt`, the peer may already hold data beyond it, and would discard an ACK carrying an "old" sequence number. Pure ACKs now carry `snd_max`. Found by the randomized host test.
3. **ACK ping-pong** (`tcp_input`, out-of-window branch). Answering every unacceptable segment with an ACK let two ends bounce bare ACKs back and forth. The reply is now sent only when the incoming segment occupies sequence space (`seglen > 0`) and is not a RST.
4. **Duplicate-ACK window** (`send_dupack`). RFC 5681 counts an ACK as a duplicate only if the window is unchanged, and the receiver's window moves whenever the application reads. Without `same_window`, a reading receiver never triggered fast retransmit, and every loss cost a full RTO. Dup ACKs now repeat `last_adv_wnd` (or a smaller current window), and the sender checks `win == snd_wnd`.
5. **TIME_WAIT ACK storm** (`receive_data`, final comment). When both ends closed at once, both sat in `TIME_WAIT` and answered each other's ACKs with ACKs until the timers expired. Now only data and FINs are acknowledged. A retransmitted FIN still gets its ACK through the out-of-window branch. `simultaneous_close` in the host test checks that the exchange takes at most 8 segments.

### Sockets

#### Readiness and waiting (`socket.c`)

All socket and protocol state is read and written with `net_lock` held. Blocking works without ever reading state unlocked:

- `sock_wake(s)` (always called with `net_lock` held, from the protocols through `tcp_wake_owner`, from `enqueue`, and from `do_shutdown`):
  1. `refresh(s)`: `ready = compute_ready(s)`;
  2. `events++`;
  3. `wake_up(&s->wq)`.
- `sock_wait(s)` remembers `events`, drops `net_lock`, sets `wait_what = "socket"` (shown by `ps`), and runs `wait_event_interruptible(&s->wq, events != seen)`. It then retakes `net_lock` and returns 0 or `-ERESTARTSYS`. Every change bumps `events` under the lock, so a change between the unlock and the sleep is not missed. Callers loop and re-check the real protocol state.
- `sock_poll` registers on `s->wq` and returns the cached `ready` without taking any lock. Because `refresh` happens before `wake_up`, a poller either sees the new mask or is woken. Every system call also refreshes the mask on exit, which covers changes that only clear bits (such as a full send buffer).

`compute_ready`:

| Socket | Mask |
|---|---|
| Stream without a tcb | `POLLOUT|POLLHUP` |
| Stream with a tcb | `tcp_poll` mapped to `POLLIN/OUT/ERR/HUP` |
| Datagram and raw | `POLLIN` if `rxq_len`, always `POLLOUT`, `POLLERR` if `s->error` (never set) |
| Any, if `shut_rd` | adds `POLLIN` |

`tcp_poll`:

- A listener reports `IN` if any child is not in `SYN_RCVD`.
- `IN` for buffered data, a FIN, an error, or `CLOSED`.
- `OUT` in `ESTABLISHED`/`CLOSE_WAIT` with no FIN queued and `snd_len < 32 KiB`.
- `ERR` if `error` is set.
- `HUP` in `CLOSED`, or once FINs have gone both ways.

#### Ports

`bind_locked`:

- Rejects a second bind (`EINVAL`).
- Rejects non-local addresses other than `255.255.255.255` (`EADDRNOTAVAIL`).
- Ports below 1024 need `euid == 0` (`EACCES`).
- Port 0 takes an ephemeral port. Raw sockets keep port 0.

`ephemeral_port` walks a single counter shared by TCP and UDP from 49152 to 65535 and wraps around, skipping ports in use (up to 16384 tries). `sock_port_in_use` checks, for the same type:

- for TCP, `tcp_port_in_use` (tcbs not in `TIME_WAIT`/`CLOSED`);
- bound sockets with the same port and an overlapping address (wildcard matches everything). For stream sockets only those without a tcb count here; those with a tcb are covered by the first check.

#### Data transfer

`sock_send(s, f, buf, n, user, flags, has_dst, dst, dport)`:

- Allocates a 4 KiB bounce buffer (`CHUNK`) and takes `net_lock`.
- **Stream:**
  - Fails with `ENOTCONN` without a tcb or on a listener.
  - Loops over 4 KiB chunks: `copy_from_user` (while holding the lock), then `tcp_send`.
  - On `-EAGAIN` a blocking call waits for room and continues, so it returns only when all the data is queued, as on Linux; a signal ends the wait and returns what was queued so far (or restarts the call if nothing was). A non-blocking call returns what fit, or `EAGAIN`.
  - On `-EPIPE` with nothing sent, it raises `SIGPIPE` unless `MSG_NOSIGNAL` (0x4000) is set.
  - Ends with `net_kick()`.
  - A destination address passed to `sendto` is ignored.
- **Datagram and raw:**
  - Use the connected peer when no destination is given (`EDESTADDRREQ` if there is none).
  - At most 4096 bytes (`EMSGSIZE`).
  - Datagram sockets bind automatically on the first send, then call `udp_output`.
  - Raw sockets build a netbuf (at most 1480 bytes) and call `ip_output(s->laddr, dst, proto)`.

`sock_recv(s, f, buf, n, user, flags, uaddr, ulen)`:

- **Stream:** `ENOTCONN` without a tcb or on a listener. `shut_rd` gives 0. Otherwise it loops on `tcp_recv`, up to 4 KiB per call (`MSG_PEEK` supported). If data was read, `net_kick()` (a window update may be due). The address reported is the peer's.
- **Datagram and raw:** wait for a queued datagram; `shut_rd` with an empty queue gives 0. Copy `min(n, len, 4096)` bytes and silently drop the rest of the datagram (no `MSG_TRUNC`). Consume the datagram unless `MSG_PEEK`.
- The copy to user space happens after `net_lock` is released. Errors from writing back the address are ignored.

Non-blocking behavior comes from `O_NONBLOCK` on the file (set by `SOCK_NONBLOCK` or `fcntl`) or from `MSG_DONTWAIT` (0x40).

#### System calls

Numbers are from the `SYSCALLS` table in `lib/include/uapi/syscall.h`. The implementations are `do_*` in `socket.c`. Every call taking a descriptor first returns `EBADF` (no such descriptor) or `ENOTSOCK` (not a socket).

| Call (nr) | Semantics | Errors |
|---|---|---|
| `socket` (198) | `AF_INET` only. Type bits `& 0xf`; flags `SOCK_NONBLOCK`, `SOCK_CLOEXEC`. Stream (proto 0/TCP), datagram (0/UDP), raw (ICMP only, root only). | `EAFNOSUPPORT`, `EPROTONOSUPPORT`, `EINVAL` (type), `EPERM` (raw, not root), `ENOMEM`, `fd_install` errors |
| `bind` (200) | See "Ports" | `EINVAL` (address length below 16, or already bound), `EFAULT`, `EAFNOSUPPORT`, `EADDRNOTAVAIL`, `EACCES`, `EADDRINUSE` |
| `listen` (201) | Stream only. Binds automatically if needed. Creates the listener tcb. A second `listen` returns 0 and does nothing. | `EOPNOTSUPP`, `EISCONN` (already has a tcb), bind errors, `ENOMEM` |
| `accept` (202) / `accept4` (242) | Blocks for an established child. Creates a socket owning it. Flags apply to the new descriptor. Writes the peer address. | `EINVAL` (not listening), `EAGAIN`, `ERESTARTSYS`, `ENOMEM`, `fd_install` errors |
| `connect` (203) | `0.0.0.0` means `127.0.0.1`. Datagram/raw: set the default peer (datagram binds automatically). Stream: may replace a tcb left `CLOSED` by a failed attempt; picks `laddr` with `ip_source_for`; binds automatically; `tcp_connect`; then `EINPROGRESS` (non-blocking) or waits while in `SYN_SENT`/`SYN_RCVD`. | `EINVAL` (listening), `EALREADY` (`SYN_SENT`), `EISCONN`, `ENETUNREACH`, `EINPROGRESS`, `ERESTARTSYS`, `ECONNREFUSED`, `ETIMEDOUT`, address errors |
| `sendto` (206), `write` | See `sock_send` | `ENOTCONN`, `EFAULT`, `EAGAIN`, `EPIPE`(+`SIGPIPE`), `ECONNREFUSED`, `EDESTADDRREQ`, `EMSGSIZE`, `ENETUNREACH`, `ENOBUFS`, `ERESTARTSYS` |
| `recvfrom` (207), `read` | See `sock_recv` | `ENOTCONN`, `EAGAIN`, `ERESTARTSYS`, `ECONNRESET`/`ETIMEDOUT`/`ECONNREFUSED` (from `tcb->error`), `EFAULT` |
| `shutdown` (210) | `SHUT_RD` sets `shut_rd`. `SHUT_WR` sets `shut_wr` and calls `tcp_shutdown`. Always wakes and kicks. | `EINVAL` (how), `ENOTCONN` (stream without a tcb) |
| `getsockname` (204) | Local address (`0.0.0.0:0` if unbound) | `EFAULT` |
| `getpeername` (205) | Stream: has a tcb, not listening, not `SYN_SENT`. Datagram: `connected`. | `ENOTCONN`, `EFAULT` |
| `setsockopt` (208) | `SO_REUSEADDR`, `SO_RCVBUF`, `SO_SNDBUF` and `IPPROTO_TCP`/1 (`TCP_NODELAY`) accepted and ignored | `ENOPROTOOPT` for everything else (including `SO_RCVTIMEO`) |
| `getsockopt` (209) | `SO_ERROR` returns a positive errno: `s->error` (always 0), else `tcb->error` (not cleared). `SO_TYPE`. | `ENOPROTOOPT`, `EFAULT` |
| `ioctl` | `NET_IOC_GETIF` (by index) fills `struct netif_req`. `NET_IOC_SETIF` (by name, root) sets addr/netmask/gateway/up; it returns 0 and changes nothing for `lo`. | `ENODEV`, `EPERM`, `EFAULT`, `ENOTTY` |
| `close` | `sock_close` → `sock_free`: free queued datagrams, `tcp_close` the tcb, `net_kick` | |
| `ppoll` | `sock_poll` (cached mask) | |

## Concurrency and locking

| Lock | Type | Protects |
|---|---|---|
| `net_lock` ("net") | mutex | Everything else: interfaces and their configuration, the ARP table, `all_sockets` and every socket's fields, every tcb and the `tcbs` list, `net_stats`/`tcp_stats`, `next_id`/`next_ephemeral`/`next_sock_id`. |
| `rxq_lock` ("net-rxq") | spinlock, irqsave | netd's receive queue and `rxq_len` |
| virtqueue `q->lock` | spinlock, irqsave | eth0's receive and transmit rings (`virtio.c: virtq_add`, `virtq_get`) |
| `netd_wq`, `s->wq` | wait queue spinlocks | Wait lists |

Rules:

- **Who runs protocol code.** netd (packets and timers) and system calls (transmit, `tcp_send`/`tcp_recv`, `connect`, `close`) do, always under `net_lock`. `tcp.c`'s header states "Every call happens with net_lock held". Nothing in `tcp.c`, `udp.c`, `ipv4.c` or `ether.c` takes a lock.
- **Interrupt context.** Only `netif_rx`, `nb_alloc`/`nb_free` (the driver refilling and freeing) and `wake_up` run in interrupts. All device interrupts go to cpu0. `netif_rx` never touches protocol state.
- **Lock order:** `net_lock` → `rxq_lock` (through `lo_xmit` → `netif_rx`) → wait queue and `sched_lock`. Also `net_lock` → virtqueue lock (`net_xmit`), `net_lock` → slab locks, and `net_lock` → `proc_lock` (`signal_send` for `SIGPIPE`). `install()`/`fd_install` run without `net_lock`.
- **Sleeping with `net_lock`.** Only through `sock_wait`, which releases it first. However, `sock_send` calls `copy_from_user` while holding `net_lock`, so a user page fault (demand paging or copy-on-write) is handled with the network locked.
- **SMP.** netd is not bound to a CPU. Socket calls run on any CPU. Since all protocol work is serialized by one mutex, at most one CPU does network work at a time. The cost is contention, not correctness.
- **Unlocked reads.** `sock_poll` reads the cached `ready`. netd reads `list_empty(&rxq)` after `prepare_to_wait`. `net_print_stats` reads `rxq_len`. The monitor's `net`/`netstat` read everything without the lock on purpose (the holder may be parked) and say so when `net_lock.owner` is set. `/proc/net/*` takes the lock (`procfs.c: NET_GEN`).
- **netd wakeups.** Received packets cannot be missed: netd checks `rxq` after `prepare_to_wait`. **Kicks can be missed:** netd evaluates `timers` and drops `net_lock` *before* `prepare_to_wait`. A `net_kick()` from a system call that arms the first timer in that window finds no waiter, and netd then sleeps without a timeout. See Limitations.
- **`nb_live`** is a plain `u64` incremented and decremented from netd and system calls (under `net_lock`) and from the virtio interrupt handler (without it). The count can drift on SMP.

## Interfaces

**Kernel functions used by other subsystems**

| Function | Caller | Notes |
|---|---|---|
| `net_init()` | `kinit` | Brings up `lo`, eth0 and netd |
| `netif_register(dev)` | drivers | At most `MAX_NETIFS` (2) |
| `netif_rx(dev, nb)` | drivers, `lo_xmit` | Any context. Consumes `nb`. |
| `nb_alloc`/`nb_free`/`nb_push`/`nb_pull` | drivers | |
| `virtio_net_init()` | `net_init` | |
| `sock_ops` | file layer | `read`, `write`, `close`, `poll`, `ioctl` |
| `do_socket` … `do_accept4` | system call table | |
| `net_print_dev`, `arp_print`, `tcp_print`, `udp_print`, `net_print_stats` | procfs, monitor | `print_fn` style |
| `tcp_state_name` | `trace.c` | Formats `tcp_from`/`tcp_to` trace arguments |

**Hooks `tcp.c` needs** (implemented in `net.c` for the kernel and in `test_tcp.c` for the host): `ip_output`, `net_now_ms`, `net_random`, `net_log`, `kmalloc`/`kzalloc`/`kfree`, `tcp_wake_owner` (→ `sock_wake`), `tcp_trace_state`, `tcp_trace_rexmit`.

**System calls:** see the table above.

**/proc files** (`procfs.c`, generated with `net_lock` held):

| File | Contents |
|---|---|
| `/proc/net/dev` | Interfaces: address, netmask, gateway, MAC, RX/TX packets, bytes, drops |
| `/proc/net/arp` | ARP entries: address, MAC, interface, `resolving`/`ok`, age in seconds, waiting packets |
| `/proc/net/tcp` | Every tcb: id, local, remote, state, SNDQ, RCVQ, SND-WND, CWND, RTO(ms), SRTT, REXMIT, OWNER (`socket`/`backlog`/`orphan`) |
| `/proc/net/udp` | Datagram and raw sockets: id, type, local, remote, queued, rx/tx bytes, pid |
| `/proc/net/stats` | IP, ICMP, UDP, TCP (`tcp_print_stats`) and ARP counters, live netbufs, receive queue length, injected drops |

**Boot options**

| Option | Effect |
|---|---|
| `ip=<a.b.c.d>/<bits>[,<gateway>]` | eth0 address (default `10.0.2.15/24,10.0.2.2`) |
| `net.drop=<percent>` | Drop that share of received packets at random in `netif_rx` (on `lo` too, so loopback TCP loses packets in both directions) |
| `log=net:debug` | One packet-log line per packet, plus ARP debug messages |
| `log=net:trace` | Also TCP state changes (logged at level 4, `LOG_TRACE`) |
| `trace=net` | `net_rx`, `net_tx`, `net_drop`, `tcp_state`, `tcp_rexmit` events |

**Monitor commands:** `net` (interfaces, ARP, statistics) and `netstat` (TCP connections, UDP and raw sockets).

**GDB:** `eu-net` (`tools/gdb/eunuchs.py`) lists interfaces, tcbs and sockets from memory, and the holder of `net_lock`.

**User-space ABI** (`lib/include/uapi/net.h`): Linux constant values, `struct sockaddr_in`, `struct pollfd`, `struct netif_req`, `NET_IOC_GETIF` 0x8900, `NET_IOC_SETIF` 0x8901. libc (`user/lib/net.c`) implements `poll` on `ppoll` and `select` on `poll`: readable is `POLLIN|POLLHUP|POLLERR`, writable is `POLLOUT|POLLERR`.

**Make variables:** `HTTP_PORT` (8080 → guest 80), `SHELL_PORT` (2323 → 23), `UDP_PORT` (5555 → UDP 7), `HOSTFWD` (empty disables forwarding). `NETOPTS` attaches a `filter-dump` that writes `build/net.pcap`.

## Debugging and observability

**First steps for a network problem:**

1. `ping gateway` and `ping localhost`, then `ifconfig` and `netstat -a` (ARP), `-t` and `-s`.
2. Turn on the packet log: boot with `log=net:debug`, or run `log net debug` in the monitor.
3. Open `build/net.pcap` in Wireshark. It is QEMU's view of eth0, so it does not include loopback traffic. The packet log does.
4. Reproduce losses with `net.drop=N`.
5. For TCP logic bugs, write a scenario in `tests/host/test_tcp.c` and debug it on the host.

**Packet log** (`net.c: net_pkt_log`, at `LOG_DEBUG`). It logs every receive (in netd, before processing) and every transmit (`ether.c: xmit` for eth0, `ip_output` for lo). Loopback packets therefore appear twice: `lo tx` and then `lo rx`.

```
eth0 rx 10.0.2.2:80 > 10.0.2.15:49152 TCP [S.] seq … ack … win … len 0
lo tx 127.0.0.1 > 127.0.0.1 ICMP echo request id 7 seq 1
eth0 tx ARP who-has 10.0.2.2 tell 10.0.2.15
```

TCP flags print in the order `S F R P .`.

**Trace events** (`kernel/include/trace_events.h`, group `NET`):

| Event | Arguments |
|---|---|
| `net_rx` / `net_tx` | proto (`TCP`/`UDP`/`ICMP`/`ARP`), length, peer IP. Emitted by `net_pkt_log` whatever the log level. |
| `net_drop` | reason string (`"injected loss"`, `"receive queue full"`), length |
| `tcp_state` | tcb address, from-state, to-state |
| `tcp_rexmit` | tcb address, seq, length. For timeouts the length is the whole flight; for SYNs it is 0. |

**TCP log messages** (`net_log` in `tcp.c`):

- `tcp N a.b.c.d:p: STATE -> STATE` is logged at level 4. In the kernel that is `LOG_TRACE`, so it needs `log=net:trace`, despite the `/* debug */` comment.
- "segment … outside window … ACK and drop" and "ack … beyond snd_max …" use level 5. In the kernel that is above `LOG_TRACE`, so they are never printed. They appear only in the host test with `-vv`.

**Reading the counters:**

| Symptom in `/proc/net/stats` | Meaning |
|---|---|
| `malformed` (`ip_bad`) | Bad IP header or checksum (also counted as `rx_errors`) |
| `fragments dropped` | The peer sent fragments; we do not reassemble |
| `not for us` | Unicast to another address that reached our MAC |
| `no route` | `ip_route` failed (no gateway or interface down) |
| `udp … to closed ports` / `queue full` | No socket matched / 64 datagrams already queued or `shut_rd` |
| `tcp … to no listener` | A RST was sent to a SYN or segment for an unknown port |
| `SYNs over backlog` | The listener's backlog (embryonic plus unaccepted children) is full |
| `retransmits (fast, timeouts)`, `duplicate ACKs`, `out-of-order` | Loss recovery activity |
| `tcp … bad checksums` | Includes bad data-offset fields |
| `arp … unresolved` | Packets dropped waiting for ARP (no answer after 3 tries, or more than 8 queued) |
| `netbufs: N live` | Should be roughly the number of posted receive buffers (64) plus queued packets. Steady growth means a leak, but note that `nb_live` is not SMP-safe. |
| `receive queue N` | netd backlog. RX drops on the interface with a full queue mean netd is starved. |

**Reading `/proc/net/tcp`:**

- **OWNER `orphan`:** the connection is closing on its own.
- **OWNER `backlog`:** a child waiting for `accept`.
- **SND-WND 0 with SNDQ > 0:** the peer's window is shut and we are probing (persist).
- **Growing REXMIT and RTO:** loss or an unreachable peer. After 10 timeouts the connection resets with `ETIMEDOUT`. In the persist state it never gives up.

**Stuck processes.** A thread blocked in a socket call shows `socket` as its wait reason in `ps`. If netd or system calls hang, check the `net_lock` holder with the monitor (`net` prints it) or `eu-net` in GDB.

**Driver counters.** The driver keeps `rx_refill_failures` and `tx_queue_full` in `virtio_net.c`. Neither is exported, so read them with GDB (`p rx_refill_failures`).

**Tests:**

- `make test-host` runs `build/host/test_tcp` with ASan/UBSan. It covers:
  - checksums;
  - a handshake and small exchange;
  - bulk transfer both ways;
  - 5% and 20% loss;
  - 10% duplication;
  - 30 ms latency with 5% loss;
  - a receiver stalled for 3 s (zero window);
  - 25 randomized seeds (10% loss, 5% duplicates, jitter, some with stalls);
  - simultaneous close (at most 8 segments);
  - connection refused, reset on close with unread data, the listen backlog, and `ETIMEDOUT` on a dead link;
  - finally, a check that `nb_live == 0`.

  Run `build/host/test_tcp -vv LOSS SEED DUP STALL` to replay one 60 KB transfer with every state change, retransmission and out-of-window drop logged.
- User tests (`utests`): `tcp_loopback_echo` (200 KB through a forked echo server, driven by `poll` on a non-blocking client socket), `tcp_errors` (refused, `EADDRINUSE`, `ENOTCONN`, reset → `ECONNRESET` then `EPIPE`, raw UDP → `EPROTONOSUPPORT`, `ENOTSOCK`), `nonblocking_sockets` (`EAGAIN` accept, `EINPROGRESS` connect, `SO_ERROR`), `udp_loopback` (automatic bind, truncation, reply). The stress program makes one loopback TCP connection per iteration (`stress.c: tcp_round`).
- `tools/run_tests.py` has two network stages:
  - `network`: ping the gateway and localhost; HTTP from the host (including a live `/proc/net/tcp` that must show `ESTABLISHED`); UDP echo; 1 MiB host → guest; `/bin/sh` guest → host, CRC-checked.
  - `network, 5% loss` (`net.drop=5`): a multi-file transfer over loopback, then 128 KiB from the host.

  Each stage writes a `.pcap` in `build/test-logs/`.

## Limitations and known issues

**Design limits**

- One global mutex for all network state, held across transmit (`ip_output` → `net_xmit`) and across `copy_from_user` in `sock_send`. There is no parallelism, and a user page fault stalls netd.
- Timer resolution is netd's 10 ms tick, and netd polls every 10 ms whenever any TCP retransmission, TIME_WAIT, FIN_WAIT_2 or ARP timer is pending.
- Two interfaces at most, one default gateway, and no routing table.
- No IP fragmentation or reassembly, no options, no PMTU discovery (DF is always set), no ICMP errors sent or acted on. A connect to a host that is down takes the full SYN retry sequence before `ETIMEDOUT`.
- ARP does not keep using a stale entry while it revalidates it. After 10 minutes the next packet waits for a new reply. Pending entries cannot be evicted, so 32 unresolvable destinations block new ARP entries until they time out (3 s).
- Fixed 32 KiB TCP buffers, no window scaling (so at most 32 KiB per round trip between two Eunuchs ends), no SACK, timestamps, delayed ACK, Nagle, keepalive or urgent data. `SO_RCVBUF`, `SO_SNDBUF` and `TCP_NODELAY` are accepted and ignored.
- Loss recovery is basic: fast retransmit without fast recovery, go-back-N after a timeout, and only 8 out-of-order ranges. Out-of-order data beyond that is discarded.
- An out-of-order segment that carries a FIN loses the FIN, so the peer must retransmit it.
- RSTs and SYNs are accepted anywhere in the window (no RFC 5961 challenge ACKs). ISNs come from a xorshift generator seeded from the timer (`net_random`), so they are predictable.
- `TIME_WAIT` lasts only 2 s, and its timer is not restarted by a retransmitted FIN.
- A segment rejected because our receive window is zero is dropped whole, including the ACK it carries. RFC 793 says ACKs and RSTs should still be processed. Recovery then relies on the peer's later pure ACKs (which are accepted at the window edge) or on retransmission.
- Persist probes never give up. A peer that advertises a zero window and then disappears keeps an orphan alive forever, probing every 30 s.
- Orphans in `FIN_WAIT_1`/`FIN_WAIT_2` keep accepting data nobody will read (Linux would reset).
- `find_tcb` returns the first match in list order. A new connection with the same 4-tuple as a `TIME_WAIT` tcb (possible because `TIME_WAIT` ports can be rebound) is shadowed by the old one until it expires. Among several listeners on a port, the last one in the list wins, not the most specific.
- Raw sockets are ICMP only, root only, and receive every ICMP packet (including our own outgoing echo requests on `lo`).
- UDP: at most 1472 bytes per datagram, at most 64 queued per socket, a datagram larger than the read buffer is truncated without `MSG_TRUNC`, no `AF_UNSPEC` disconnect.

**Socket API deviations**

- `read`/`recv` return at most 4096 bytes per call.
- `SO_ERROR` for TCP is not cleared after it is read, and `s->error` is never set. `SO_RCVTIMEO` is in the ABI header but fails with `ENOPROTOOPT`. `MSG_WAITALL` is not supported.
- `tcp_send` turns every error except `ECONNREFUSED` into `EPIPE`, so a `write` after a timeout or reset never sees `ETIMEDOUT`/`ECONNRESET` (`read` does).
- A `connect` interrupted by a signal is restarted as a new `connect`, which then fails with `EALREADY` while the handshake is still in progress.
- `shutdown(SHUT_WR)` while in `SYN_SENT` does nothing in TCP (`tcp_shutdown` ignores that state), and `sock_send` never checks `shut_wr`. Writes keep working and no FIN is ever sent for that shutdown. `shut_wr` is not checked for datagram sockets either.
- A non-blocking `connect` never sets `s->connected`. Nothing currently reads that flag for stream sockets.

**Known bugs and suspicious code**

- **netd can miss a kick** (`net.c: netd`, `net_kick`). netd computes `timers`, releases `net_lock`, and only then calls `prepare_to_wait`. A system call that arms the first TCP timer in that window calls `net_kick` while there is no waiter, and netd then sleeps with no timeout. The retransmission does not happen until the next packet arrives or another kick. The window is small, but the effect is a stalled connection.
- **Karn's rule is not fully applied** (`tcp.c: retransmit_timeout`). The function clears `rtt_timing` and then calls `output()`, which immediately starts timing the *retransmitted* segment at `snd_una`. The next ACK yields an ambiguous sample and replaces the backed-off RTO, contrary to the "Karn: no samples from resends" comment.
- **`abort_conn` sends its RST at `snd_nxt`**, not `snd_max`. After a go-back-N rewind, the peer may reject a RST below its `rcv_nxt` as out of window. This is the same class of bug as the one fixed in `send_ack`.
- **Wrong FIN acknowledgement from `SYN_RCVD`** (`ack_advance`). If the owner shuts down in `SYN_RCVD` (only possible after a simultaneous open), the FIN goes out at once and the state becomes `FIN_WAIT_1`. The ACK that completes the handshake then has `acked = 1` (the SYN), which exceeds `snd_len = 0`, so `fin_was_acked` is set even though only the SYN was acknowledged.
- **Log levels in `tcp.c` do not match the kernel's:** 4 is `LOG_TRACE`, not debug, and 5 is never printed. If level 5 ever passed the threshold, `vklog`'s `"EWIDT"[level]` would index past the letters.
- **The virtio receive ring shrinks permanently** when `post_rx` fails (`nb_alloc` failure in the interrupt handler). Nothing retries later, and the failure count is not visible.

## Potential improvements

1. **Fix netd's lost-kick race** — Add a `netd_kicked` flag set by `net_kick` and checked (and cleared) after `prepare_to_wait`. Alternatively, compute `timers` after `prepare_to_wait` using an unlocked read. This removes a possible indefinite retransmission stall. Impact: high. Effort: small.
2. **Apply Karn's rule after timeouts** — In `retransmit_timeout`, prevent `output()` from timing the retransmitted range: start timing only when `snd_nxt == snd_max` (new data), or keep a "no sample until `snd_max` is acknowledged" flag. Also keep the backed-off RTO until a valid sample arrives (RFC 6298 5.7). Impact: medium. Effort: small.
3. **Send RSTs at `snd_max`** — Change `abort_conn` to use `snd_max`, as `send_ack` does, so resets after a rewind are not dropped by the peer. Add a host-test case that aborts during go-back-N. Impact: medium. Effort: small.
4. **Make zero-window and orphan behavior bounded** — Count persist probes, or add a total orphan lifetime, so a vanished peer with a closed window cannot keep a tcb forever. Reset orphans that receive new data after close, as Linux does. Impact: medium. Effort: small.
5. **Process ACKs on unacceptable segments when the window is zero** — Follow RFC 793/9293: when `rcv_space == 0`, still handle the ACK, window and RST fields of an otherwise valid segment. This avoids relying on retransmissions to learn ACKs while our buffer is full. Impact: medium. Effort: medium.
6. **Align `net_log` levels with `enum log_level`** — Use `LOG_DEBUG` for state changes and `LOG_TRACE` for drop messages in the kernel, and map them in the host test. Today one message needs `net:trace` and the others can never be printed. Impact: low. Effort: small.
7. **Stop copying user data under `net_lock`** — In `sock_send`, copy each chunk from user space before taking `net_lock` (or into a larger staging buffer). Page faults then no longer block netd and every other socket. Impact: medium. Effort: small.
8. **Fix socket-layer API gaps** — Honor `shut_wr` in `sock_send` (`EPIPE`) and queue the FIN when the handshake completes if `shutdown(SHUT_WR)` came during `SYN_SENT`. Clear the TCP error on `SO_ERROR`. Return `ETIMEDOUT`/`ECONNRESET` from the first failing write. Implement `FIONREAD` (the constant is already defined and unused in `socket.c`) and `SO_RCVTIMEO`. Remove the never-written `s->error`, or start using it. Impact: medium. Effort: medium.
9. **Fast recovery (NewReno) and SACK-less partial-ACK handling** — After fast retransmit, inflate `cwnd` per duplicate ACK and retransmit on partial ACKs (RFC 6582) instead of waiting for an RTO when several segments are lost in one window. The randomized host test can measure the effect. Impact: medium. Effort: medium.
10. **Delayed ACKs and Nagle** — ACK every second full segment or after about 40 ms, and coalesce small writes when data is unacknowledged. This roughly halves ACK traffic for bulk transfers and avoids tiny segments from interactive programs (`rshd`). It needs a second per-tcb timer. Impact: medium. Effort: medium.
11. **Window scaling and configurable buffers** — Add the RFC 7323 window-scale option and let `SO_RCVBUF`/`SO_SNDBUF` size the rings. 32 KiB caps throughput at 32 KiB per RTT. Impact: medium. Effort: medium.
12. **Generate and honor ICMP errors** — Send port-unreachable for UDP datagrams to closed ports. On host/port unreachable for a `SYN_SENT` connection, fail `connect` quickly. Impact: low. Effort: medium.
13. **Export driver and buffer health** — Put `rx_refill_failures`, `tx_queue_full` and posted receive buffers in `/proc/net/dev` or `/proc/net/stats`. Retry `post_rx` from netd when the ring is short. Make `nb_live` atomic (or per-CPU) so the leak check is reliable on SMP. Impact: low. Effort: small.
14. **Better demultiplexing** — Prefer a specific-address listener over a wildcard one in `find_tcb`. When a SYN matches a `TIME_WAIT` tcb with a higher sequence number, let the new connection replace it (RFC 6191-style). Consider hashing tcbs and sockets by port instead of scanning lists. Impact: low. Effort: medium.
15. **Harden against blind attacks** — Use RFC 6528 ISNs (a keyed hash of the 4-tuple plus a clock), and RFC 5961 challenge ACKs for in-window RSTs and SYNs. This matters little for a VM on a private network, but these are standard behaviors. Impact: low. Effort: medium.
16. **Remove dead declarations** — `sockets_print` is declared in `net.h` but never defined. `FIONREAD` is defined in `socket.c` but unused. The unreachable `SYN_RCVD` case in `receive_data`'s FIN switch, and the direct `FIN_WAIT_1` → `TIME_WAIT` path, deserve a comment or removal. Impact: low. Effort: small.
