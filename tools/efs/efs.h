/*
 * efs on the host: a complete read-write implementation of the Eunuchs
 * filesystem (format: kernel/include/efs_format.h) over a volume held in
 * memory. Used by mkfs and eudisk (tools/eudisk), which load an image, change
 * it, and write it back.
 *
 * Conventions shared with the kernel: an inode's nlink is the number of
 * directory entries naming it (a directory: "." and its parent's entry, plus
 * each subdirectory's ".."); a free inode has type EFS_T_FREE; a removed
 * directory entry has ino 0; the free-block bitmap marks metadata and every
 * block an inode owns.
 *
 * Functions return 0 (or an inode number) on success and a negative errno
 * value on failure.
 */
#ifndef EUNUCHS_HOST_EFS_H
#define EUNUCHS_HOST_EFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "efs_format.h"

struct efs {
    uint8_t *base;              /* block 0 of the volume */
    uint32_t nblocks;           /* blocks in the buffer */
    struct efs_super *sb;
    uint64_t now;               /* ns since the epoch, for timestamps */
    uint32_t replayed;          /* blocks a committed log transaction installed at open */
};

/* Format the volume at base (nblocks blocks): an empty root directory. */
int efs_format(struct efs *fs, uint8_t *base, uint32_t nblocks, const char *label);

/* Use the efs volume at base; with replay, a committed transaction left in
   the log is installed (as the kernel does at mount: do it before changing
   anything). -EINVAL if it is not efs. */
int efs_open(struct efs *fs, uint8_t *base, uint32_t nblocks, bool replay);

uint8_t *efs_block(struct efs *fs, uint32_t b);
struct efs_dinode *efs_inode(struct efs *fs, uint32_t ino);

/* Files: read, write (growing as needed), truncate. */
long efs_read(struct efs *fs, uint32_t ino, uint64_t off, void *buf, size_t n);
int efs_write(struct efs *fs, uint32_t ino, uint64_t off, const void *buf, size_t n);
int efs_truncate(struct efs *fs, uint32_t ino, uint64_t size);

/* Paths are absolute ("/a/b"); "" and "/" are the root. */
int efs_lookup(struct efs *fs, const char *path);            /* ino or -ENOENT ... */
int efs_lookup_in(struct efs *fs, uint32_t dir, const char *name);

/* Directory entry number i of dir (counting free slots too): 1 if there is
   one (ino 0 for a free slot), 0 past the end. */
int efs_dirent(struct efs *fs, uint32_t dir, uint32_t i, struct efs_dirent *out);

/* Create: a new empty file or directory (-EEXIST if path exists). */
int efs_create(struct efs *fs, const char *path, uint16_t type, uint16_t mode, uint16_t uid,
               uint16_t gid);
/* mkdir -p: the directories of path that are missing (mode 0755, root). */
int efs_mkdirs(struct efs *fs, const char *path);
/* A hard link at path to the file at target. */
int efs_link(struct efs *fs, const char *target, const char *path);
/* Remove a file (its inode goes when no entry is left) or an empty directory. */
int efs_unlink(struct efs *fs, const char *path);

/* A file's whole contents: malloc'ed, length in *len (NULL on error). */
uint8_t *efs_slurp(struct efs *fs, uint32_t ino, size_t *len);
/* Replace a file's contents (keeping its inode, so its hard links too). */
int efs_replace(struct efs *fs, uint32_t ino, const void *data, size_t len);

struct efs_usage {
    uint32_t files, dirs, free_inodes;
    uint32_t data_blocks, used_blocks, free_blocks;
};
void efs_usage(struct efs *fs, struct efs_usage *u);

/* Check everything (as fsck); prints to out; returns the number of errors. */
int efs_check(struct efs *fs, FILE *out);

/* Apply mkfs-style specs: "/path=hostfile", "/path=@/existing" (hard link),
   "/dir/[:mode[:uid[:gid]]]". Programs keep 0755, other files get 0644 (the
   host's execute bit). Errors are printed; returns -1 on the first. */
int efs_apply_spec(struct efs *fs, const char *spec);

const char *efs_strerror(int err);

#endif
