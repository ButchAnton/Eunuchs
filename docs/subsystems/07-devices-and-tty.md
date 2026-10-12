# Device drivers and the terminal

This document covers the hardware-facing half of Eunuchs: the PLIC interrupt router, the 16550 UART and the kernel console, the virtio-mmio transport with its split virtqueues, the virtio block and network drivers, the goldfish RTC, the console TTY line discipline, and the device files in `/dev`. All drivers target QEMU's `virt` board. The code is small (about 1,600 lines across the files listed below) and deliberately simple: one console TTY, one disk, one NIC, every device interrupt on cpu0.

## Responsibilities and scope

**This subsystem does:**

- Discover nothing on its own: device addresses and IRQ numbers come from `struct platform_info`, which `kernel/core/fdt.c` fills from the device tree. Drivers map their registers with `ioremap` (`kernel/mm/vmm.c`).
- Route external interrupts from the PLIC to registered handlers on the boot hart (`kernel/arch/riscv/plic.c`).
- Drive the UART: polled output, interrupt-driven input (`kernel/drivers/uart16550.c`, `kernel/core/console.c`).
- Split console input between the debug monitor (`Ctrl-]`) and the TTY (`console_rx_char`).
- Implement the console line discipline, termios ioctls and terminal job control (`kernel/drivers/tty.c`).
- Implement the virtio-mmio modern transport, device-status handshake and split virtqueues (`kernel/drivers/virtio.c`).
- Provide a block I/O primitive, `blk_rw(disk, ...)`, for up to four virtio disks, used by the buffer cache and `/dev/disk0`/`disk1` (`kernel/drivers/virtio_blk.c`).
- Provide the `eth0` network interface's transmit function and receive path into `netif_rx` (`kernel/drivers/virtio_net.c`).
- Provide wall-clock time, `rtc_time_ns` (`kernel/drivers/goldfish_rtc.c`).
- Expose `/dev/console`, `/dev/tty`, `/dev/tty0`, `/dev/null`, `/dev/zero`, `/dev/kmsg`, `/dev/disk0`, `/dev/disk1` (`kernel/fs/devfs.c`).

**It does not:**

- Parse the device tree (core, `fdt.c`), handle traps in general (`trap.c`), or implement signals and process groups (`kernel/proc/signal.c`). The TTY only *sends* signals and reads process-group fields.
- Implement any network protocol. The driver hands frames to `netif_rx` (`kernel/net/net.c`) and gets frames through `netif.xmit`; netbufs belong to the network stack (`kernel/net/netbuf.c`). See the network document.
- Cache disk blocks or know about filesystems. `kernel/fs/bcache.c` calls `blk_rw`; see the filesystem document.
- Implement the monitor itself (`kernel/debug/monitor.c`); this document only covers how keystrokes reach it.

## Source map

| File | Lines | Role |
|---|---:|---|
| `kernel/arch/riscv/plic.c` | 118 | PLIC setup, `irq_register`, claim/complete loop (`plic_handle`), `/proc/interrupts` |
| `kernel/include/irq.h` | — | `MAX_IRQS` (64), `irq_handler_t`, `HZ` |
| `kernel/drivers/uart16550.c` | 67 | NS16550A registers: init, polled `uart_putc`, `uart_getc`, RX interrupt enable |
| `kernel/core/console.c` | 84 | Console abstraction over the UART (or SBI before it); UART IRQ handler; `Ctrl-]` to the monitor |
| `kernel/core/printk.c: console_write_locked` | — | Console output serialized with kernel log lines (used by the TTY) |
| `kernel/include/console.h` | — | Console and UART prototypes |
| `kernel/drivers/tty.c` | 394 | Console TTY: line discipline, termios, job control, `tty_ops` |
| `lib/include/uapi/termios.h` | 71 | `struct termios` (Linux layout), flag bits, `c_cc` indexes, ioctl numbers, `struct winsize` |
| `kernel/include/virtio.h` | 94 | Virtqueue/ring structures, `struct virtio_dev`, transport and block API |
| `kernel/drivers/virtio.c` | 254 | virtio-mmio transport, feature negotiation, `virtq_*`, `dma_addr`, `/proc/virtio` |
| `kernel/drivers/virtio_blk.c` | 154 | virtio-blk: `blk_init`, `blk_rw`, completion IRQ, statistics |
| `kernel/drivers/virtio_net.c` | 123 | virtio-net as `eth0`: RX buffer posting, TX, IRQ |
| `kernel/drivers/goldfish_rtc.c` | 21 | `rtc_time_ns` |
| `kernel/mm/vmm.c: ioremap` | — | Map an MMIO range into the direct map |
| `kernel/fs/devfs.c` | 210 | `/dev` filesystem: device table, `/dev/tty` redirection, `kmsg`, `disk0`, `disk1` |
| `kernel/core/fdt.c` (match code) | — | Fills `platform.uart`, `.plic`, `.rtc`, `.virtio[]` from `compatible` strings |
| `user/lib/libc.c` | — | `tcgetattr`, `tcsetattr`, `cfmakeraw`, `tcgetpgrp`/`tcsetpgrp`, `isatty` (user-side termios) |

## Concepts and data structures

### Platform devices and ioremap

`kernel/core/fdt.c` matches device-tree nodes by `compatible`:

| `compatible` | Field in `platform` | Notes |
|---|---|---|
| `ns16550a`, `ns16550` | `uart` | first one only |
| `riscv,plic0`, `sifive,plic-1.0.0` | `plic` | |
| `google,goldfish-rtc` | `rtc` | |
| `virtio,mmio` | `virtio[]`, `nvirtio` | up to `MAX_VIRTIO` = 8 slots, in device-tree order |

Each is a `struct mmio_dev { base, size, irq }` (`kernel/include/platform.h`). The IRQ is the first cell of the node's `interrupts` property.

`ioremap(pa, size)` (`kernel/mm/vmm.c`) records the page-aligned range in a fixed table of `MAX_IOREMAP` = 32 entries and returns `__va(pa)`: MMIO lives in the same direct-map window as RAM (`KERNEL_BASE + pa`). Before `vm_init`, the early boot page table already covers low physical memory, so the range is only recorded; `vm_init` maps every recorded range read-write. After `vm_init`, `ioremap` maps immediately with `vm_map`. A repeated call with the same page-aligned start and a size that fits returns at once, so drivers may call it repeatedly (`virtio_print` does). Exceeding 32 regions panics (`"ioremap: too many regions"`). Touching an unmapped MMIO address faults, and `vm_fault_hint` explains it as `"unmapped physical address: MMIO region not ioremap()ed?"`.

### DMA addresses

`dma_addr(va)` (`kernel/drivers/virtio.c`) converts a kernel virtual address to the physical address the device should use:

- Addresses in `[KERNEL_BASE, KERNEL_BASE + mem_base + mem_size)` (the direct map, which includes the kernel image, slab and page allocations) are converted with `__pa`.
- Anything else (in practice the kernel-stack area at `0xffffffe800000000`) is looked up with `vm_translate(kernel_pagetable, ...)`. An unmapped address panics with `"dma_addr: %p is not mapped"`.

There is no IOMMU, bounce buffering or cache maintenance (QEMU's DMA is coherent). The translated address is only valid for the page containing `va`: a buffer outside the direct map must not cross a page, because consecutive virtual pages of a kernel stack need not be physically adjacent. `blk_rw` enforces this with a panic (below). Direct-map buffers may span pages because the direct map is physically contiguous.

### PLIC interrupt table

`kernel/arch/riscv/plic.c` keeps a static `struct irq_desc irqs[MAX_IRQS]` (64 entries):

| Field | Meaning |
|---|---|
| `name` | Shown in `irq` / `/proc/interrupts` |
| `handler`, `arg` | Called as `handler(arg)` from `plic_handle` |
| `count` | Number of claims dispatched to this handler |
| `time` | `rdtime` ticks spent inside the handler |

Plus a global `spurious` counter (claims with no handler) and `plic_ctx = 2 * platform.boot_hart + 1`, the boot hart's S-mode context on QEMU's PLIC layout. Register offsets used: priority `4*irq`, enable `0x2000 + 0x80*ctx`, threshold `0x200000 + 0x1000*ctx`, claim/complete `0x200004 + 0x1000*ctx`. Only three pages are mapped: the priority page, the page with this context's enable bits, and the page with its threshold/claim registers. Every registered source gets priority 1; the threshold is 0.

### Virtio device and virtqueue

`struct virtio_dev` (`kernel/include/virtio.h`):

| Field | Meaning |
|---|---|
| `slot` | Index into `platform.virtio[]` |
| `regs` | Kernel VA of the MMIO register block (one page is mapped) |
| `irq` | PLIC source number |
| `device_id` | `VIRTIO_DEV_NET` (1) or `VIRTIO_DEV_BLOCK` (2) |
| `features` | Negotiated feature bits (offered & wanted, always including `VIRTIO_F_VERSION_1`, bit 32) |

The MMIO registers used (`kernel/drivers/virtio.c`, virtio 1.2 section 4.2.2):

| Offset | Register | Use |
|---|---|---|
| `0x000` | MagicValue | must be `0x74726976` ("virt") |
| `0x004` | Version | must be 2 (modern); 1 means legacy and is rejected |
| `0x008` | DeviceID | 0 = empty slot |
| `0x010`/`0x014` | DeviceFeatures / Sel | read both 32-bit halves |
| `0x020`/`0x024` | DriverFeatures / Sel | write both halves |
| `0x030`–`0x044` | QueueSel, QueueNumMax, QueueNum, QueueReady | queue setup |
| `0x050` | QueueNotify | the "kick" |
| `0x060`/`0x064` | InterruptStatus / InterruptACK | read then write back the same bits |
| `0x070` | Status | device-status handshake |
| `0x080`–`0x0a4` | QueueDesc/Driver(avail)/Device(used) Low/High | ring physical addresses |
| `0x100` | Config space | device-specific (`virtio_config_read32`) |

`struct virtqueue`:

| Field | Meaning |
|---|---|
| `index`, `num` | Queue number on the device; ring size (≤ 128) |
| `desc` | Descriptor table, `num` × 16 bytes |
| `avail` | Driver ring: `flags`, `idx`, `ring[num]` (volatile) |
| `used` | Device ring: `flags`, `idx`, `ring[num]` of `{id, len}` (volatile) |
| `free_head`, `nfree` | Head and length of the free-descriptor list, linked through `desc[i].next` |
| `last_used` | Next used-ring index the driver has not consumed yet (free-running u16) |
| `cookies` | `void *[num]`, indexed by a chain's head descriptor; the request object returned on completion |
| `lock` | Spinlock `"virtqueue"` protecting all of the above |
| `free_wq` | Wait queue `"virtq-desc"`: woken whenever descriptors are returned |
| `submitted`, `completed` | Counters; their difference is the number in flight |

#### Ring memory layout

`virtq_init` puts the whole queue in **one zeroed page** and caps `num` at 128 so it fits:

```
page + 0                         desc[num]           16 * num bytes
page + 16*num                    avail               flags(2) idx(2) ring(2*num) used_event(2)
page + ALIGN_UP(16*num + 6 + 2*num, 64)
                                 used                flags(2) idx(2) ring(8*num) avail_event(2)
```

For `num` = 128: descriptors 0–2047, avail 2048–2309, used from 2368 to 3397, all inside 4096 bytes. For `num` = 64 the used ring starts at 1216.

Invariants:

- Initially every descriptor is free and `desc[i].next = i + 1` (the last one's `next` is out of range but is never followed, because `nfree` bounds every walk).
- `avail->idx` and `last_used` are free-running 16-bit counters; ring slots are `idx % num`. This is only correct when `num` divides 65536, i.e. is a power of two. The callers request 64 and the code caps at 128 and at QueueNumMax, which keeps it a power of two as long as the device's maximum is one (virtio requires that for split queues).
- A chain occupies `n` descriptors linked by `VIRTQ_DESC_F_NEXT`; the head index identifies it in both the avail and used rings and in `cookies[]`.
- `cookies[head]` is non-NULL exactly while the chain is owned by the device. A completion whose cookie is NULL is logged (`"virtqueue %u: completion for descriptor %u with no request"`).

`struct virtio_buf { addr (physical), len, device_writes }` describes one segment of a chain for `virtq_add`.

### Block request

`struct blk_req` (`kernel/drivers/virtio_blk.c`), allocated per request with `kzalloc`:

| Field | Meaning |
|---|---|
| `hdr.type` | `VIRTIO_BLK_T_IN` (0, read) or `VIRTIO_BLK_T_OUT` (1, write) |
| `hdr.reserved`, `hdr.sector` | Request header the device reads (16 bytes); sector is in 512-byte units |
| `status` | One byte the device writes; initialized to `0xff`, `VIRTIO_BLK_S_OK` = 0 |
| `done` | Set by the IRQ handler; the submitter sleeps until it is true |
| `start` | `rdtime` at submission, for latency statistics |

Each request is a three-descriptor chain:

```
desc[h]   -> &r->hdr     16 bytes    device reads
desc[h+.] -> buf         len bytes   device writes for a read, reads for a write
desc[h+.] -> &r->status  1 byte      device writes
```

With the 64-descriptor queue that `blk_init` asks for, at most 21 requests can be in flight.

Driver state is one `struct disk` per disk in `disks[MAX_DISKS]` (`MAX_DISKS` = 4, `kernel/include/virtio.h`), `ndisks` of them in use. Each has its own `dev`, `vq`, `readonly` (from `VIRTIO_BLK_F_RO`, bit 5), `capacity` (sectors, from config offset 0 as two 32-bit reads), wait queue `done_wq` (`"blk-io"`), and counters `n_reads`, `n_writes`, `sectors_read`, `sectors_written`, `n_errors`, `busy_ticks`, `max_latency`, `total_latency`. A disk is named by its index ("disk0", "disk1", ...): the order in which `blk_init` found it.

### Network driver state

`kernel/drivers/virtio_net.c` has two queues: `rxq` (queue 0, `RX_BUFFERS` = 64 descriptors) and `txq` (queue 1, 64 descriptors), plus the `struct netif eth0` it registers (`name "eth0"`, `has_mac`, `up`, `mtu = ETH_MTU` = 1500) and two counters, `rx_refill_failures` and `tx_queue_full`.

Each packet carries a 12-byte virtio-net header (`VNET_HDR_LEN`: `struct virtio_net_hdr` including `num_buffers`, which is always present with `VIRTIO_F_VERSION_1`). Netbufs (`kernel/include/net.h`) are `NB_SIZE` = 2048 bytes with `NB_HEADROOM` = 128. A receive buffer is laid out so that, after the header is stripped, the Ethernet frame starts exactly at `head + NB_HEADROOM`, the same place `nb_alloc` puts data:

```
head                head+116        head+128                          head+2048
|  unused headroom  | virtio hdr 12 | Ethernet frame ...               |
                    ^ descriptor addr, length 2048 - 116 = 1932 bytes
```

Only `VIRTIO_NET_F_MAC` (bit 5) is requested; with no `MRG_RXBUF`, every frame (at most 1514 + 12 bytes) must fit in one buffer, which 1932 bytes does.

### The TTY

There is exactly one TTY, `console_tty` (`kernel/drivers/tty.c`):

| Field | Meaning |
|---|---|
| `lock` | Spinlock `"tty"` |
| `t` | Current `struct termios` |
| `line[LINE_MAX + 1]`, `line_len` | Canonical-mode line being edited (`LINE_MAX` = 255) |
| `q[QUEUE]`, `qhead`, `qtail` | Readable input ring of 1024 16-bit entries: a byte, or the marker `Q_EOF` (0x100) where `^D` ended a line; free-running u32 indexes |
| `delims` | Entries in `q` that end a canonical read (`'\n'` and `Q_EOF`); maintained by `q_push`/`q_pop` in every mode |
| `rwait` | Wait queue `"tty-read"`, woken on every input character and on termios changes |
| `sid`, `pgrp` | Session and foreground process group |
| `ws` | `struct winsize`, initialized to 24 rows × 80 columns; set by `TIOCSWINSZ` (the shell does, when the terminal reports its size) |
| `in`, `out`, `dropped`, `sigs` | Counters for `tty` / `/proc/tty` |

Two-stage input buffering in canonical mode: characters accumulate in `line` while being edited, and are copied to `q` (`commit_line`) when the line ends. In raw mode characters go straight to `q`. Readers only take from `q`.

`struct termios` (`lib/include/uapi/termios.h`) uses the Linux layout and values: four `tcflag_t` (u32) flag words, `c_line`, and `c_cc[NCCS]` with `NCCS` = 19. Default settings (`tty_defaults`):

| Word | Default | Bits that the kernel acts on |
|---|---|---|
| `c_iflag` | `ICRNL \| IXON` | `ICRNL`, `IGNCR` |
| `c_oflag` | `OPOST \| ONLCR` | none (see Limitations) |
| `c_cflag` | `CS8 \| CREAD` | none |
| `c_lflag` | `ISIG ICANON ECHO ECHOE ECHOK ECHOCTL IEXTEN` | `ISIG`, `ICANON`, `ECHO`, `ECHOE`, `ECHONL`, `NOFLSH`, `ECHOCTL` |

| `c_cc` index | Default | Meaning here |
|---|---|---|
| `VINTR` (0) | `^C` (3) | SIGINT to the foreground group (ISIG) |
| `VQUIT` (1) | `^\` (0x1c) | SIGQUIT (ISIG) |
| `VERASE` (2) | DEL (0x7f) | erase one character (`\b` also always erases) |
| `VKILL` (3) | `^U` (0x15) | erase the line |
| `VEOF` (4) | `^D` (4) | end the line without a newline; on an empty line, EOF |
| `VTIME` (5) | 0 | ignored |
| `VMIN` (6) | 1 | only "0 or not 0" matters in raw mode |
| `VSUSP` (10) | `^Z` (0x1a) | SIGTSTP (ISIG) |
| `VWERASE` (14) | `^W` (0x17) | erase the previous word (canonical) |

## How it works

### Boot order

From `kernel/core/main.c: kmain` and `kinit`:

1. `fdt_parse` fills `platform`; `console_init` calls `uart_init` if a UART was found. Before that, `console_putc` goes through `sbi_console_putchar`.
2. `vm_init` maps every range `ioremap`ped so far (the UART at least).
3. `plic_init` maps the PLIC, sets this context's threshold to 0 and sets `SIE_SEIE` in `sie`.
4. `tty_init` loads the default termios.
5. `console_enable_irq` registers `uart_irq` for `platform.uart.irq` and enables the UART's receive interrupt. From here on, keystrokes reach the TTY.
6. `kinit` (first kernel thread) calls `blk_init`, which returns the number of disks found (`kinit` panics if it is 0: `"no disk: the root filesystem lives on a virtio block device ..."`), then `vfs_init` (mounts `/`, `/home` and devfs on `/dev`), then `net_init`, which calls `virtio_net_init`.
7. `proc_spawn` opens `/dev/console` three times for fds 0–2 of `init`, makes it a session leader and calls `tty_set_ctty`, giving it the console as controlling terminal and its group as the foreground group.

The RTC has no initialization; `rtc_time_ns` maps its page on first use.

### Interrupt flow: PLIC to handler

```
device raises line --> PLIC (priority 1 > threshold 0, enabled for ctx 2*boot_hart+1)
   --> S-mode external interrupt on cpu0 (scause = IRQ_S_EXT)
   --> trapentry.S --> trap.c: handle_interrupt
         irq_count[code]++, irq_depth++, irq_tf = tf, trace irq_entry
   --> plic.c: plic_handle
         loop: irq = *claim; if 0 break
               c->last_irq = irq
               irqs[irq].count++; handler(arg); irqs[irq].time += elapsed
               (or spurious++ if no handler)
               *claim = irq          /* complete */
   --> trace irq_exit; maybe preempt_schedule() if need_resched and the
       interrupted code had interrupts enabled
```

Details:

- Only the boot hart's S-mode context has any source enabled, so device interrupts are taken only on cpu0 (the boot hart becomes cpu0). Other CPUs never claim. A handler therefore never runs concurrently with itself.
- Hardware clears `sstatus.SIE` on trap entry and `handle_interrupt` does not re-enable it: handlers run with interrupts off and must not sleep. They may take spinlocks (with `irqsave`), allocate with `kmalloc`, and call `wake_up`.
- `plic_handle` keeps claiming until the claim register reads 0, so several pending sources are handled in one trap.
- `irq_register(irq, name, handler, arg)` fails (returns -1 and logs `"irq_register(%u, %s) failed"`) if the PLIC was not initialized, `irq` is 0 or ≥ 64, or the source already has a handler. There is no sharing and no unregistering.

### Console output

`console_putc` (`kernel/core/console.c`) turns `\n` into `\r\n` unconditionally and writes through `uart_putc`, which busy-waits on `LSR_THRE` and writes `THR`. Output is fully synchronous and works with interrupts off, which printk and panic rely on. `console_write_locked` (`kernel/core/printk.c`) wraps `console_write` in `log_lock` (with interrupts off) unless a panic is in progress, so user output and kernel log lines do not interleave mid-line. All TTY output (`out_raw`) goes through it.

`uart_init` disables UART interrupts, sets divisor 3 (38400 baud; QEMU ignores it), 8N1, and enables and clears the FIFOs. `uart_enable_rx_irq` sets `MCR.OUT2` and `IER_RX`.

### Console input: monitor vs. TTY

```
UART RX --> PLIC --> plic_handle --> console.c: uart_irq
              while ((c = uart_getc()) >= 0) console_rx_char(c)
                 c == 0x1d (Ctrl-])  --> monitor_enter("console break (Ctrl-])", irq_tf, true)
                 otherwise           --> tty_input(c)
```

- `Ctrl-]` (`BREAK_CHAR` = 0x1d) is checked before the line discipline, in every termios mode. A user program can never read byte 0x1d.
- The monitor runs inside the UART interrupt handler, on cpu0, with interrupts off and the other CPUs stopped. It reads keys with `console_getc`, which polls `uart_getc` and sleeps about 10 ms between polls with `cpu_wait_ticks` (a `wfi` with only the timer interrupt enabled), so it does not depend on the UART interrupt. It has its own line editor (`monitor.c: readline`: DEL/`^H`, `^U`, `^C`, arrow-key history) and ignores termios.
- When the monitor returns (`c`), `uart_irq` continues draining the FIFO into the TTY, and `plic_handle` then completes the UART claim.
- Monitor entries that happen before `plic_init` (boot option `break`, early panics) use the same polling `console_getc`; without a UART it falls back to `sbi_console_getchar`.

### TTY input: `tty_input`

Called once per character from the UART interrupt, with `tty->lock` held for the processing:

1. `in++`. If `c == '\r'`: drop it if `IGNCR`; otherwise turn it into `'\n'` if `ICRNL`.
2. **Signals** (if `ISIG`): `VINTR` → SIGINT, `VQUIT` → SIGQUIT, `VSUSP` → SIGTSTP. Unless `NOFLSH`, all pending input is flushed (`flush_input`: edited line, queue, `delims`). The character is echoed (`^C`) followed by a newline if `ECHO`. Processing stops.
3. **Canonical mode** (`ICANON`):
   - `VERASE` or `'\b'`: `erase_chars(1)`. With `ECHO` and `ECHOE`, each erased character is rubbed out with `"\b \b"`, twice for a control character shown as `^X` (when `ECHOCTL`).
   - `VKILL`: erase the whole line the same way.
   - `VWERASE`: erase trailing spaces, then the preceding non-space run (only `' '` counts as a separator).
   - `VEOF`: `commit_line(false)`: copy the line to `q` followed by a `Q_EOF` marker. `^D` is not echoed.
   - `'\n'`: echo it, `commit_line(true)`: copy the line plus `'\n'` to `q`.
   - `commit_line` is all or nothing: if the line and its delimiter don't fit in `q`, the whole line is dropped (and counted in `dropped`), so a line is never committed without its end.
   - Any other character: append to `line` and echo it, unless the line already has 255 characters, in which case `dropped++` and the character is lost.
4. **Raw mode**: push the character to `q` (or count it in `dropped` if `q` holds 1024 entries) and echo it.
5. After unlocking: if a signal was recognized, `sigs++`, emit the `tty_signal` trace event and, if there is a foreground group, `signal_send_pgrp(tty->pgrp, sig, 0)`. Always `wake_up(&tty->rwait)`.

Echo (`echo_char`): nothing unless `ECHO`, except a newline when `ECHONL`. With `ECHOCTL`, bytes below 0x20 other than `\n` and `\t` print as `^` followed by `c + '@'`. Echo is written synchronously from the interrupt handler.

### TTY read: `tty_read`

1. **Job control** (`job_control`): if the caller's process has this TTY as controlling terminal, a foreground group is set and the caller is not in it, then: if SIGTTIN is ignored or blocked, return `-EIO`; otherwise send SIGTTIN to the caller's group and return `-ERESTARTSYS` (the read restarts after `SIGCONT`). Processes without the TTY as controlling terminal (e.g. after `setsid`) read without this check.
2. **Raw mode with `VMIN` = 0** and nothing queued: return 0 at once (a polling read; `top` uses this).
3. Otherwise sleep interruptibly on `rwait` until `readable()`: in canonical mode `delims > 0` (a complete line or a `^D` is queued); in raw mode `q` non-empty. A signal returns `-ERESTARTSYS`. The thread's `wait_what` is `"tty read"`.
4. Under the lock, pop entries into a 256-byte stack buffer, up to `min(n, 256)` bytes:
   - canonical mode stops after a `'\n'` (included), or at a `Q_EOF` marker (consumed, not returned). A marker with nothing before it is end of file: the read returns 0;
   - raw mode returns whatever is queued; `Q_EOF` markers left over from canonical input are skipped.
5. Copy to the caller (`copy_to_user` or `memcpy` for kernel callers); `-EFAULT` on a bad pointer.

Lines and `^D`s come out in the order they were typed, whatever mixture of modes reads them. So a canonical read returns at most one line (or part of one if `n` is smaller; the rest stays queued), and a raw read returns whatever is queued, up to 256 bytes, as soon as there is at least one byte. `VMIN` values above 1 and `VTIME` are not implemented.

### TTY write: `tty_write`

Copies from the caller in 256-byte chunks and writes each chunk with `out_raw` → `console_write_locked`. Output is synchronous (the call returns after the UART has accepted every byte), not subject to job control, and not processed by termios (`OPOST`/`ONLCR` are ignored; `console_putc` always emits `\r\n`). A fault after some bytes returns the count written so far.

### termios and other ioctls: `tty_ioctl`

| Request | Behavior |
|---|---|
| `TCGETS` | Copy `tty->t` out |
| `TCSETS`, `TCSETSW`, `TCSETSF` | Copy in a `struct termios`; under the lock, `TCSETSF` first flushes all input. Install it. If leaving canonical mode with a partially edited line, move that line into `q` so it can be read raw. Wake readers. `TCSETSW` does not wait for anything: output is already synchronous. |
| `TIOCGPGRP` | Return the foreground group |
| `TIOCSPGRP` | Set the foreground group; `-ENOTTY` unless the caller's controlling terminal is this TTY and its session is the TTY's session. The group id is not validated. |
| `TIOCSCTTY` | Caller must be a session leader (`sid == pid`), else `-EPERM`. Makes the console its controlling terminal and sets the TTY's session and foreground group to the caller's. |
| `TIOCNOTTY` | Clear the caller's controlling terminal if it is this TTY |
| `TIOCGWINSZ` | Return `ws` (24 × 80 until set) |
| `TIOCSWINSZ` | Set `ws`; ignored if rows or columns is 0. No `SIGWINCH` is sent. |
| `FIONREAD` | Bytes in `q` (committed input; not the line being edited) |
| other | `-ENOTTY` |

User space maps these in `user/lib/libc.c`: `tcsetattr(TCSANOW/TCSADRAIN/TCSAFLUSH)` → `TCSETS/TCSETSW/TCSETSF`; `isatty` is `tcgetattr` succeeding; `cfmakeraw` clears `ICRNL IXON OPOST ECHO ECHONL ICANON ISIG IEXTEN` and sets `VMIN` 1, `VTIME` 0.

`tty_poll` registers `rwait` and reports `POLLIN` when `readable()`, and always `POLLOUT`.

### Controlling terminal and sessions

- `tty_set_ctty(p)` (called by `proc_spawn` for kernel-spawned processes such as init) sets `p->ctty` and makes `p`'s session and group the TTY's.
- `fork` copies `ctty` (`signal_fork`); `setsid` clears it (`do_setsid`). Only `TIOCSCTTY` or `tty_set_ctty` set it again. Opening `/dev/console` does not make it the controlling terminal; `O_NOCTTY` is defined but unused.
- The shell (`esh`) uses `tcsetpgrp` (`TIOCSPGRP`) to move jobs to the foreground and `TCSETSW` to restore its saved modes after a job. While you edit a command line it puts the TTY in raw mode (no `ICANON`, `ECHO`, `ISIG`, `ICRNL`), and reads keys with `ppoll` timeouts to tell a lone Escape from an escape sequence ([12-shell.md](12-shell.md#the-line-editor)).

### virtio-mmio initialization handshake: `virtio_find`

`virtio_find(id, want, dev)` is `virtio_find_from(0, id, want, dev)`. `virtio_find_from(from, ...)` starts the scan at slot `from` and records the slot it used in `dev->slot`, so a driver can find several devices of one type by calling it again from `dev->slot + 1`. For each slot `i` from `from` in `platform.virtio[]`:

1. `ioremap` one page and check MagicValue and DeviceID; skip slots that do not match.
2. If Version ≠ 2, log `"virtio%d: legacy (version %u) interface; run QEMU with -global virtio-mmio.force-legacy=false"` and return -1 (the Makefile passes that option).
3. Status ← 0 (reset), then `ACKNOWLEDGE` (1), then `ACKNOWLEDGE | DRIVER` (2).
4. Read the 64 offered feature bits (two 32-bit halves through DeviceFeaturesSel). Negotiated = offered & (want | `VIRTIO_F_VERSION_1`). If `VERSION_1` is missing: status ← `FAILED` (128), error.
5. Write the negotiated bits (two halves), set `FEATURES_OK` (8), read Status back; if the device cleared `FEATURES_OK`, status ← `FAILED`, error `"device rejected features"`.
6. Fill `*dev` and log `"virtio%d: %s device at %#lx irq %u, features %#lx"`.

The driver then calls `virtq_init` for each queue, `irq_register`, and finally `virtio_ready`, which ORs in `DRIVER_OK` (4). The device is not notified before `DRIVER_OK`.

`virtq_init(dev, q, index, num)`:

1. Select the queue; fail if it is already ready or QueueNumMax is 0.
2. `num = min(num, QueueNumMax, 128)`.
3. Allocate one zeroed page for the three rings (layout above), and `cookies` with `kzalloc`.
4. Link all descriptors into the free list; init `lock` and `free_wq`.
5. Write QueueNum, the three physical addresses (`__pa`; the page is in the direct map), and QueueReady ← 1.

### Descriptor management: `virtq_add`, `virtq_kick`, `virtq_get`

`virtq_add(q, bufs, n, cookie)` (under `q->lock`, interrupts off):

1. If fewer than `n` descriptors are free, return -1. Callers either sleep on `free_wq` and retry (block) or reclaim and retry once (net TX).
2. Take `n` descriptors from the free list, filling `addr`, `len`, and flags (`VIRTQ_DESC_F_WRITE` for device-writable segments, `VIRTQ_DESC_F_NEXT` on all but the last). The `next` links already present in the free list become the chain links, so no relinking is needed.
3. `free_head` ← the old `next` of the last chain descriptor; `nfree -= n`; `cookies[head] = cookie`.
4. Write `head` into `avail->ring[avail->idx % num]`, then a full fence (`__sync_synchronize`), then `avail->idx++`, so the device never sees the new index before the ring entry and descriptors.

`virtq_kick` issues a fence and writes the queue index to QueueNotify. It is separate from `virtq_add` so several chains can be added per notification (`net_irq` does this for RX refills). The driver never checks `used->flags` (notification suppression) and does not negotiate `EVENT_IDX`; it always kicks.

`virtq_get(q, &len)` (under `q->lock`):

1. Fence; if `last_used == used->idx`, return NULL.
2. Read `used->ring[last_used % num]` (`id` = head, `len` = bytes the device wrote), `last_used++`.
3. Take and clear `cookies[id]`.
4. Walk the chain by `VIRTQ_DESC_F_NEXT` to count its descriptors, link the tail to the old `free_head`, make `id` the new `free_head`, `nfree += count`.
5. Unlock, `wake_up(&q->free_wq)`, return the cookie.

`virtio_ack_irq` reads InterruptStatus and writes the same value to InterruptACK; drivers call it first in their handler. Bit 1 (configuration change) is acknowledged but not acted on.

### Block I/O request lifecycle

`blk_init` loops until `MAX_DISKS` disks are found or `virtio_find_from(slot, VIRTIO_DEV_BLOCK, VIRTIO_BLK_F_RO)` fails. For each device: read `capacity` from config bytes 0–7, `virtq_init(…, 0, 64)` (on failure, log it and skip the device), initialize its `done_wq`, register `blk_irq` with the `struct disk` as its argument, `DRIVER_OK`, log `"disk%d: %lu MiB (%lu sectors)%s, queue of %u descriptors"`. It returns the number of disks; with none it logs `no virtio block device`.

Disk numbers follow the order of the virtio-mmio slots in the device tree, which need not match the order of the `-drive` options on the QEMU command line. That is why the filesystem finds the root and `/home` disks by their efs volume label rather than by number ([06](06-filesystems.md#boot-and-mounting)).

`blk_rw(disk, sector, buf, len, write)`, called in thread context (it sleeps), works on `d = &disks[disk]`; every queue, wait queue and counter below is that disk's:

```
blk_rw (any CPU, thread)                         blk_irq (cpu0, interrupt)
-----------------------------------------------  ---------------------------------
check disk < ndisks (-ENODEV), len % 512 and range
  (-EINVAL), write to read-only disk (-EROFS),
  page crossing outside the direct map (panic)
r = kzalloc(blk_req); hdr, status = 0xff
bufs = { hdr: dev reads, buf: dev writes iff
         read, status: dev writes }
r->start = rdtime; trace blk_submit
wait_event(vq.free_wq, virtq_add(...) == 0)
virtq_kick
wait_event(done_wq, r->done)  -- sleeps -->      virtio_ack_irq
                                                 while (r = virtq_get(&vq)):
                                                   latency stats; trace blk_done
                                                   r->done = true
                                                 wake_up(done_wq) if any completed
<-- woken; r->done is true
err = status == OK ? 0 : -EIO (logs "disk%d %s error at sector %lu (status %u)")
update counters, busy_ticks; kfree(r)
return err
```

Notes:

- Both waits are uninterruptible (`wait_event`), so a thread doing disk I/O cannot be killed until the device answers. `wait_what` is `"disk I/O"` during the second wait.
- Every completion wakes all threads waiting on that disk's `done_wq`; each rechecks its own `r->done`. Disks are independent: each has its own queue and IRQ.
- Many threads can have requests in flight at once (up to the 21 that fit in the queue); the device may complete them in any order, and `cookies[]` maps each completion back to its `blk_req`.
- `buf` is passed as one descriptor of `len` bytes, so the buffer must be physically contiguous. Callers use direct-map memory: the buffer cache's 4 KiB blocks and `/dev/disk0`'s page.
- No flush command is issued and `VIRTIO_BLK_F_FLUSH` is not negotiated. The filesystem's crash consistency (efs log) relies on writes being durable when they complete.

### Network RX and TX

`virtio_net_init` (from `net_init`):

1. `virtio_find(VIRTIO_DEV_NET, VIRTIO_NET_F_MAC)`; queue 0 (RX, 64) and queue 1 (TX, 64).
2. MAC from config bytes 0–5 if `VIRTIO_NET_F_MAC` was negotiated, else `02:00:00:00:00:01`.
3. Default address 10.0.2.15/24, gateway 10.0.2.2 (QEMU user networking); `net.c: parse_ip_option` may override with `ip=`.
4. `eth0.xmit = net_xmit`; post 64 receive buffers (`post_rx`); register `net_irq`; `DRIVER_OK`; kick RX; `netif_register(&eth0)`.

**Receive** (`net_irq`, interrupt context on cpu0):

1. `virtio_ack_irq`.
2. For each completed RX chain: `nb->len = len` (bytes written, including the 12-byte header), `nb_pull(nb, 12)`, `netif_rx(&eth0, nb)`. `netif_rx` (network stack) applies injected loss and queues the buffer for the `netd` thread; protocol processing never happens in the interrupt handler.
3. Post one fresh buffer per received one (`post_rx`: `nb_alloc` + `virtq_add`), then kick RX once.
4. Reap the TX queue: free every completed transmit netbuf.

If `nb_alloc` or `virtq_add` fails in `post_rx`, `rx_refill_failures++` and that slot is not refilled later: the ring permanently has one fewer posted buffer.

**Transmit** (`net_xmit`, called by the stack in the sending thread, any CPU):

1. `nb_push(nb, 12)` and zero the header (no checksum offload, no GSO).
2. `virtq_add(&txq, one descriptor, cookie = nb)`. If the ring is full, reap completed TX buffers in the caller and retry once; if still full, `tx_queue_full++`, free the netbuf and return -1 (the packet is dropped; TCP retransmits).
3. Kick TX. The netbuf is freed later, by `net_irq` or by a later `net_xmit` that finds the ring full.

One interrupt line serves both queues; the handler always checks both.

### RTC

`rtc_time_ns` (`kernel/drivers/goldfish_rtc.c`) reads `TIME_LOW` (offset 0) then `TIME_HIGH` (offset 4); reading the low word latches the high word, so the pair is consistent. The result is nanoseconds since the Unix epoch. If the device tree has no RTC it returns 0. Users: `do_clock_gettime` (`CLOCK_REALTIME`) and `do_gettimeofday` (`kernel/proc/cred.c`), and inode timestamps (`kernel/fs/efs.c`, `kernel/fs/vfs.c`). `CLOCK_MONOTONIC` uses `rdtime`, not the RTC. There is no way to set the time, and the RTC alarm/interrupt is not used.

### Device files: devfs

`kernel/fs/devfs.c` is a synthetic filesystem mounted on `/dev`. Inode numbers are the enum values; the table `devs[]` gives each a name, `struct chr_ops` and mode:

| Name | Ino | Ops | Mode | Behavior |
|---|---:|---|---|---|
| `console` | 2 | `tty_ops` | 0620 | The console TTY |
| `null` | 3 | `null_ops` | 0666 | Reads return 0; writes succeed |
| `zero` | 4 | `zero_ops` | 0666 | Reads fill zeros (256-byte chunks); writes succeed |
| `kmsg` | 5 | `kmsg_ops` | 0444 | `open` snapshots the log ring (`log_ring_copy`) into a `LOG_BUF_SIZE` buffer; reads walk the snapshot |
| `disk0` | 6 | `disk_ops` | 0440 | Read-only raw disk 0; size = `blk_capacity(0) * 512` |
| `tty` | 7 | `ctty_ops` | 0666 | The caller's controlling terminal |
| `tty0` | 8 | `tty_ops` | 0620 | Same console TTY |
| `disk1` | 9 | `disk_ops` | 0440 | Read-only raw disk 1; present only when `blk_count() > 1` |

`dev_read_inode` marks entries `I_CHR` and sets `ip->cops`; `vfs_open` copies `cops` into the file and calls `cops->open` if present (`kernel/fs/vfs.c`). `read`, `write`, `ioctl`, `poll` and `close` on a file go through `f->cops` (`kernel/fs/file.c`, `do_ioctl`).

- `/dev/tty`: `ctty_open` returns `-ENXIO` if the caller's process has no controlling terminal; otherwise it replaces `f->cops` with `tty_ops`. Its placeholder `read`/`write` exist only so `vfs_open`'s access checks pass. Kernel threads (no process) can open it.
- `/dev/disk0`, `/dev/disk1`: `disk_of(ino)` gives the disk number; `disk_rd` reads one page at a time through `blk_rw` into a temporary page, copying out the requested part. An error before any byte is transferred returns `-EIO` (also for a copy-out fault).
- `dev_readdir` lists `.`, `..` and then inodes 2 … `DEV_MAX - 1` in order, skipping disk nodes whose disk is not attached (`dev_exists`; `dev_lookup` and `dev_read_inode` use the same test), so the enum must stay dense and every entry must have a name.

### Adding a new device driver

A checklist, following the existing drivers:

1. **Describe the hardware.** For a non-virtio device, add a `struct mmio_dev` to `struct platform_info` (`kernel/include/platform.h`), a `compat_has` match in `kernel/core/fdt.c`, and printing in `platform_print` and `eu-platform` (`tools/gdb/eunuchs.py`). virtio devices are already in `platform.virtio[]`; add a `VIRTIO_DEV_*` id to `virtio.h` and a name to `device_name` in `virtio.c`.
2. **Map registers** with `ioremap(base, size)`. Remember the 32-region limit. Use `volatile` accesses.
3. **For virtio:** `virtio_find(id, wanted_features, &dev)`; read config with `virtio_config_read32`; `virtq_init` for each queue (power-of-two size ≤ 128); post any receive buffers with `virtq_add`; `irq_register`; `virtio_ready`; then `virtq_kick` queues that have buffers. Do not kick before `virtio_ready`.
4. **Buffers for DMA** must be physically contiguous. Use `kmalloc`, `page_alloc` or static data (direct map) and convert with `dma_addr`. Avoid stack buffers, or keep them within a page.
5. **Interrupt handler**: `irq_register(irq, "name", handler, arg)`. The handler runs on cpu0 with interrupts off: call `virtio_ack_irq` first, drain completions with `virtq_get` in a loop, never sleep, hand longer work to a thread (as `netif_rx` hands packets to `netd`), and `wake_up` waiters.
6. **Thread-side API**: sleep for completion with `wait_event` on a wait queue the handler wakes (set `current->wait_what` for `ps`); when `virtq_add` fails, wait on `q->free_wq` and retry, as `blk_rw` does.
7. **Call the init function** from `kmain` (if needed before threads), `kinit` (after the scheduler, before filesystems), or a subsystem init such as `net_init`. Any new `.c` in `kernel/drivers/` is compiled automatically (`KDIRS` wildcard in the `Makefile`).
8. **Expose it.** Write a `foo_print(print_fn)` and add it to `root_files[]` in `kernel/fs/procfs.c` and a command in the monitor table (`kernel/debug/monitor.c`). Add trace events to the `X(...)` list in `kernel/include/trace_events.h` with a suitable group. Use `#define LOG_SUBSYS SUBSYS_DRV` for log messages.
9. **Device file (optional).** Add an enum value before `DEV_MAX` in `kernel/fs/devfs.c` and an entry in `devs[]` with a `struct chr_ops` (`open`, `read`, `write`, `ioctl`, `poll`, `close`; the `user` flag says whether buffers are user pointers, so use `copy_to_user`/`copy_from_user` or `memcpy` accordingly). Keep the enum dense: `dev_readdir` lists every inode number up to `DEV_MAX`.
10. **QEMU options.** Attach the device in the `Makefile` (see `DISKOPTS`/`NETOPTS`), keeping `-global virtio-mmio.force-legacy=false`.

## Concurrency and locking

### Locks

| Lock | Type | Protects | Taken from |
|---|---|---|---|
| `virtqueue.lock` (`"virtqueue"`) | spinlock, irqsave | descriptor table, free list, `nfree`, avail ring, `last_used`, `cookies`, counters | `virtq_add` (threads, IRQ), `virtq_get` (IRQ, `net_xmit`) |
| `console_tty.lock` (`"tty"`) | spinlock, irqsave | `t`, `line`, `q`, `delims` | `tty_input` (IRQ), `tty_read`, `TCSETS*` |
| `log_lock` | spinlock, irqsave | console output ordering | `console_write_locked` (TTY echo and writes), printk |
| `proc_lock` | spinlock | process group membership and signal state | `signal_send_pgrp` from `tty_input` (after the TTY lock is dropped) |
| `kpt_lock` | spinlock | kernel page table | `vm_map` via `ioremap` after `vm_init` |

Lock order observed in this code: `tty` → `log_lock` (echo and erase output happen under the TTY lock). The TTY lock is released before `signal_send_pgrp` (`proc_lock`) and before `wake_up`. `virtqueue.lock` is released before `wake_up(&q->free_wq)`. No driver lock is held across a sleep.

### Interrupt context rules

- All device handlers (`uart_irq`, `blk_irq`, `net_irq`) run on cpu0 with interrupts disabled, one at a time. They must not sleep. They do allocate memory (`nb_alloc` in `post_rx`) and send signals (`tty_input`); the slab and `proc_lock` are interrupt-safe.
- Because every lock that an IRQ handler takes is taken with `spin_lock_irqsave` elsewhere, a thread on cpu0 holding such a lock cannot be interrupted by a handler that wants it. A thread on another CPU may hold it; the handler then spins briefly.
- `blk_irq` writes `r->done` and the latency counters without a lock. The submitting thread reads `r->done` through `wait_event`, which rechecks after every wakeup; `wake_up` provides the needed ordering.

### SMP

- Threads on any CPU can call `blk_rw` and `net_xmit` concurrently; the virtqueue lock serializes ring updates against each other and against the handler on cpu0.
- `net_xmit` and `net_irq` both reap the TX queue; the lock makes that safe.
- Several counters are updated without locks or atomics from multiple CPUs: `n_reads`, `n_writes`, `sectors_*`, `n_errors`, `busy_ticks` in `blk_rw`; `tty->sigs`, `tty->out`; `rx_refill_failures`, `tx_queue_full`. They are statistics only and can lose increments.
- Some TTY state is read without the lock: `readable()` in `wait_event_interruptible` and `tty_poll` (rechecked under the lock in `tty_read`), `tty->pgrp` and `tty->sid` in `job_control`, `TIOCGPGRP`, `TIOCSPGRP`, `TIOCSCTTY`, `FIONREAD`, and `TCGETS`. A concurrent `TCSETS` can make `TCGETS` return a torn struct.
- `ioremap` updates its region table without a lock. It is called at boot, by `virtio_print` (monitor or `/proc/virtio`), and once lazily by `rtc_time_ns`, which may run concurrently on several CPUs the first time.
- `irq_register` does a read-modify-write of the PLIC enable register without a lock; all registrations happen during single-threaded init.

## Interfaces

### Kernel functions

| Function | File | Used by |
|---|---|---|
| `void *ioremap(uintptr_t pa, size_t size)` | `mm/vmm.c` | every driver |
| `uintptr_t dma_addr(const void *va)` | `drivers/virtio.c` | virtio drivers |
| `int irq_register(u32 irq, const char *name, irq_handler_t, void *arg)` | `arch/riscv/plic.c` | drivers |
| `void plic_init(void)`, `void plic_handle(void)` | `arch/riscv/plic.c` | `kmain`, `trap.c` |
| `int virtio_find(u32 id, u64 want, struct virtio_dev *)` | `drivers/virtio.c` | virtio drivers |
| `int virtq_init(dev, q, index, num)`, `void virtio_ready(dev)` | `drivers/virtio.c` | virtio drivers |
| `int virtq_add(q, bufs, n, cookie)`, `void virtq_kick(dev, q)`, `void *virtq_get(q, &len)` | `drivers/virtio.c` | virtio drivers |
| `u32 virtio_ack_irq(dev)`, `u32 virtio_config_read32(dev, off)` | `drivers/virtio.c` | virtio drivers |
| `int blk_init(void)` (returns the number of disks), `int blk_count(void)`, `u64 blk_capacity(int disk)` (sectors; 0 for a bad number) | `drivers/virtio_blk.c` | `kinit`, `vfs_init`, devfs |
| `int blk_rw(int disk, u64 sector, void *buf, u32 len, bool write)` | `drivers/virtio_blk.c` | `fs/bcache.c`, devfs |
| `int virtio_find_from(int from, u32 id, u64 want, struct virtio_dev *dev)` | `drivers/virtio.c` | `blk_init` (`virtio_find` is the `from = 0` form) |
| `int virtio_net_init(void)` | `drivers/virtio_net.c` | `net/net.c: net_init` |
| `void console_init/putc/write`, `console_write_locked`, `console_getc[_nowait]` | `core/console.c`, `core/printk.c` | printk, monitor, TTY |
| `void console_enable_irq(void)`, `console_rx_char(char)` | `core/console.c` | `kmain`, UART IRQ |
| `void tty_init(void)`, `void tty_input(char)` | `drivers/tty.c` | `kmain`, console |
| `void tty_set_ctty(struct proc *)`, `struct tty *tty_console(void)` | `drivers/tty.c` | `proc/proc.c` |
| `const struct chr_ops tty_ops` | `drivers/tty.c` | devfs |
| `u64 rtc_time_ns(void)` | `drivers/goldfish_rtc.c` | time syscalls, efs, VFS |
| `blk_print`, `virtio_print`, `tty_print`, `irq_print` | various | monitor, procfs |

### System calls

No device-specific system calls. Devices are reached through `open`/`read`/`write`/`ioctl`/`ppoll`/`close` on `/dev` files. `ioctl` on a file without `cops->ioctl` returns `-ENOTTY` (`do_ioctl`), which is how `isatty` fails on pipes. The TTY ioctls are listed above. `clock_gettime(CLOCK_REALTIME)` and `gettimeofday` read the RTC.

### /proc files

| File | Generator | Contents |
|---|---|---|
| `/proc/interrupts` | `irq_print` | per-CPU timer/IPI/external counts; per-PLIC-source count, time (µs), handler symbol; spurious claims |
| `/proc/blk` | `blk_print` | one block per disk (`diskN: virtio-blk ...`): capacity, queue size and free descriptors, IRQ, reads/writes (count and KiB), errors, average and max latency, in-flight count |
| `/proc/virtio` | `virtio_print` | every virtio slot: base, IRQ, device type, version, status register |
| `/proc/tty` | `tty_print` | session, foreground group, size, termios flags, line being edited, queued entries and complete lines, in/out/dropped/signals |

### Monitor commands

| Command | Shows |
|---|---|
| `irq` | `irq_print` |
| `blk` | `blk_print` and `virtio_print` |
| `tty` | `tty_print` |
| `plat`, `dtb` | platform summary and device tree (where devices are) |
| `traps` | exception/interrupt counters |
| `net` | interfaces (including `eth0` statistics kept by the stack) |

### Boot options and build settings

- No driver-specific boot options. `log=drv:<level>` controls messages from `virtio.c`, `virtio_net.c`, `tty.c` and `plic.c` (`SUBSYS_DRV`); `log=blk:<level>` controls `virtio_blk.c`. `trace=blk` and `trace=irq` select the related trace groups. `ip=` (network stack) overrides `eth0`'s address. `break` enters the monitor before interrupts are set up.
- QEMU must run with `-global virtio-mmio.force-legacy=false` (the Makefile's `DISKOPTS` sets it); each disk is a `-drive ... -device virtio-blk-device` pair (`make run` attaches `build/fs.img` and `home.img`; `HOMEOPTS`) and the NIC `-device virtio-net-device` (`Makefile`).

## Debugging and observability

**First look.** `irq` (or `cat /proc/interrupts`) shows whether a device's interrupts arrive at all and how long its handler takes. `blk` shows I/O counts, latency and the in-flight count, plus the status register of every virtio slot. `tty` shows the termios flags, who owns the terminal, and what input is queued.

**Boot log.** Each driver logs one line on success: `virtio%d: <type> device at ... irq ..., features ...`, `irq N -> name`, `diskN: M MiB (...)` per disk, `eth0: virtio-net, MAC ...`, `PLIC at ..., hart H S-mode context C`. `dmesg`, `/dev/kmsg` or GDB `eu-dmesg` show them. `eu-platform` in GDB shows the device addresses and IRQs from the device tree.

**Trace events.**

| Event | Group | Fields |
|---|---|---|
| `irq_entry`, `irq_exit` | IRQ | cause; `irq_exit` also has the last PLIC source |
| `blk_submit` | BLK | sector, length, write |
| `blk_done` | BLK | sector, status, latency µs |
| `tty_signal` | PROC | signal, foreground group |

View with `trace show` in the monitor, `eu-trace` in GDB, or a Perfetto export (`trace dump`). IRQ slices appear on each CPU's track.

**Sleeping threads.** A thread blocked in the TTY shows `wait_what` `"tty read"`; one blocked on the disk shows `"disk I/O"` (`ps`/`sched` in the monitor). Wait queue names: `tty-read`, `blk-io`, `virtq-desc`.

**Typical messages and failures.**

| Message | Meaning |
|---|---|
| `virtio%d: legacy (version 1) interface; run QEMU with -global virtio-mmio.force-legacy=false` | QEMU presented the legacy transport |
| `virtio%d: device does not offer VIRTIO_F_VERSION_1` / `device rejected features` | Handshake failed; status set to FAILED |
| `no virtio block device` then panic `no disk: ...` | No `-drive`/`virtio-blk-device` on the QEMU command line |
| `diskN: M MiB (S sectors), queue of Q descriptors` | One line per disk found, numbered in discovery order |
| `virtio-blk: cannot set up the request queue` / `virtio-net: cannot set up the queues` | `virtq_init` failed (queue already ready, max 0, or no memory) |
| `no virtio network device (make run attaches one)` | Informational; networking runs on `lo` only |
| `blk_rw: bad request diskN sector ... len ...` | Caller passed a length not a multiple of 512 or beyond the disk |
| panic `blk_rw: buffer %p crosses a page outside the direct map` | A kernel-stack buffer was used for disk I/O and crosses a page |
| panic `dma_addr: %p is not mapped` | A DMA buffer address is not mapped in the kernel page table |
| `diskD read/write error at sector N (status S)` | Device returned a non-OK status (`IOERR` 1, `UNSUPP` 2), or `0xff` meaning it never wrote it |
| `virtqueue %u: completion for descriptor %u with no request` | Ring corruption or a double completion |
| `irq_register(%u, %s) failed` | No PLIC, IRQ out of range (≥ 64) or already taken |
| `spurious PLIC claims: N` (non-zero) | A source fired with no handler registered |
| panic `ioremap: too many regions` | More than 32 distinct MMIO ranges |

**Interrupt never arrives.** Check `irq` counts. If zero: check `plat` for the IRQ number, that `irq N -> name` appeared at boot, and the device's status in `/proc/virtio` (should include `DRIVER_OK`, 0x4, and `FEATURES_OK`, 0x8; typically 0xf). If `blk` shows requests in flight while the count does not move, the device did not complete them: check the descriptors. With GDB, `vq` in `virtio_blk.c` holds the ring pointers; `vq.used->idx` vs. `vq.last_used` tells whether completions are pending but not consumed.

**Terminal problems.** "Input does nothing": `tty` shows whether bytes are arriving (`in`), whether a partial line is being edited, and the foreground group; a program that is not in the foreground group gets SIGTTIN. "Characters echo twice/not at all": check `echo` and `icanon` in `tty`. Programs that exit without restoring raw mode leave the terminal raw; the shell restores its saved modes when a job ends (`esh`, `term_take`).

**Tests.** `user/bin/utests.c: t_tty` checks `isatty`, `tcgetattr`/`tcsetattr`, `tcgetpgrp`, opening `/dev/tty`, and SIGTTIN for a background reader. Disk and network drivers are exercised indirectly by every filesystem and network test.

## Limitations and known issues

**General**

- One UART TTY, up to four virtio-blk devices (only disks 0 and 1 get `/dev` nodes), the first virtio-net device. `blk_init` stops scanning at the first block device that fails the handshake (for example a legacy one), so disks after it are not found. No hotplug, no driver unload, no device reset after errors.
- Driver initialization is hard-coded in `kmain`, `kinit` and `net_init`; there is no driver registry or device-tree match table for drivers.
- All device interrupts go to cpu0. Under heavy I/O, cpu0 does all interrupt work.
- The PLIC context is computed as `2 * boot_hart + 1`, which matches QEMU `virt` but is not read from the device tree's `interrupts-extended`.
- Handler time in `/proc/interrupts` for the UART includes time spent in the monitor when it was entered with `Ctrl-]` (the monitor runs inside `uart_irq`).

**virtio**

- Queues are capped at 128 descriptors (one page). No indirect descriptors, no `EVENT_IDX`, no notification or interrupt suppression: every submission kicks the device and every completion interrupts.
- `virtio_find` does not wait for the reset to read back 0, and config-space reads do not use the config generation counter (`capacity` is read as two 32-bit halves).
- `virtq_get` has no barrier between reading `used->idx` and reading the ring entry (the fence comes before the index read). On a weakly ordered implementation the entry could be read stale. QEMU TCG does not expose this.
- `virtq_init` does not check `kzalloc` for `cookies`, and leaks the ring page on later failure paths.
- Configuration-change interrupts are acknowledged and ignored.

**Block**

- Requests are a single data segment; there is no scatter-gather and no check of the device's `seg_max`/`size_max`.
- No `FLUSH` command; durability depends on the device completing writes durably.
- All waiters share `done_wq`, so each completion wakes every waiting thread.
- Disk waits are uninterruptible; a hung device hangs the calling threads in D state.
- Error handling is reporting only: no retries, and `/dev/disk0` is read-only.

**Network**

- No offloads (checksum, GSO), no `MRG_RXBUF`, no multiqueue, no link status.
- An RX refill failure permanently shrinks the RX ring; nothing retries. `rx_refill_failures` and `tx_queue_full` are not reported anywhere.
- TX completions are reaped only from the interrupt handler or when the ring is full.

**TTY**

- Only the console TTY exists; no pseudo-terminals, so `rshd` sessions do not get a terminal.
- `VTIME` is ignored; `VMIN` > 1 behaves like 1. `O_NONBLOCK` is ignored by `tty_read` (only raw mode with `VMIN` = 0 gives non-blocking reads).
- Output processing flags are ignored: `\n` always becomes `\r\n`, even with `OPOST` cleared.
- `IXON` (`^S`/`^Q`), `ECHOK`, `IEXTEN` gating of `^W`, `CREAD`, and `_POSIX_VDISABLE` (a `c_cc` value of 0 disabling a character) are not implemented. Setting a control character to 0 makes NUL bytes trigger it. `'\b'` always erases.
- The window size is whatever was last set with `TIOCSWINSZ` (24 × 80 at boot); the serial line can't report resizes, and there is no `SIGWINCH`.
- No `SIGTTOU`: background jobs may write and change termios or the foreground group freely. `TIOCSPGRP` does not check that the group exists in the session; `TIOCSCTTY` takes the terminal from another session without checking. No `SIGHUP` on session leader exit.
- Erasing a tab rubs out one column.
- Console output is polled with interrupts off under `log_lock`, 256 bytes per chunk; a large write delays interrupts on that CPU for the time it takes the UART to accept 256 bytes.
- `Ctrl-]` cannot be passed through to programs.

**RTC and devfs**

- The RTC is read-only; no `settimeofday`/`clock_settime` path, no alarm.
- `rtc_time_ns` maps its page lazily without a lock.
- `/dev/disk0` returns `-EIO` for a copy-out fault before any data is transferred, instead of `-EFAULT`. `/dev/console`, `/dev/tty0` and `/dev/tty` are all the same device. The devfs header comment lists only five devices.

## Potential improvements

1. **Implement `VMIN`/`VTIME` and `O_NONBLOCK` for the TTY** — Wait until `VMIN` bytes are available, use `VTIME` as an inter-byte or overall timeout per POSIX, and return `-EAGAIN` for non-blocking files. Editors and serial tools depend on this. Impact: medium. Effort: medium.
2. **Retry RX refills and expose driver counters** — When `post_rx` fails, remember the deficit and refill from the next interrupt or from `netd`; print `rx_refill_failures`, `tx_queue_full`, posted RX buffers and TX in-flight in `/proc/net` or the `net` command. Today a burst of allocation failures silently and permanently starves receive. Impact: medium. Effort: small.
3. **Add a read barrier in `virtq_get` and use the config generation counter** — Fence after reading `used->idx` and before reading the element; read 64-bit config fields under ConfigGeneration (0x0fc); wait for Status to read 0 after reset. Cheap spec compliance that matters on real hardware or other hypervisors. Impact: medium. Effort: small.
4. **Per-request completion for block I/O** — Give each `blk_req` its own wait entry (or wake only the owner thread) instead of waking every waiter on `done_wq`. Reduces wakeups and context switches under concurrent I/O. Impact: medium. Effort: small.
5. **Negotiate `VIRTIO_BLK_F_FLUSH` and issue flushes at log commit** — The efs log's crash guarantees assume durable completed writes; a flush after the commit block makes that explicit and holds with write-back caches. Impact: medium. Effort: small.
6. **Implement `SIGTTOU`, validate `TIOCSPGRP`/`TIOCSCTTY`** — Stop background writers when `TOSTOP` is set and background callers of `TCSETS*`/`TIOCSPGRP`; check the target group belongs to the session; refuse to steal a terminal owned by another session. Completes job control. Impact: medium. Effort: small.
7. **Honor `OPOST`/`ONLCR` and add `SIGWINCH`** — Move `\n` → `\r\n` translation from `console_putc` into the TTY output path (printk keeps its own), and send `SIGWINCH` to the foreground group when `TIOCSWINSZ` changes the size, so full-screen programs can adapt. Impact: low. Effort: small.
8. **Buffered, interrupt-driven UART output for the TTY** — Queue user output and drain it on the THR-empty interrupt, keeping polled output for printk and panics. Removes long interrupts-off windows during large writes. Impact: medium. Effort: medium.
9. **Unit tests for the line discipline** — `KTEST`s that feed `tty_input` character sequences (erase, kill, word erase, `^D` placement, mode switches, queue overflow) and check `tty_read` results. The line accounting bug fixed earlier (H5 in IMPROVEMENTS.md) is the kind of thing they would catch. Impact: medium. Effort: small.
10. **Notification suppression and TX batching** — Negotiate `VIRTIO_F_EVENT_IDX` or honor `used->flags`, and avoid a kick per packet; reap TX completions without an interrupt per frame. Fewer VM exits under load. Impact: low. Effort: medium.
11. **Scatter-gather and indirect descriptors for block requests** — Allow multi-segment buffers (respecting `seg_max`/`size_max`) so larger I/O and non-contiguous buffers (kernel stacks, user pages) work without the page-crossing panic. Impact: low. Effort: medium.
12. **`/dev` nodes for every disk** — Generate `diskN` entries from `blk_count()` instead of the two fixed enum values, so disks 2 and 3 (which the driver already supports) can be read. Impact: low. Effort: small.
13. **A small driver registry** — A table of `{ virtio device id or DT compatible, init function }` walked at boot, plus a common place to register `/proc` and monitor printers, instead of hard-coded calls in `kinit`/`net_init`. Makes adding drivers a one-file change. Impact: low. Effort: medium.
14. **Implement `IXON`, `_POSIX_VDISABLE` and `IEXTEN` gating** — Treat a `c_cc` value of 0 as disabled, gate `^W` on `IEXTEN`, and implement `^S`/`^Q` or clear `IXON` from the defaults so `stty`-style output is truthful. Impact: low. Effort: small.
15. **Thread-safe `ioremap` and eager RTC mapping** — Take `kpt_lock` around the region table in `ioremap` and map the RTC at boot, so no MMIO mapping happens lazily on a hot path. Impact: low. Effort: small.
