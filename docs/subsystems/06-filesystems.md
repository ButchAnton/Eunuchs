# Files and filesystems

This subsystem turns one or more virtio disks into a Unix file tree. Three filesystem types sit under a small VFS: efs, an on-disk filesystem with a write-ahead log in the xv6 style; devfs, mounted on `/dev`; and procfs, a read-only view of kernel state on `/proc`. It also provides the file objects and per-process descriptor tables behind every `read`/`write`, plus pipes and `ppoll`. A host tool (`tools/mkfs/mkfs.c`) builds efs images and checks them (fsck).

## Responsibilities and scope

**In scope:**

- **Buffer cache** (`bcache.c`): 128 cached 4 KiB disk blocks, keyed by (disk, block).
- **Write-ahead log** (`log.c`): makes every efs change atomic across power failures, with crash-point injection for testing.
- **efs** (`efs.c`): block and inode allocation, the block map (direct, indirect and double-indirect blocks), file read/write/truncate, and directories.
- **VFS** (`vfs.c`): the inode table, mounts (choosing the root and `/home` disks by volume label), path lookup, permission checks, and the namespace operations (create, mkdir, unlink, link, rename, getcwd).
- **Files and descriptors** (`file.c`): `struct file`, 32-slot descriptor tables, and the file system calls.
- **Pipes** (`pipe.c`), **`ppoll`** (`poll.c`), **devfs** (`devfs.c`) and **procfs** (`procfs.c`).
- **Host tools**: `mkfs`/`fsck` (`tools/mkfs/mkfs.c`) and their host test (`tests/host/test_fsck.py`).

**Out of scope (owned by other subsystems):**

- The block driver. `blk_rw`, `blk_count`, `blk_capacity` and `blk_print` are in `kernel/drivers/virtio_blk.c`; disks are numbered 0..`blk_count()-1` in the order the driver finds them. The filesystem calls `blk_rw` synchronously: the caller sleeps until the request completes.
- The TTY line discipline (`tty_ops`, `kernel/drivers/tty.c`). devfs only exposes it as `/dev/console`, `/dev/tty0` and `/dev/tty`.
- Sockets (`kernel/net/socket.c`). They are `struct file`s with their own `chr_ops`, and they plug into `ppoll` through `chr_ops.poll`.
- Credentials and the `chmod`/`chown`/`access`/`utimensat` system calls. These live in `kernel/proc/cred.c`, but they use `vfs_permission`, `namei` and `write_inode` from this subsystem.
- `exec`, which reads a whole program with `vfs_read_all` (`kernel/proc/proc.c`).
- The generators behind most `/proc` files (`pmm_print`, `tcp_print`, ...). procfs only calls them.

## Source map

| File | Role |
|---|---|
| `kernel/include/fs.h` | All in-kernel FS types (`buf`, `inode`, `file`, `super_block`, `fs_ops`, `chr_ops`, `kdirent`) and prototypes; documents the lock order. |
| `kernel/include/efs_format.h` | On-disk efs format, shared by the kernel and the host tool (only `<stdint.h>` types). |
| `lib/include/uapi/fs.h` | ABI: `O_*`, `F_*`, `S_*`, `struct stat` (with `st_mtim`/`st_ctim` timespecs), `UTIME_NOW`/`UTIME_OMIT`, `struct dirent`, `R_OK..`, `PATH_MAX` (256), `NAME_MAX` (59). |
| `kernel/fs/bcache.c` | Buffer cache: `bread`, `bwrite`, `brelse`, `bpin`/`bunpin`, LRU, statistics. |
| `kernel/fs/log.c` | Write-ahead log: `log_begin`/`log_end`/`log_write`, `commit`, `install`, recovery in `log_create`, crash points. |
| `kernel/fs/efs.c` | efs `fs_ops`: `balloc`/`bfree`, inode read/write/alloc/free, `bmap`, `efs_truncate`, `efs_read`/`efs_write`, directories, `efs_mount`, `efs_label`. |
| `kernel/fs/vfs.c` | Inode cache (`iget`/`iput`/`ilock`), `vfs_mount`/`vfs_mount_disk`, `disk_labeled`, `vfs_permission`, `walk`/`namei`/`nameiparent`, `vfs_open`, `vfs_mkdir`, `vfs_unlink`, `vfs_link`, `vfs_rename`, `vfs_getcwd`, `vfs_read_all`, `vfs_init`, `fs_error`. |
| `kernel/fs/file.c` | `file_read`/`file_write`/`file_close`, descriptor tables (`fd_install`, `files_fork`, `files_exec`, `files_close_all`), file system calls `do_*`. |
| `kernel/fs/pipe.c` | 4 KiB ring pipes, `pipe_create`, `/proc/pipes`. |
| `kernel/fs/poll.c` | `poll_wait`, `file_poll`, `do_ppoll`. |
| `kernel/fs/devfs.c` | `/dev`: `console`, `null`, `zero`, `kmsg`, `disk0`, `disk1`, `tty`, `tty0`. |
| `kernel/fs/procfs.c` | `/proc`: global files, `/proc/net/*`, `/proc/<pid>/*`, `/proc/self`, `seq_print`. |
| `kernel/proc/cred.c` | `do_fchmod`, `do_fchmodat`, `do_fchownat`, `do_faccessat` (FS-adjacent). |
| `tools/mkfs/mkfs.c` | Host `mkfs [--label NAME] IMAGE SIZE_MB specs...` and `mkfs --check IMAGE` (fsck). |
| `tests/host/test_fsck.py` | Injects seven kinds of corruption into an image and checks that fsck reports each (plus a clean fresh image). |
| `kernel/tests/test_fs.c` | KTESTs: leak checks, sparse files and truncate, path walk, error and free-count consistency. |
| `tools/run_tests.py`, `user/bin/utests.c: scenario` | Multi-boot persistence and power-failure scenarios. |

## Concepts and data structures

### On-disk format (efs)

All sizes come from `efs_format.h`. A block is `EFS_BSIZE` = 4096 bytes, which is 8 sectors (`EFS_SECTORS_PER_BLOCK`). Block numbers are `uint32_t`, and block 0 never appears in a pointer, so 0 means "no block".

**Layout.** `tools/mkfs/mkfs.c: do_mkfs` decides it:

| Region | Blocks | Default image (`FS_MB=32`, 8192 blocks) |
|---|---|---|
| boot block (unused) | 1 | 0 |
| superblock | 1 | 1 |
| log header | 1 | 2 (`logstart`, always 2) |
| log data | `EFS_LOGSIZE` = 64 | 3–66 |
| inode table | ⌈`ninodes` / `EFS_IPB`⌉ = 1024/32 = 32 | 67–98 (`inodestart`) |
| free bitmap | ⌈`size` / 32768⌉ | 99 (`bmapstart`) |
| data | the rest | 100–8191 (`datastart`), 8092 blocks |

`nlog` = `EFS_LOGSIZE + 1` = 65 (header included). mkfs always uses `ninodes` = 1024. One bitmap block covers 32768 blocks (128 MiB). The bitmap has one bit per block of the whole disk, so mkfs marks metadata blocks 0..`datastart-1` as used.

**`struct efs_super`** (64 bytes, in block 1): `magic` = `EFS_MAGIC` 0x53465545 ("EUFS"), `version` = 1, `bsize`, `size` (total blocks), `ninodes`, `nlog`, `logstart`, `inodestart`, `bmapstart`, `datastart`, `mkfs_time` (ns since the epoch), and `label[16]`, the volume label (`root`, `home`; NUL-padded, not necessarily NUL-terminated when all 16 bytes are used). Images made before the label existed have zeros there, which reads as an empty label; the version was not bumped.

**`struct efs_dinode`** (128 bytes, `_Static_assert`ed; `EFS_IPB` = 32 per block). Inode `i` is at block `inodestart + i/32`, slot `i%32`.

| Offset | Field | Notes |
|---:|---|---|
| 0 | `u16 type` | `EFS_T_FREE` 0, `EFS_T_DIR` 1, `EFS_T_FILE` 2 |
| 2 | `u16 nlink` | directory entries naming it (directories count `.` and each child's `..`) |
| 4 | `u16 mode` | permission bits only (07777); the type comes from `type` |
| 6 | `u16 uid`, 8 `u16 gid` | 16-bit ids |
| 10 | `u16 reserved0` | |
| 12 | `u32 flags` | unused by the kernel |
| 16 | `u64 size` | bytes |
| 24 | `u64 mtime`, 32 `u64 ctime` | ns since the epoch (`rtc_time_ns`) |
| 40 | `u32 addrs[14]` | 12 direct, `[12]` single-indirect, `[13]` double-indirect |
| 96 | `u8 reserved[32]` | |

Inode 0 is never used, because a directory entry with `ino == 0` marks a free slot. The root directory is inode 1 (`EFS_ROOT_INO`).

**Block map** (`kernel/fs/efs.c: bmap`; mirrored in `mkfs.c: bmap` and `file_block`). An indirect block holds `EFS_NINDIRECT` = 1024 `u32` pointers.

```
file block bn            where
0 .. 11                  addrs[bn]                                      (48 KiB)
12 .. 1035               ind = addrs[12];  ind[bn-12]                   (+4 MiB)
1036 .. 1036+1048575     dbl = addrs[13];  dbl[(bn-1036)/1024][(bn-1036)%1024]  (+4 GiB)
```

`EFS_MAXFILE` = (12 + 1024 + 1024²) × 4096 = 4,299,210,752 bytes. In practice the disk is the limit: 8092 data blocks in the default image. A pointer of 0 inside the map is a hole, which reads as zeros (`efs_read` uses a static zero block).

**Directories** are ordinary files made of `struct efs_dirent` records (64 bytes, `EFS_DPB` = 64 per block): `u32 ino` and `char name[60]` (`EFS_NAMELEN` = 59 plus NUL). `ino == 0` is a free slot. Every directory has `.` and `..`, and the root's `..` points to itself. Entries are never compacted, so a directory never shrinks.

**`struct efs_logheader`** (272 bytes, in block `logstart`): `magic` = `EFS_LOG_MAGIC` 0x474f4c45 ("ELOG"), `n` (committed blocks; 0 means the log is empty), `u64 seq`, and `u32 blocks[64]`, the home block number of each log data block. The log data blocks follow the header in order: log data block `i` is at `logstart + 1 + i`.

### Buffer cache (`struct buf`, `bcache.c`)

| Field | Meaning |
|---|---|
| `disk`, `blockno` | which disk (`blk_rw` numbering) and which 4 KiB block on it is cached here |
| `valid` | `data` holds the block's contents |
| `refcnt` | holders (protected by `bc_lock`) |
| `pins` | held by the log until installed; a pinned buffer is never recycled |
| `lock` | a sleeping mutex; `bread` returns the buffer locked |
| `lru` | position in the global LRU list (most recently used first) |
| `data` | one zeroed page from `page_alloc_zeroed` |
| `get_site`, `last_used` | debugging: return address of the last `bread`, `rdtime` of the last release |

`NBUF` = 128, so the cache holds 512 KiB, shared by every disk. Every efs read and write goes through it, including the log's own blocks.

### The log (`struct efs_log`, private to `log.c`)

| Field | Meaning |
|---|---|
| `lock` | spinlock over the fields below |
| `wq` | `"log-space"` wait queue for `log_begin` |
| `disk` | the disk of the filesystem this log belongs to (`sb->disk`) |
| `start` | header block number (`esb->logstart`) |
| `outstanding` | operations between `log_begin` and `log_end` (outermost level only) |
| `committing` | a commit is in progress; no new operation may start |
| `n`, `blocks[64]` | distinct blocks modified by the current transaction, with their home numbers |
| `seq` | next transaction number |
| `crash_at` | armed crash point (`fs.crash=` or `efs_arm_crash`) |
| `commits, logged, absorbed, recovered, max_n, commit_ticks` | statistics for `log_print` |

`MAXOPBLOCKS` = 16 is the space reserved for each operation. `file.c` sizes `WRITE_CHUNK` (4 blocks = 16 KiB per transaction) to fit inside it. Each thread carries `fs_op_depth` (`kernel/include/sched.h`) so operations can nest.

### VFS objects (`fs.h`)

- **`struct super_block`**: one per mount.
  - `type` and `ops`.
  - `root`: the root inode of this filesystem.
  - `covered`: the directory it is mounted on, `NULL` for `/`.
  - `ns_lock`: a mutex that serializes all namespace changes.
  - `path[32]`, `priv` (efs: `struct efs_info`), and `errors`, which counts `fs_error` reports.
- **`struct inode`**: one in-memory copy per (sb, ino) while referenced.
  - `refcnt` is protected by `icache_lock`. Everything else is protected by `lock`, a mutex.
  - `valid` is set by the first `ilock`, which calls `read_inode`.
  - Generic copies of the inode fields: `type` (`enum itype`: `I_FREE`, `I_DIR`, `I_FILE`, `I_CHR`), `nlink`, `mode`, `uid`, `gid`, `size`, `mtime`, `ctime`, `rdev`.
  - `cops`: device or synthetic-file hooks.
  - `mounted`: the filesystem mounted on this directory.
  - `u.efs`: a full copy of the `efs_dinode`, including `addrs`.
- **`struct efs_info`** (`efs.c`): a copy of the superblock, the log pointer, `bhint` (the next place to look for a free block), and the in-memory counters `nfree_blocks` and `nfree_inodes`. It also has an `alloc_lock`, which is initialized but never taken (see Limitations).
- **`struct fs_ops`**: `read_inode`, `write_inode`, `alloc_inode`, `free_inode`, `read`, `write`, `truncate`, `lookup`, `link`, `unlink`, `readdir`, `begin_op`, `end_op` and `stat_fs`. Any of them may be `NULL`. devfs and procfs provide only `read_inode`, `lookup` and `readdir`. Because they have no `alloc_inode`, `link`, `unlink` or `write_inode`, the VFS returns `EPERM` for creates, removals, chmod and chown on them.
- **`struct chr_ops`**: `open`, `read`, `write`, `close`, `ioctl` and `poll`. They are used for devices, procfs files, pipes and sockets. When `f->cops->read` or `write` is set, `file_read`/`file_write` call it instead of `fs_ops`.

**The inode "cache" is not a cache.** `iput` frees the in-memory inode as soon as `refcnt` reaches 0 (`kernel/fs/vfs.c: iput`). Every later `iget` + `ilock` therefore re-reads the dinode, which usually comes from the buffer cache. The `icache` list holds exactly the inodes that are referenced (`icache_busy` counts them for leak tests).

### Files and descriptors

**`struct file`** (`fs.h`):
- `refcnt`, protected by `file_lock` in `file.c`;
- `ip` (`NULL` for pipes and sockets);
- `off`, `flags` (the `O_*` flags given to open), `readable`, `writable`;
- `cops`;
- `priv` and `priv_len`: the procfs or kmsg snapshot, or the `struct pipe`;
- `path[64]`, for debugging. Pipes use `"pipe:[n] read"` or `"pipe:[n] write"`.

**Descriptor table**: `struct proc` has `files[32]` (`NOFILE`), `fd_flags[32]` (only `FD_CLOEXEC`) and `cwd` (`kernel/include/proc.h`). Several descriptors and processes can share one `struct file`, and with it the offset (`dup`, `fork`).

### Pipes

**`struct pipe`** (`pipe.c`):
- `lock` (a mutex);
- `buf`: one page, `PIPE_SIZE` = 4096 bytes;
- `rpos` and `wpos`: monotonic counters, so the number of buffered bytes is `wpos - rpos`;
- `readers` and `writers`: open ends;
- `ends`: the struct files still using the pipe. It is decremented atomically, and the last closer frees the pipe;
- `rwait` and `wwait`: wait queues;
- `id`, `total`, and a list node for `/proc/pipes`.

### procfs inode numbering

| Range | Object |
|---|---|
| 1 | `/proc` |
| 2 .. 2+NROOT-1 | global files in `root_files[]` order: `meminfo slabinfo interrupts threads sched procs mounts bcache blk icache trace virtio uptime pipes tty cpus lockdep profile` (18) |
| `NET_DIR` = 0x80000 | `/proc/net` |
| 0x80001..0x80005 | `dev arp tcp udp stats` |
| `PID_BASE` + pid×8 (`PID_BASE` = 0x100000) | `/proc/<pid>` |
| … + 1..6 | `status maps fds stack ustack cmdline` |

### devfs inode numbering

| ino | name | ops | mode |
|---:|---|---|---|
| 1 | `/dev` | — | 0755 |
| 2 | `console` | `tty_ops` | 0620 |
| 3 | `null` | read: EOF, write: sink | 0666 |
| 4 | `zero` | read: zeros, write: sink | 0666 |
| 5 | `kmsg` | snapshot of the log ring | 0444 |
| 6 | `disk0` | raw disk 0, read-only | 0440 |
| 7 | `tty` | controlling terminal (`ctty_open` swaps in `tty_ops`) | 0666 |
| 8 | `tty0` | `tty_ops` | 0620 |
| 9 | `disk1` | raw disk 1, read-only; exists only if a second disk is attached | 0440 |

All devfs inodes have uid and gid 0 (they are `kzalloc`ed and never set). `rdev` equals the inode number.

## How it works

### Boot and mounting

`kinit` calls `blk_init`, which finds every virtio disk (up to `MAX_DISKS` = 4; with none, `kinit` panics), then `vfs_init` (`kernel/fs/vfs.c`):

1. `bcache_init` allocates 128 zeroed pages, links them on the LRU, and logs `buffer cache: 128 buffers of 4096 bytes`.
2. It chooses the root disk: `disk_labeled("root")` reads the superblock of each disk in turn (`efs_label`, through the buffer cache) and returns the first whose efs volume label is `root`. If none is, the root is disk 0, so an unlabelled image attached alone still boots.
3. `vfs_mount_disk(&efs_type, "/", root)` records the disk in `sb->disk` and calls `efs_mount`:
   - It reads block 1 of that disk and checks `magic`, `bsize` and `version`; on a mismatch it fails with `-EINVAL`, and `vfs_init` panics with "cannot mount the root filesystem".
   - It sets `bhint = datastart`.
   - It calls `log_create`, which **replays a committed log** (see Recovery).
   - It calls `iget(sb, 1)` for the root.
   - It counts free blocks by scanning the bitmap (`efs_free_blocks`) and free inodes by scanning the inode table.
4. If some other disk is labelled `home`, it is mounted on `/home` the same way. A failure only warns (`/home (disk1) not mounted: ...`), and the root filesystem's own `/home` directory shows through. The boot log names the disk: `mounted efs (disk1) on /home`.
5. `vfs_mount(&devfs_type, "/dev")` and `vfs_mount(&procfs_type, "/proc")` mount the synthetic filesystems (`vfs_mount` is `vfs_mount_disk` with disk 0, which they ignore).
   - `vfs_mount` resolves the mount point with `namei`, requires a directory that has nothing mounted on it, and keeps that reference in `sb->covered`.
   - It sets `covered->mounted = sb`.
   - If the directory is missing from the image, it only warns (`/dev not mounted (...); create /dev in the image`).

Every block access in `efs.c` goes to its own superblock's disk (the `BREAD(bno)` macro is `bread(sb->disk, bno)`), and each mounted efs has its own `struct efs_log` on its own disk, with its own log area, transactions and recovery. The two filesystems' transactions are independent: an operation never spans them, since `rename` and `link` across mounts fail with `EXDEV`.

The `/` mount sets the global `root_inode`. Mounts are added to the `mounts` list and are never removed: there is no unmount and no mount system call.

### Buffer cache

`bread(disk, blockno)` calls `bget`:

1. Under `bc_lock` (a spinlock, IRQs off), it scans the LRU list for a buffer with this `disk` and `blockno` that is `valid` or referenced. It bumps `refcnt`, drops the spinlock, and sleeps on the buffer's mutex. This waits out another thread that is still reading the block in.
2. Otherwise it walks the LRU from the tail (least recently used) and takes the first buffer with `refcnt == 0 && pins == 0`. It sets `disk`, `blockno`, `valid = false` and `refcnt = 1`, then locks the buffer.
3. If no such buffer exists, it prints every held buffer with the code that took it (`dump_held`) and panics with "bcache: no free buffers (a missing brelse?)". This is deliberate: it turns a hang into a diagnosis.
4. Back in `bread`, if the buffer is not valid, `blk_rw` reads 8 sectors and the buffer is marked valid. On an I/O error it prints `bread: I/O error on diskD block N` and **still marks the buffer valid**.

The other calls:

- `bwrite` writes the block through to its disk (`b->disk`). It panics unless the caller holds the buffer's mutex.
- `brelse` checks ownership, records `last_used`, unlocks, and when `refcnt` drops to 0 moves the buffer to the head of the LRU.
- `bpin` and `bunpin` adjust `pins` under `bc_lock`.

Statistics: `hits`, `misses` and `writes` (`bcache_print`).

### Transactions: begin, write, end

Every efs modification happens between `fs_begin_op(sb)` and `fs_end_op(sb)`, which map to `log_begin` and `log_end`. Inside the transaction, a modified buffer is passed to `log_write` instead of `bwrite`.

**`log_begin`** (`kernel/fs/log.c`):

- If `current->fs_op_depth` was already nonzero, the call is nested and returns at once. The outer operation's reservation covers it.
- Otherwise it loops until `!committing && n + (outstanding+1) × 16 ≤ 64`, then increments `outstanding`.
- While waiting it sleeps on `log->wq` with `wait_what = "log space"` (not interruptible).
- So at most 4 operations run at once, fewer when blocks are already pending.

**`log_write(b)`**:

- It panics if called outside a transaction ("log_write of block N outside a transaction (missing fs_begin_op?)").
- If `b->blockno` is already in `blocks[]`, the write is **absorbed**: nothing changes except the `absorbed` counter.
- Otherwise it appends the block number and pins the buffer, so the modified data cannot be evicted before it is installed.
- If the transaction already holds 64 blocks it panics with "transaction too big".

Note that `MAXOPBLOCKS` is a reservation only. Nothing checks that one operation stays within 16 blocks; only the hard 64-block limit panics.

**`log_end`**:

- It decrements `fs_op_depth` and returns if the call was nested.
- It decrements `outstanding`.
- If other operations are still running, it wakes the waiters (space may have opened up) and returns. **The changes are not on disk yet.** They are committed together with the other operations (group commit).
- If this was the last operation, it sets `committing`, drops the spinlock, and calls `commit` with no locks held. Commit sleeps on disk I/O.
- Afterwards it clears `committing` and wakes the waiters.

### Commit, install and crash points

`commit` (`kernel/fs/log.c`), when `n > 0`:

```
1. for i in 0..n-1: copy cached home block blocks[i] -> log data block start+1+i; bwrite it
   crash_point("after-log")
2. write_head(n)          header: magic, n, seq, blocks[0..n-1]      <- COMMIT POINT
   crash_point("after-commit")
3. install(n, blocks):    for each i: copy log block i -> home block; bwrite; bunpin
                          crash_point("mid-install") after block n/2
4. n = 0; write_head(0)   log empty
   statistics; trace(log_commit, seq, blocks, us); seq++
```

The header is one block, and the transaction counts as committed exactly when the header write with `n > 0` reaches the disk. If the power fails before that point, the log data blocks are garbage and are ignored. If it fails after, the transaction is replayed.

Every block is written twice, once to the log and once home. **Data blocks are journaled as well as metadata**: `efs_write` calls `log_write` on file data blocks.

A crash point is armed by the `fs.crash=after-log|after-commit|mid-install` boot option (read in `log_create`), or at run time by `efs_arm_crash` through the `SYS_debug` call `DBG_FSCRASH` (`dbg fscrash POINT` in `eutils`). When armed, `crash_point` logs `fs.crash=<point>: simulating a power failure during commit N` and calls `platform_exit(3)`, which stops QEMU with exit status 3. Crash points apply only to the first efs log created (`the_log`, the root filesystem): `log_create` reads `fs.crash=` only for that one, and `efs_arm_crash` arms only it, so `/home` never simulates a power failure.

### Recovery

`log_create` reads the header:

- Bad magic: "log header has bad magic ...; not replaying".
- `n > 64`: "log header claims N blocks; not replaying".
- `n > 0`: it prints "recovering: replaying committed transaction SEQ (N blocks)", calls `install(..., recovering=true)` (no unpinning, because nothing was pinned), and then writes an empty header.

After that, `seq = lh.seq + 1`. Replay is idempotent: if the machine dies during replay, the header still says `n > 0` and the next mount replays again. `install` also checks the `mid-install` crash point during recovery, so booting with `fs.crash=mid-install` while a committed transaction is pending stops the machine in the middle of the replay.

### Allocation

**`balloc`** (`efs.c`):

1. Starting at `bhint`, it scans the data area circularly, reading the bitmap block that covers each candidate (`bmapstart + bno/32768`).
2. At the first clear bit it sets the bit and logs the bitmap block.
3. It advances `bhint`, decrements `nfree_blocks`, and zeroes the new block through the log (`bzero_block`, so a new block never exposes old contents).
4. It returns the block number. On a full disk it returns 0 and warns "efs: out of disk blocks".

So each newly allocated block costs two log entries: the bitmap block (often absorbed) and the zeroed block. A data write that follows absorbs the second.

**`bfree`** checks that the block is inside the data area (otherwise `fs_error`), reports a double free with `fs_error`, clears the bit, logs the bitmap block, and increments `nfree_blocks`.

**`efs_alloc_inode`** scans inodes 1..`ninodes`-1 for `type == EFS_T_FREE`. It clears the dinode, sets the type and the times, logs the block, decrements `nfree_inodes`, and returns `iget(sb, ino)`. On failure it warns "efs: out of inodes". The VFS then sets `nlink`, `mode` and owner, and writes the inode (`vfs.c: create_in`).

### Reading and writing file data

**`efs_read(ip, dst, off, n, user)`**:

- It clips the read at `size`.
- Then, block by block, it calls `bmap(..., alloc=false)` and copies from the buffer, or from zeros for a hole.
- Copies to user space use `copy_to_user` while the buffer is locked.
- A fault returns the bytes copied so far, or `-EFAULT`.

**`efs_write`**:

- It returns `-EFBIG` past `EFS_MAXFILE`.
- For each block it calls `bmap(..., alloc=true)`, which allocates the data block and any missing indirect blocks; `ind_get` logs the updated indirect block. It then copies the data in and logs the block.
- It stops early on a full disk or a fault.
- It updates `size` and `mtime` and writes the inode.
- It returns the bytes written, or `-ENOSPC`/`-EFAULT` if nothing was written.

**`file_write`** (`file.c`) splits a request into 16 KiB chunks. Each chunk is its own transaction: `fs_begin_op`, then `ilock`, then the write, then `iunlock`, then `fs_end_op`. With `O_APPEND`, `f->off` is reset to `ip->size` under the inode lock for every chunk.

Worst-case logged blocks for one chunk: 4 data blocks, the bitmap block(s), the single-indirect, the double-indirect and one inner indirect block, and the inode block. That is about 10, under the 16 reserved.

A large `write()` is therefore **not atomic** across chunks: a crash can leave a prefix of it on disk. Each 16 KiB chunk is atomic.

**`file_read`** takes `ilock`, reads at `f->off`, and advances it. A regular file read never starts a transaction.

### Truncate and freeing inodes

**`efs_truncate(ip, size)`**:

- `keep` = ⌈size / 4096⌉ blocks are kept. It frees the direct blocks from `keep` on.
- It calls `free_ind` on the single-indirect block (depth 1) and the double-indirect block (depth 2), with `from` relative to each region.
- `free_ind` frees the data blocks at or after `from` and zeroes their slots, logging the indirect block. A block it fully empties (`from == 0`) is freed too.
- It sets `size` and `mtime` and writes the inode.

The only caller with `size != 0` would be a growing or shrinking `ftruncate`, but no such system call exists. In practice truncate runs from `open(O_TRUNC)` (`vfs_open`) and from `efs_free_inode`.

**Freeing an inode.** `iput` frees it on disk when dropping the **last** reference to a valid inode whose `nlink == 0`:

1. `iput` drops `icache_lock`.
2. It calls `fs_begin_op` (nested if the caller is already in a transaction).
3. It locks the inode and calls `free_inode`. For efs, `efs_free_inode` truncates to 0, sets `type = I_FREE` and `nlink = 0`, writes the inode, and increments `nfree_inodes`.
4. It calls `fs_end_op`.

This gives Unix semantics for unlinking an open file: the data lives until the last `close`.

### Directories

All directory operations run with the directory's inode locked:

- **`dir_find`** reads the directory one 64-byte record at a time through `efs_read` and compares names with `strncmp(..., 60)`. Lookup is a linear scan.
- **`efs_link`**:
  1. It validates the name (`name_ok`: 1..59 bytes, no `/`), returning `-ENAMETOOLONG` or `-EINVAL`.
  2. It returns `-EEXIST` for a duplicate name.
  3. It reuses the first free slot, or appends at `size`, and writes the entry with `efs_write`.
- **`efs_unlink`** overwrites the entry with zeros.
- **`efs_readdir`** returns the next non-empty entry from byte offset `*off` with type `DT_UNKNOWN`. `do_getdents64` then `iget`s and locks each child to fill in `d_type`.

### Path lookup (`walk`)

`walk(path, parent, name, err)` in `kernel/fs/vfs.c`:

1. It starts at the process's `cwd` for a relative path, otherwise at `root_inode`. An empty path is `-ENOENT`.
2. For each component (`next_component`; one longer than 59 bytes is `-ENAMETOOLONG`):
   1. It locks the current inode. A non-directory gives `-ENOTDIR`. Without search permission (`vfs_permission(ip, X_OK)`) it returns `-EACCES`.
   2. In `parent` mode, if this was the last component, it copies the name out and returns the directory unlocked but referenced. The lookahead skips trailing slashes.
   3. `.` stays put.
   4. `..` at the root of a mounted filesystem continues from the covered directory in the parent filesystem; at `/` it stays at `/`.
   5. It looks up the name with `ops->lookup`; a missing name is `-ENOENT`.
   6. It `iget`s the result and unlocks and releases the directory, so only one inode is locked at a time.
   7. If the result has a filesystem mounted on it, it switches to that filesystem's root.
3. If `parent` is set and the loop ran out (the path was `/` or only `.`), it returns `-EEXIST`.

There are no symbolic links. Only `AT_FDCWD` is accepted as `dirfd`: `get_path` in `file.c` returns `-EINVAL` for any other `dirfd` with a relative path.

### Permission checks

`vfs_permission(ip, mask)` uses the current process's effective uid and gid; kernel threads count as root.

- **uid 0:** everything is allowed, except executing a regular file that has no `x` bit at all.
- **Everyone else:** exactly one class of bits applies: owner if the euid matches, else group if the egid matches, else other. All bits in `mask & 7` must be present.

Supplementary groups and the sticky bit are not supported.

Where the checks happen:

| Operation | Check |
|---|---|
| each directory searched by `walk` | `X_OK` |
| create (`vfs_open` with `O_CREAT`), `mkdir`, `unlink`, `link`, `rename` | `W_OK\|X_OK` on the parent; for `rename`, both parents |
| `open` | `R_OK` for `O_RDONLY`/`O_RDWR`; `W_OK` for `O_WRONLY`/`O_RDWR`, or whenever `O_TRUNC` is given |
| `open` of a device or `/proc` file | `-EACCES` for an access mode its `chr_ops` cannot serve (no `write` or no `read`) |
| `exec` (`kernel/proc/proc.c`) | `X_OK` on a regular file |
| `chmod` (`cred.c: chmod_inode`) | owner or root |
| `chown` | root only |
| `faccessat` | `vfs_permission` with the effective ids |

### Opening and creating (`vfs_open`)

**With `O_CREAT`:**

1. `nameiparent`.
2. Lock `sb->ns_lock`, then `fs_begin_op`, then `ilock(dir)`.
3. Look up the name.
4. **If it exists:** release everything. Then `-EEXIST` with `O_EXCL`; otherwise re-resolve the full path with `namei` so that a mount point is crossed.
5. **If not:** check `W|X` on the directory, then `create_in`:
   - `alloc_inode`, `ilock`, `nlink = 1`, `mode = mode & 07777 & ~umask`, uid and gid from the effective ids;
   - for a directory: link `.` and `..`, `nlink = 2`, and `parent->nlink++`;
   - `write_inode`, then link the name into the parent.
   - On failure it sets `nlink = 0` and `iunlockput`s, which frees the inode inside the same transaction.

**Without `O_CREAT`:** `namei` and `ilock`.

**Then the common checks:**

- an invalid inode (`read_inode` failed) gives `-EIO`;
- a directory opened for writing gives `-EISDIR`;
- `O_DIRECTORY` on a non-directory gives `-ENOTDIR`;
- the permission checks from the table above;
- `chr_ops` capability checks.

**Then:**

- `O_TRUNC` on a non-empty regular file opened for writing calls `truncate(ip, 0)` in its own transaction. Note that this calls `fs_begin_op` while holding the inode lock (see Concurrency).
- It allocates the `struct file`, records the path, unlocks the inode (keeping the reference), and emits `trace(file_open, <last 24 bytes of path>)`.
- It calls `cops->open` if there is one. procfs generates its snapshot here; `/dev/tty` swaps in `tty_ops`.

### Namespace operations

All of these take `sb->ns_lock` and run as one transaction, so each is atomic across a crash.

- **`vfs_mkdir`**: `create_in(..., I_DIR, ...)`.
- **`vfs_unlink(path, want_dir)`**:
  - It rejects `.` and `..`.
  - `unlink_locked` locks the parent, looks up the name, checks `W|X`, and locks the child.
  - It returns `-EBUSY` for a mount point, `-ENOTDIR`/`-EISDIR` for a type mismatch, and `-ENOTEMPTY` for a directory with entries other than `.` and `..`.
  - Then it removes the entry. Removing a directory decrements the parent's `nlink` (for the child's `..`) and sets the child's `nlink` to 0; removing a file decrements its `nlink`.
  - Finally `iunlockput(child)` frees the inode if that was the last reference.
- **`vfs_link`**:
  - Both paths must be on the same filesystem (`-EXDEV`), and the target may not be a directory (`-EPERM`).
  - It increments `nlink` and writes the inode first, then adds the entry. If adding the entry fails, it undoes the increment.
- **`vfs_rename`**:
  1. It rejects `.` and `..` (`-EINVAL`) and paths on different filesystems (`-EXDEV`).
  2. It checks `W|X` on both parents.
  3. A directory moved under itself is `-EINVAL` (`is_ancestor` walks `..` from the new parent, at most 256 levels).
  4. Renaming one name of a file onto another name of the same file does nothing.
  5. An existing target must be a file, and so must the source (`-EISDIR`/`-ENOTDIR` otherwise); the target is removed with `unlink_locked`.
  6. It links the new name, then unlinks the old one. For a directory moved between parents it adjusts both parents' `nlink` and rewrites the directory's `..`.

  It does not check whether the source is a mount point.
- **`vfs_getcwd`**: walks up from the directory. At each step it finds `..`, then scans the parent for an entry with the child's inode number, and prepends that name. It climbs out of mounts through `sb->covered`. Depth is limited to 64, and the result to `PATH_MAX`.

### Descriptor tables and file system calls (`file.c`)

- **Installing:** `fd_install_from` picks the lowest free slot at or after `start`; a full table gives `-EMFILE`.
  - `openat` sets `FD_CLOEXEC` from `O_CLOEXEC`.
  - `dup` takes the lowest free slot.
  - `fcntl(F_DUPFD / F_DUPFD_CLOEXEC)` takes the lowest free slot at or after the argument.
  - `dup3` replaces a slot (`-EINVAL` if `oldfd == newfd`) and closes the old file.
- **`fcntl`** supports `F_GETFD`/`F_SETFD` (only `FD_CLOEXEC`), `F_GETFL`, and `F_SETFL`, which may change only `O_APPEND` and `O_NONBLOCK`.
- **`fork`** (`files_fork`): dups every file and copies the flags and `idup(cwd)`. **`exec`** (`files_exec`): closes `FD_CLOEXEC` descriptors. **`exit`** (`files_close_all`): closes everything and drops `cwd`, clearing the `cwd` pointer under `proc_lock`.
- **`file_close`**: drops `refcnt` under `file_lock`. On the last reference it calls `cops->close` and `iput(ip)` (which may free an unlinked inode) and frees the file.
- **`lseek`**:
  - It returns `-ESPIPE` for files without an inode (pipes, sockets), pipes, and devices without a snapshot. That includes `/dev/null`, `/dev/zero`, the tty devices and `/dev/disk0`.
  - procfs files and `/dev/kmsg` (`priv` set) are seekable; `SEEK_END` uses the snapshot length.
  - Seeking past EOF is allowed, and a later write leaves a hole.
- **`fstat` / `fstatat`**: `vfs_stat` fills `struct stat`.
  - `st_mode` = mode | `S_IFDIR`/`S_IFCHR`/`S_IFREG`; `st_blocks` = ⌈size/512⌉ (0 for devices); `st_dev` is always 0. Times are POSIX `struct timespec`s: `st_mtim` and `st_ctim` hold seconds and nanoseconds split from the inode's nanosecond `mtime`/`ctime`, and `st_mtime`/`st_ctime` are macros for their `tv_sec` (until this change they were `u64` nanosecond fields). efs keeps no access time, so `st_atim`/`st_atime` are macros for the modification time. Filesystems that keep no times (devfs, procfs) report the boot time (`boot_time_ns`, from the RTC minus the uptime).
  - A pipe gets `S_IFIFO|0600`. For a procfs snapshot, `st_size` is the snapshot's length.
- **`utimensat`** (88, `kernel/proc/cred.c: do_utimensat`), behind libc's `utimensat`, `futimens` and `touch`:
  - A NULL path means the open file `dirfd` (that is `futimens`); otherwise `dirfd` must be `AT_FDCWD` (`EINVAL`) and the path is looked up.
  - `times` NULL means now. Otherwise `times[0]` (access time) is ignored and `times[1]` gives the modification time: `UTIME_NOW`, `UTIME_OMIT` (change nothing), or an explicit time (`tv_nsec` must be in 0..999,999,999 and `tv_sec` ≥ 0, else `EINVAL`).
  - Permissions: root, or the owner, may do anything. Others may only set the time to now, and only with write permission through the group or other bits; an explicit time gives `EPERM`, a missing write permission `EACCES`. A filesystem without `write_inode` (devfs, procfs) gives `EPERM`.
  - It sets `mtime` and sets `ctime` to now, inside a transaction, and writes the inode. Like `chmod`, it begins the transaction before taking the inode lock (see Concurrency).
- **`getdents64`**: fills fixed 64-byte `struct dirent` records (`d_ino`, `d_type`, `d_name[59]`) and advances `f->off` after each record it copies.
- **`chdir`**: requires a directory and swaps `p->cwd` under `proc_lock`.
- **`getcwd`**: returns the length including the NUL.
- **`sync` and `fsync`** do nothing. They are justified by "every transaction is already on disk", which is true only when no other operation was in progress (see Limitations).

### Pipes

`pipe_create` allocates the `struct pipe` and a page, plus two files that share it (`f->priv`); `pipe2` installs them. `O_CLOEXEC` is honored; `O_NONBLOCK` is ignored.

**`pipe_read`:**

- While the pipe is empty and writers remain, it releases the mutex and waits interruptibly on `rwait` (`"pipe read"`); a signal gives `-ERESTARTSYS`.
- It then copies `min(n, buffered)` bytes, handling the wrap at 4096, and wakes `wwait`.
- An empty pipe with no writers returns 0 (EOF).

**`pipe_write`:**

- With no readers left it sends `SIGPIPE` and returns `-EPIPE` (or the partial count).
- When the pipe is full it wakes the readers and waits interruptibly on `wwait` (`"pipe write"`).
- It copies as much as fits and loops until all `n` bytes are written. Writes larger than the free space are therefore split, and **concurrent writers can interleave**.

**`pipe_close`:** decrements `readers` or `writers` under the mutex and wakes both queues. Then it decrements `ends` atomically **as its very last access to the pipe**; whoever reaches 0 unlinks the pipe from the list and frees it. This lets both ends close at the same time on different CPUs.

**`pipe_poll`:**
- read end: `POLLIN` if data is buffered, `POLLHUP` if there are no writers;
- write end: `POLLERR` if there are no readers, else `POLLOUT` if there is space.

### `ppoll`

`do_ppoll(fds, nfds, timeout, sigmask)` (`poll.c`):

1. It allows at most `POLL_MAX_FDS` = 64 descriptors and copies them in. A timeout becomes an `rdtime` deadline; a `NULL` timeout waits forever. The signal mask argument is ignored.
2. It allocates a `poll_table` with `POLL_MAX_WAITS` = 128 wait entries.
3. In a loop:
   1. `set_current_sleeping("poll")`.
   2. Each `file_poll` registers its wait queues through `poll_wait` and returns a mask. Disk files without `chr_ops.poll` report `POLLIN` and/or `POLLOUT` according to `readable`/`writable`. A bad fd reports `POLLNVAL`.
   3. `revents = mask & (events | POLLERR | POLLHUP | POLLNVAL)`.
   4. It stops if anything is ready, the deadline has passed, or a signal is pending (`-EINTR`).
   5. Otherwise `schedule_until(deadline)`, then it removes all wait entries and repeats.
4. It marks itself running, removes the entries, and copies `revents` back.

Because the thread is marked sleeping before any check, a wakeup between a check and the sleep is not lost. The rule this imposes is that **`chr_ops.poll` must not sleep**: no mutexes and no I/O. `pipe_poll` reads its fields without the lock for that reason.

### devfs

`dev_lookup` matches names in the `devs[]` table, and `dev_readdir` lists `.`, `..` and then inodes 2..9 in order, skipping missing disks. Files and their `chr_ops`:

- **`/dev/kmsg`**: `kmsg_open` copies the log ring (`log_ring_copy`, up to `LOG_BUF_SIZE`) into `f->priv`. Reads come from that snapshot.
- **`/dev/disk0`, `/dev/disk1`**: `disk_rd` reads a page at a time with `blk_rw` from `f->off`, up to `blk_capacity(disk) × 512` bytes. There is no write. `disk1` (inode 9) exists only when `blk_count() > 1`: `dev_lookup`, `dev_read_inode` and `dev_readdir` skip a disk node whose disk is missing. Disks 2 and 3 have no device node.
- **`/dev/tty`**: `ctty_open` fails with `-ENXIO` if the process has no controlling terminal. Otherwise it replaces `f->cops` with `tty_ops`. Its placeholder `read`/`write` exist only so `vfs_open`'s capability checks pass.

### procfs

**Opening a file** (`proc_open`):

1. Allocate a 64 KiB buffer (`SEQ_MAX`).
2. Take the global `gen_lock` mutex, point `seq_buf` at the buffer, and call the generator with `seq_print` as its `print` function. Output beyond 64 KiB − 1 is silently dropped.
3. Store the length in `f->priv_len`.

Reads (`proc_read`) and seeks then work on this snapshot, so a reader always sees one consistent snapshot.

**Generators:**

- **Global files**: the same print functions the monitor uses (`pmm_print`, `bcache_print`, `vfs_mounts_print`, ...). `uptime` and `trace` (the last 200 events) are local.
- **`/proc/net/*`**: generated with the network mutex `net_lock` held (`NET_GEN`).
- **`/proc/<pid>/*`** (`gen_pid`): generated with `proc_lock` held, so the process cannot be freed meanwhile.
  - `status`: pid, ppid, name, state, tid, waiting, switches, syscalls, RSS, ids, umask, process group, session, and signal masks plus caught and ignored signals; then the fields `ps` needs: `ustate` (the Unix state letter: R, S, D for an uninterruptible sleep, T, Z), `start_ms` (start time, ms since boot), `cpu_ms` (CPU time including the current slice, `thread_cpu_ticks`), `vsz_kib` (sum of the VMAs), `tty` (`console` or `-`), `tpgid` (the terminal's foreground group), `psr` (the CPU it last ran on).
  - `maps`: `mm_print`.
  - `fds`: first a `cwd <path>` line, then `files_print_fds`. The cwd path needs sleeping I/O, so it is resolved *before* taking `proc_lock`, using a reference taken with `idup`.
  - `stack`: the kernel backtrace (`thread_backtrace`).
  - `ustack`: the user backtrace from the saved trap frame.
  - `cmdline`: the arguments, NUL-separated (`proc.cmdline`, set by `do_exec`).

**Names** (`pr_lookup`):
- `self` maps to the caller's pid directory.
- A decimal name maps to that pid directory if `proc_exists`.
- `..` always maps to `/proc`.

**Listing** (`pr_readdir`): the global files, then `net`, then one directory per process (`proc_nth_pid(index)`). Processes that come and go between `getdents` calls can be skipped or listed twice. `self` is not listed.

A pid directory's `read_inode` fails with `-ENOENT` once the process is gone.

### Host mkfs and fsck (`tools/mkfs/mkfs.c`)

**`mkfs [--label NAME] IMAGE SIZE_MB specs...`** builds the image in memory (`calloc`):

- It lays out the regions as above and marks the metadata blocks used. It writes the log header magic with `n = 0`, and copies `--label` into the superblock (`strncpy`, so up to 16 bytes; empty by default).
- It creates the root directory with `.` and `..`.
- Then it processes each spec:
  - `/path=hostfile`: a regular file, mode 0755 if the host file is executable, 0644 otherwise.
  - `/path=@/existing`: a hard link to a file listed earlier.
  - `/dir/[:mode[:uid[:gid]]]`: a directory, created with any missing parents.
- Blocks and inodes are allocated sequentially (`next_block`, `next_ino`).

The Makefile builds `build/fs.img` with `FS_MB ?= 32` from the user programs (`/bin/*`; the `eutils` names are hard links), `/etc/motd`, `/etc/passwd`, `/etc/group`, `/etc/hostname`, `/etc/eshrc`, `/dev/`, `/proc/`, `/tmp/:0777`, `/www/`, `/home/` and `/home/guest/:0755:1000:1000`. The image is rebuilt whenever a program changes, which discards run-time files. It gets `--label root`.

The persistent `/home` disk, `home.img` (`HOME_IMG ?= home.img` in the top directory, `HOME_MB ?= 64`), is made once with `--label home` and the single spec `/guest/:0755:1000:1000`, so it holds `/guest` owned by uid and gid 1000 (`guest` in `/etc/passwd`, home `/home/guest`). The Makefile never rebuilds it, so what is created there survives `make` and `make clean`; `make clean-home` deletes it. `make run`, `debug` and `qemu-log` attach it as a second virtio disk; the test scenarios do not. The root image keeps its own `/home/guest`, which is what you see when no home disk is attached.

**`mkfs --check IMAGE`** (`do_check`) exits with 0 if clean, 1 if it found errors, 2 for usage or I/O errors. It checks, in order:

1. The superblock: magic (fatal; the label, if any, is printed as `label NAME`), `bsize`, `size ≤` file size, `logstart == 2`, `inodestart == logstart + nlog`, and that the regions are ordered and `datastart < size`.
2. The log header: magic, `n ≤ 64`. A committed transaction only produces a *note* ("the kernel replays it at mount"); fsck does not replay it.
3. That every metadata block is marked used in the bitmap.
4. Root is a directory. A recursive `check_dir` (depth ≤ 64) checks:
   - the directory size is a multiple of 64;
   - names are NUL-terminated and non-empty;
   - entries point to allocated, in-range inodes;
   - `.` and `..` are correct;
   - no directory has two parents;
   - types are known.

   It also counts references per inode.
5. Every allocated inode, through `check_blocks`: each pointer is in range and not used twice (`owner[]`), and every used block is marked in the bitmap. The size must be ≤ `EFS_MAXFILE`. Inodes with no references are orphans; otherwise `nlink` must equal the number of references.
6. Leaked blocks: marked used but owned by no inode.

`tests/host/test_fsck.py` checks a fresh image and seven corruptions: a used block marked free, a leaked block, a wrong `nlink`, a doubly used block, an orphan inode, a bad `..`, and a bad magic.

### Crash-recovery test scenario

`make test` runs `tools/run_tests.py` on a scratch copy of the image (`build/test-fs.img`). Every boot uses `panic=shutdown lockdep=panic`.

| Stage | Boot option | What `user/bin/utests.c: scenario` does | Then |
|---|---|---|---|
| kernel+user tests | `ktest utest` | the full suites; the last step writes `/home/persist.txt` | fsck must be clean and must have no pending log |
| persistence | `utest=persist-check` | the file must read "survives reboots" | — |
| crash after commit | `utest=crash-commit-write` | writes "part1", arms `after-commit`, appends "part2". QEMU must exit with status 3. | fsck: clean plus the note "log holds a committed transaction" |
| recovery replays | `utest=crash-commit-check` | the boot log must show "recovering: replaying"; the file must read `part1part2` | fsck clean, log empty |
| crash before commit | `utest=crash-log-write` | the same, with `after-log` | fsck clean, no pending log |
| recovery discards | `utest=crash-log-check` | the file must read `part1`; no replay | fsck clean |
| smp stress | `utest=stress` | `/bin/stress -j 8` | must have no "filesystem error", LOCKDEP or HUNG THREAD; fsck clean |

The `mid-install` point is not part of the scenario. Use it by hand.

## Concurrency and locking

**Locks:**

| Lock | Type | Protects |
|---|---|---|
| `sb->ns_lock` | mutex | serializes every namespace change on one filesystem (create, mkdir, unlink, link, rename) |
| log space (`log->outstanding`/`committing`) | wait queue, not a lock | admission to a transaction; commit excludes all operations |
| `inode->lock` | mutex | an inode's fields and contents (data blocks, directory entries) |
| `buf->lock` | mutex | a buffer's `data` while held |
| `bc_lock` | spinlock | LRU list, `refcnt`, `pins`, `disk`/`blockno`/`valid` during lookup, statistics |
| `icache_lock` | spinlock | the inode list and `refcnt` |
| `log->lock` | spinlock | log fields |
| `file_lock` | spinlock | `file.refcnt`; lets other CPUs read descriptor tables (`files_print_fds`) |
| `proc_lock` | spinlock (proc subsystem) | `p->cwd` swaps (`do_chdir`, `files_close_all`); held while procfs generates per-pid files |
| `pipe->lock` | mutex | ring positions and counts |
| `pipes_lock` | spinlock | the pipe list |
| `gen_lock` | mutex | procfs's single `seq_buf` (one generator at a time) |
| `net_lock` | mutex (net) | taken by `/proc/net` generators |

**Lock order.** `fs.h` documents it outermost first: `sb->ns_lock`, then inode locks (directory before entry), then buffer locks. In practice the transaction is begun **after** `ns_lock` and **before** inode locks in all namespace operations and in `file_write`:

```
ns_lock  ->  fs_begin_op (log space)  ->  inode lock(s) (dir, then entry)  ->  buffer lock(s)  ->  bc_lock / log->lock
```

- `walk`, `vfs_link`, `vfs_rename`, `is_ancestor` and `vfs_getcwd` hold at most one inode lock at a time.
- `create_in` and `unlink_locked` hold the directory and then the child.
- `iput`'s free path takes the inode lock inside a (possibly nested) transaction.

**No exceptions to the order.** `vfs_open` with `O_TRUNC`, `cred.c: chmod_inode`, `do_fchownat` and `do_utimensat` used to take `ilock` *before* `fs_begin_op`. Waiting for log space while holding an inode lock can deadlock with an operation that is already admitted and waits for that inode (a `file_write` that has begun and waits for `ilock`, while enough blocks are pending that the other operation cannot be admitted until that writer finishes). They now begin the transaction first; `vfs_open` drops the inode lock to do so and takes it again. lockdep does not see log space as a lock, so it could not have reported the old order; the hung-thread detector (mutex waits over 30 s) would have.

**Transactions and sleeping:**
- `commit` runs in the thread that ended the last operation, with no spinlocks held. It sleeps on disk I/O.
- New operations wait in `log_begin` (not interruptible) while a commit runs.
- A thread may nest operations. The depth is per thread (`fs_op_depth`), so a nested `iput` → `free_inode` joins the outer transaction.

**Buffers:**
- The `bc_lock` spinlock is dropped before sleeping on the buffer mutex.
- A buffer found mid-read (referenced, not yet valid) is safe to wait on, because the reader holds its mutex across `blk_rw`.
- Pinned buffers are never recycled, so up to 64 of the 128 buffers can be pinned by a transaction.

**Readers see uncommitted state.** Modifications are made directly in cached buffers. A reader (`file_read`, lookups) sees them before the commit, which is consistent because the cache is the authoritative copy and pinned buffers cannot be evicted.

**Allocation:**
- Bitmap test-and-set is atomic because it happens under the bitmap buffer's mutex.
- Inode allocation is atomic under the inode-block buffer's mutex, and creators also hold `ns_lock`.
- `bhint`, `nfree_blocks` and `nfree_inodes` are updated without any lock (`alloc_lock` exists but is unused).

**Interrupt context.** None of this code runs in interrupt context. The block driver's completion interrupt only wakes the waiting thread. The spinlocks use `irqsave` and are held only briefly.

**Monitor and SMP safety:**
- `icache_print`, `pipes_print` and `files_print_fds` use `spin_lock_debug`. They can run from the stopped-world monitor even if the lock is held.
- `bcache_print` and `efs_stat_fs` read counters without locks and do no I/O.
- `files_print` reports only the cwd inode number, because resolving a path would sleep.
- Descriptor slots are always cleared *before* `file_close` takes `file_lock`. A reader holding `file_lock` therefore never sees a slot whose file is being freed (comment in `file.c`).

**`ppoll`** registers on wait queues while marked sleeping. `chr_ops.poll` implementations must not take mutexes.

## Interfaces

### Kernel functions used by other subsystems

| Function | Used by |
|---|---|
| `vfs_init` | `kinit` |
| `vfs_mount_disk(type, path, disk)`, `efs_label(disk, out)` | `vfs_init` (`vfs_mount` is the disk-0 form, used for devfs and procfs) |
| `namei`, `nameiparent`, `iget`, `idup`, `ilock`, `iunlock`, `iput`, `iunlockput` | `exec`, `cred.c`, procfs, tests |
| `vfs_open`, `file_read`, `file_write`, `file_dup`, `file_close`, `fd_install`, `fd_get` | sockets, exec, ktests |
| `vfs_read_all(path, &size, &err)` | `exec`: whole file into `kmalloc` memory, max 16 MiB, regular files only |
| `vfs_permission`, `vfs_stat`, `vfs_getcwd`, `fs_begin_op`/`fs_end_op` | `cred.c`, `proc.c`, procfs |
| `files_fork`, `files_exec`, `files_close_all` | `fork`, `exec`, `exit` |
| `poll_wait`, `file_poll` | socket and tty `poll` ops |
| `pipe_create`, `file_is_pipe` | `pipe2`, `lseek` |
| `efs_arm_crash` | `SYS_debug`/`DBG_FSCRASH` |
| `efs_free_blocks`, `efs_free_blocks_counter`, `bcache_busy`, `icache_busy` | ktests |
| `bcache_print`, `icache_print`, `vfs_mounts_print`, `pipes_print`, `files_print`, `files_print_fds` | monitor, procfs |
| `seq_print` | procfs generators (also declared for `/dev/kmsg`) |
| `fs_error(sb, fmt, ...)` | efs consistency reports |

### System calls

Numbers are Linux RISC-V (`lib/include/uapi/syscall.h`).

| # | Call | Implementation | Notes |
|---:|---|---|---|
| 17 | getcwd | `do_getcwd` | |
| 23 / 24 | dup / dup3 | `do_dup`, `do_dup3` | |
| 25 | fcntl | `do_fcntl` | `F_DUPFD`, `F_DUPFD_CLOEXEC`, `F_GETFD`/`F_SETFD`, `F_GETFL`/`F_SETFL` |
| 29 | ioctl | `do_ioctl` | `chr_ops.ioctl`, else `-ENOTTY` |
| 34 / 35 | mkdirat / unlinkat | `do_mkdirat`, `do_unlinkat` | `AT_REMOVEDIR` |
| 37 / 38 | linkat / renameat | `do_linkat`, `do_renameat` | `AT_FDCWD` only (an absolute path with another `dirfd` also works) |
| 48 | faccessat | `cred.c: do_faccessat` | uses the effective ids |
| 49 | chdir | `do_chdir` | |
| 52 / 53 / 54 | fchmod / fchmodat / fchownat | `cred.c` | |
| 88 | utimensat | `cred.c: do_utimensat` | see below |
| 56 / 57 | openat / close | `do_openat`, `do_close` | |
| 59 | pipe2 | `do_pipe2` | `O_CLOEXEC` only |
| 61 | getdents64 | `do_getdents64` | 64-byte `struct dirent` |
| 62 / 63 / 64 | lseek / read / write | | |
| 73 | ppoll | `do_ppoll` | sigmask ignored |
| 79 / 80 | fstatat / fstat | | |
| 81 / 82 | sync / fsync | no-ops | |
| 502 | debug(`DBG_FSCRASH`, point) | `kernel/proc/syscall.c` | arms a crash point; also `DBG_FILES` prints the caller's table |

### Files

- **`/dev`**: `console`, `tty`, `tty0`, `null`, `zero`, `kmsg`, `disk0`, and `disk1` when a second disk is attached.
- **`/proc`**: `meminfo`, `slabinfo`, `interrupts`, `threads`, `sched`, `procs`, `mounts`, `bcache`, `blk`, `icache`, `trace`, `virtio`, `uptime`, `pipes`, `tty`, `cpus`, `lockdep`, `profile`.
- **`/proc/net`**: `dev`, `arp`, `tcp`, `udp`, `stats`.
- **Per process**: `/proc/<pid>/{status,maps,fds,stack,ustack,cmdline}` and `/proc/self`.

### Boot options

| Option | Effect |
|---|---|
| `fs.crash=after-log\|after-commit\|mid-install` | stop the machine (exit 3) at that point of the next commit |
| `log=vfs:debug`, `log=fs:...`, `log=blk:...` | message thresholds. `vfs.c`, `file.c`, `pipe.c`, `poll.c`, `devfs.c` and `procfs.c` log as `vfs`; `efs.c` and `log.c` as `fs`; `bcache.c` as `blk` |
| `trace=fs,blk` | trace groups for `file_open`, `log_commit`, `blk_submit` and `blk_done` |
| `hung=<s>` | report mutex waits (inode, buffer, `ns_lock`, pipe) longer than this |
| `utest=persist-check\|crash-commit-write\|...` | the multi-boot scenarios |

### Monitor commands

`mounts`, `bcache`, `icache`, `pipes`, `blk` (`blk_print` plus `virtio_print`), `files <pid>` and `tty`. None of them sleeps.

### User tools

- `dbg fscrash POINT` and `dbg files` (`eutils`).
- `make fsck`, which runs `build/tools/mkfs --check build/fs.img`. For the home disk, run `build/tools/mkfs --check home.img` while QEMU is not running.
- `make clean-home`, which deletes `home.img` (it is recreated, empty except for `/guest`, by the next `make run`).

## Debugging and observability

**Space and log state:**
- `mounts` (monitor) or `cat /proc/mounts` shows, per mount, the error count.
- For efs it also shows:
  - blocks used and free, and inodes used and free (from the in-memory counters);
  - the log line: commits and `seq`, blocks logged and absorbed, the largest transaction, the average commit time in µs, operations in progress, pending blocks, blocks recovered at mount, and whether a crash point is armed.
- Many "ops in progress" together with a thread stuck in `"log space"` points to a transaction that never ended (a missing `fs_end_op`).

**Buffers:** `bcache` and `/proc/bcache` show buffers valid, referenced and pinned, the hit rate, and every held buffer with the symbolized site of its last `bread` and the lock holder. A buffer that stays referenced after an operation is a missing `brelse`. In GDB, `eu-bcache` shows the same from memory.

**Inodes:** `icache` and `/proc/icache` list every referenced inode: fs, ino, refs, type (`?` while not yet valid), nlink, size, and lock holder. Leaks show up as a growing list; the ktest `fs_ops_release_buffers_and_inodes` asserts that `icache_busy()` and `bcache_busy()` return to their baseline.

**Files and pipes:**
- `files <pid>` (monitor), `/proc/<pid>/fds` (includes the cwd path), `dbg files`, and GDB `eu-files PID`.
- `pipes` and `/proc/pipes` show buffered bytes, readers, writers and the total per pipe.

**Blocked threads:** `ps` and `/proc/<pid>/status` show `waiting`:
- `"log space"`: waiting in `log_begin`;
- `"disk I/O"`: in `blk_rw`;
- `"pipe read"` / `"pipe write"`;
- `"poll"`.

**Trace events:**
- `file_open` (last 24 bytes of the path) and `log_commit` (seq, blocks, µs), in group `fs`;
- `blk_submit` / `blk_done`, in group `blk`.

Export them to Perfetto as described in `DEBUGGING.md`.

**Offline:** `make fsck`. After a crash test, a "note: log holds a committed transaction" is expected; anything listed as `ERROR:` is a real inconsistency.

### Messages and what they mean

| Message | Meaning |
|---|---|
| `efs filesystem error on /: ...` with a backtrace (`fs_error`) | an on-disk inconsistency found at run time; `sb->errors` is incremented. The variants are: freeing a block outside the data area, a double free, a block pointer outside the data area, an inode number out of range, "inode N is free but referenced (a stale directory entry?)", and a directory short read. `make test` fails if this appears. |
| `buffer cache exhausted ...` then panic `bcache: no free buffers (a missing brelse?)` | all 128 buffers are referenced or pinned; the held list says who took them |
| panic `bwrite/brelse of block N by a thread that does not hold the buffer` | a buffer used without `bread`, or released twice |
| panic `log_write of block N outside a transaction (missing fs_begin_op?)` | a modification without `fs_begin_op` |
| panic `transaction too big: N blocks` | one transaction modified more than 64 distinct blocks |
| panic `log_end without log_begin` | unbalanced `fs_end_op` |
| panic `ilock of NULL/unreferenced inode`, `iput of inode N with refcnt R (double iput?)` | reference-count bugs |
| panic `file_close of 'path' with refcnt R (double close?)` | file reference bug |
| `bread: I/O error on diskD block N` | a disk read failed; the buffer is cached anyway (see Limitations) |
| `mounted efs (diskN) on /home` | the boot found a disk labelled `home` and mounted it |
| `/home (diskN) not mounted: E` | a disk is labelled `home` but `efs_mount` failed; `/home` is the root filesystem's directory |
| `efs: out of disk blocks` / `efs: out of inodes` | `ENOSPC` follows |
| `recovering: replaying committed transaction S (N blocks)` | a crash happened after a commit point; normal after crash tests |
| `log header has bad magic ...; not replaying` / `log header claims N blocks; not replaying` | a damaged log header; mounting continues without replay |
| `fs.crash=P: simulating a power failure during commit S` | an armed crash point fired (QEMU exit status 3) |
| `efs: not an efs filesystem` / `unsupported block size / version`, then panic `cannot mount the root filesystem` | wrong or damaged image |
| `/dev not mounted (...)` / `/proc not mounted (...)` | the image lacks the mount-point directory |

## Limitations and known issues

**Durability and the log:**
- Full data journaling: every modified block, file data included, reaches the disk twice (into the log, then home). On allocation, the zeroing and the data write absorb into one log entry, so a new block is not written a third time.
- Commits are synchronous and happen only when no operation is outstanding. A steady stream of overlapping operations delays the commit, and under concurrency `write()` can return before its data is durable. `sync` and `fsync` do nothing, so they do not close this gap.
- A `write()` larger than 16 KiB is split into several transactions and is not atomic as a whole.
- There are no disk cache flushes (`virtio_blk.c` sends only `T_IN`/`T_OUT`). Crash consistency relies on each `blk_rw` write being durable when it completes, which holds for QEMU's emulation but not for a disk with a volatile write cache.
- I/O errors are not propagated:
  - `bread` marks a buffer valid even after a read error, so the garbage stays cached.
  - `commit`, `install` and `write_head` ignore `bwrite` results, so a failed log write can be followed by a successful commit-point write.
- `MAXOPBLOCKS` (16) is a reservation, not a limit.
  - `free_ind` logs every indirect block it empties, even though those blocks are about to be freed.
  - Deleting or truncating a file with N inner double-indirect blocks logs at least N+4 blocks. On images much larger than the default 32 MiB, freeing a file of a few hundred MiB in one transaction can exceed 64 blocks and panic with "transaction too big".
  - A large rename-over (unlink of a big target plus link, unlink and `..` updates) can also exceed its 16-block reservation.

**Locking:**
- `bhint`, `nfree_blocks` and `nfree_inodes` are updated without a lock; `efs_info.alloc_lock` is never used. The counters can drift under concurrent allocation. The ktest `fs_no_errors_recorded` compares the block counter with the bitmap, but only once.

**Inode cache:**
- Unreferenced inodes are freed immediately, so every lookup re-reads dinodes.
- `iget` scans a linear list, and directory lookup is a linear scan of the directory.

**Missing features:**
- No symlinks, no `ftruncate`/`truncate`, no `*at` system calls relative to a directory fd, no mount or unmount system call, no atime.
- `ctime` is set only when the inode is allocated and by `utimensat`: `chmod`, `chown` and `link` do not update it.
- No access time: `utimensat` ignores `times[0]` and `stat` reports the modification time as `st_atime`.
- `utimensat` accepts only `AT_FDCWD` (or a NULL path with an fd) and ignores its flags (there are no symlinks for `AT_SYMLINK_NOFOLLOW` to matter).
- `st_dev` is always 0, so inode numbers from efs, devfs and procfs collide in `stat`, and so do those of the root and `/home` filesystems (both start at inode 1). Tools that detect hard links or loops by (`st_dev`, `st_ino`) can be fooled across the mount.
- Ids are 16 bits; there are no supplementary groups and no sticky bit.
- `faccessat` checks the effective ids, where POSIX uses the real ids.
- `chdir` does not require search (`X`) permission on the target directory itself.

**Limits:**
- 32 descriptors per process. `PATH_MAX` 256. Names up to 59 bytes, but `struct dirent.d_name` is 59 bytes including the NUL, so `getdents64` truncates a 59-byte name to 58 bytes (`do_getdents64` uses `strlcpy`).
- 1024 inodes, fixed by mkfs. A 64-block log per filesystem.
- Up to four disks (`MAX_DISKS`), but only two are ever mounted: the one labelled `root` (or disk 0) on `/` and the one labelled `home` on `/home`. There is no way to mount another disk or another path, and only disks 0 and 1 have `/dev` nodes. If two disks carry the same label, the lower-numbered one wins silently.
- All disks share the 128 buffers of the cache.
- Directories never shrink.

**Pipes:**
- `O_NONBLOCK` is ignored, whether given to `pipe2` or set later with `fcntl`.
- Writes of at most 4096 bytes are not atomic with respect to other writers (no `PIPE_BUF` guarantee).
- A zero-length `read` on an empty pipe that still has writers blocks.

**`ppoll`:**
- The signal mask argument is ignored.
- At most 64 descriptors.
- Wait registrations beyond 128 are silently dropped, which would lose wakeups.

**procfs:**
- Output is truncated at 64 KiB with no indication.
- All generation is serialized by one mutex, and generating `/proc/<pid>/fds` does disk I/O under it.
- The process list in `readdir` can skip or repeat entries when processes come and go.

**devfs:**
- `/dev/disk0` cannot be seeked (`lseek` gives `ESPIPE` because it has no snapshot), so it can only be read sequentially from offset 0.
- `disk_rd` returns `-EIO` for a zero-length read and for a user-copy fault.

**fsck:**
- It does not replay or examine a committed log, and it does not check the contents of the log data blocks.
- It accepts any `nlog` and `ninodes` without checking that they fit their regions.
- Its `dir_find` (mkfs side) handles only direct and single-indirect directory blocks.

**Mount-time validation:** `efs_mount` checks only magic, block size and version. A damaged superblock with bad region boundaries is trusted. This now matters more, since `home.img` persists across kernel changes.

**Bugs found while reading:**
- `vfs_read_all` reads `ip->type` after `iunlockput(ip)` on its error path, a use after free when that was the last reference.
- `vfs_getcwd` stops after 64 levels and returns the partial path as success. It also calls `strlen(de.name)` before checking `found`, which reads an uninitialized buffer when the parent directory has no entries.
- There is a window between `free_inode` and the final `--refcnt` in `iput`. A concurrent `efs_alloc_inode` can claim the just-freed inode number and `iget` the stale in-memory inode, which is still `valid` with `type == I_FREE`. `create_in` would then write the inode back as free.
- `DBG_FSCRASH` has no privilege check, so any user can stop the machine at the next commit.

## Potential improvements

1. **Propagate I/O errors through bcache and the log**:
   - Leave a buffer invalid when `bread`'s `blk_rw` fails, and return an error to the caller.
   - Check `bwrite` results in `commit`, `install` and `write_head`, and do not write the commit point if any log block write failed. Mark the filesystem read-only or panic on a failed install.

   Today a failed read is cached as valid data, and a failed log write can still be "committed". Impact: high. Effort: medium.
2. **Restore a single lock order for transactions**: done; `vfs_open` (`O_TRUNC`), `chmod_inode`, `do_fchownat` and `do_utimensat` now call `fs_begin_op` before `ilock` (see Concurrency).
3. **Close the inode free/reuse race**:
   - In `iput`, invalidate the inode (`valid = false`) and unlink it from `icache` *before* releasing the on-disk inode, or hold off the bitmap/inode-table update until `refcnt` reaches 0.
   - Alternatively, have `efs_alloc_inode` refuse an inode number that is still in the cache.

   This prevents a new file from being written back as `EFS_T_FREE`. Impact: high. Effort: small.
4. **Fix the use after free in `vfs_read_all`**: save `ip->type` before calling `iunlockput`. Impact: medium. Effort: small.
5. **Use `alloc_lock` (or atomics) for `bhint` and the free counters**: the lock is already declared in `struct efs_info`. Unsynchronized `nfree_blocks--`/`++` can drift under SMP, and `mounts` and the ktest rely on the counter. Impact: medium. Effort: small.
6. **Bound transaction size in truncate and unlink**:
   - Stop logging indirect blocks that `free_ind` is about to free.
   - Free large files in several transactions: truncate in steps, or keep an orphan list as ext3 does.
   - Add a debug check in `log_write` that panics when one operation exceeds `MAXOPBLOCKS`, so oversize operations are caught on small images too.

   Today, deleting a big file on a large image can panic with "transaction too big". Impact: medium. Effort: medium.
7. **Make `fsync` and `sync` mean something**:
   - Record the transaction `seq` each operation joined.
   - Have `fsync`/`sync` (and optionally `O_SYNC`) wait until that `seq` has committed, and force a commit when no other operation is running.
   - Add `VIRTIO_BLK_T_FLUSH` around the commit point if the device offers it.

   Impact: medium. Effort: medium.
8. **Ordered-mode journaling for file data**: write data blocks to their home locations before the commit instead of logging them. Only metadata (bitmap, inodes, indirect blocks, directories) would go through the log. This halves write traffic for bulk writes, lets one transaction carry much more file data than 16 KiB, and stops large writes from consuming log space. Impact: high. Effort: large.
9. **Keep unreferenced inodes cached and hash lookups**:
   - Retain inodes with `refcnt == 0` on an LRU instead of `kfree`ing them in `iput`.
   - Hash `icache` and `bget` by (sb, ino) and blockno instead of scanning lists under a spinlock with IRQs off.

   Path lookups currently re-read every dinode. Impact: medium. Effort: medium.
10. **Validate the superblock and log geometry at mount**:
    - In `efs_mount`, check `nlog == EFS_LOGSIZE + 1`, that `ninodes` fits in `bmapstart - inodestart` blocks, that the bitmap covers `size`, region ordering, and `size ≤ blk_capacity()/8`.
    - Add the same checks to fsck, and let fsck optionally replay (or at least validate) a committed log.

    Impact: medium. Effort: small.
11. **Pipe semantics**:
    - Honor `O_NONBLOCK`, both from `pipe2` flags and from `F_SETFL`, returning `-EAGAIN`.
    - Return 0 immediately for zero-length reads.
    - Make writes of ≤ `PIPE_SIZE` bytes atomic by waiting until the whole write fits.

    Programs like shells and `select` loops depend on these. Impact: medium. Effort: small.
12. **Fix the `getdents64` name truncation**: widen `struct dirent.d_name` to 60 bytes, or reject 59-byte names in `efs_link`, so that every name that can be created can also be listed intact. This is an ABI change (`lib/include/uapi/fs.h`, `user/` libc). Impact: low. Effort: small.
13. **Harden `vfs_getcwd`**: return `-ENAMETOOLONG` when the depth limit is hit instead of returning a partial path, and do not touch `de` unless an entry was found. Impact: low. Effort: small.
14. **Restrict the crash-injection hook**: require `euid == 0` for `DBG_FSCRASH` in `kernel/proc/syscall.c`, so unprivileged users (e.g. `guest`) cannot stop the machine. Impact: low. Effort: small.
15. **A mount table instead of two fixed labels**: read `/etc/fstab` (label → path) from the root filesystem in `vfs_init`, or add a `mount` system call taking a disk number or label; give every disk a `/dev/diskN` node; and report a distinct `st_dev` per superblock so `(st_dev, st_ino)` identifies a file. Impact: medium. Effort: medium.
16. **Better observability for the log**:
    - Expose the per-transaction block high-water mark per operation type, and the time threads spend in `"log space"`.
    - Add a trace event for `log_begin` waits.
    - Have `eu-files` in `tools/gdb/eunuchs.py` handle `f->ip == NULL` (pipes and sockets) instead of dereferencing it.

    This makes log stalls and transaction growth visible without a debugger. Impact: low. Effort: small.
