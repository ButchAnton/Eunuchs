#include <fs.h>
#include <kstring.h>
#include <ktest.h>
#include <mm.h>
#include <uapi/errno.h>

/* Kernel-side file helpers (kernel buffers: user = false). */
static int kwrite(const char *path, const void *data, size_t n, u64 off)
{
    int err;
    struct file *f = vfs_open(path, O_RDWR | O_CREAT, 0644, &err);
    if (!f)
        return err;
    f->off = off;
    long r = file_write(f, data, n, false);
    file_close(f);
    return r == (long)n ? 0 : -EIO;
}

static long kread(const char *path, void *buf, size_t n, u64 off)
{
    int err;
    struct file *f = vfs_open(path, O_RDONLY, 0, &err);
    if (!f)
        return err;
    f->off = off;
    long r = file_read(f, buf, n, false);
    file_close(f);
    return r;
}

KTEST(fs_ops_release_buffers_and_inodes)
{
    int inodes = icache_busy();
    KT_EXPECT_EQ(bcache_busy(), 0);

    KT_ASSERT(vfs_mkdir("/tmp/kt", 0755) == 0);
    KT_EXPECT_EQ(kwrite("/tmp/kt/a", "hello", 5, 0), 0);
    char buf[16] = { 0 };
    KT_EXPECT_EQ(kread("/tmp/kt/a", buf, sizeof(buf), 0), 5);
    KT_EXPECT_STREQ(buf, "hello");
    KT_EXPECT_EQ(vfs_rename("/tmp/kt/a", "/tmp/kt/b"), 0);
    KT_EXPECT_EQ(vfs_unlink("/tmp/kt", true), -ENOTEMPTY);
    KT_EXPECT_EQ(vfs_unlink("/tmp/kt/b", false), 0);
    KT_EXPECT_EQ(vfs_unlink("/tmp/kt", true), 0);

    /* Every buffer released, every inode reference dropped. */
    KT_EXPECT_EQ(bcache_busy(), 0);
    KT_EXPECT_EQ(icache_busy(), inodes);
}

KTEST(fs_sparse_file_and_truncate)
{
    struct super_block *sb = root_inode->sb;
    u32 free0 = efs_free_blocks(sb);

    /* One byte at 5 MiB: data + indirect + double-indirect blocks only. */
    KT_ASSERT(kwrite("/tmp/kt_sparse", "Z", 1, 5UL << 20) == 0);
    u32 used = free0 - efs_free_blocks(sb);
    KT_EXPECT(used >= 3 && used <= 4);

    char buf[8];
    memset(buf, 0x55, sizeof(buf));
    KT_EXPECT_EQ(kread("/tmp/kt_sparse", buf, sizeof(buf), 12345), 8);
    KT_EXPECT(memchr_inv(buf, 0, sizeof(buf)) == NULL);            /* holes read as zeros */
    KT_EXPECT_EQ(kread("/tmp/kt_sparse", buf, 4, 5UL << 20), 1);
    KT_EXPECT_EQ(buf[0], 'Z');

    int err;
    struct file *f = vfs_open("/tmp/kt_sparse", O_WRONLY | O_TRUNC, 0, &err);
    KT_ASSERT(f != NULL);
    file_close(f);
    KT_EXPECT_EQ(efs_free_blocks(sb), free0);                       /* all blocks back */
    KT_EXPECT_EQ(vfs_unlink("/tmp/kt_sparse", false), 0);
}

KTEST(fs_path_walk)
{
    int err = 0;
    struct inode *ip = namei("/", &err);
    KT_EXPECT(ip == root_inode);
    iput(ip);
    ip = namei("/dev/..", &err);                    /* out of a mounted fs */
    KT_EXPECT(ip == root_inode);
    iput(ip);
    ip = namei("/dev/null", &err);
    KT_ASSERT(ip != NULL);
    ilock(ip);
    KT_EXPECT_EQ(ip->type, I_CHR);
    iunlockput(ip);

    char name[EFS_NAMELEN + 1];
    ip = nameiparent("/tmp/newfile", name, &err);
    KT_ASSERT(ip != NULL);
    KT_EXPECT_STREQ(name, "newfile");
    iput(ip);
    KT_EXPECT(namei("/no/such/path", &err) == NULL && err == -ENOENT);
    KT_EXPECT(namei("/etc/motd/x", &err) == NULL && err == -ENOTDIR);
    KT_EXPECT(nameiparent("/", name, &err) == NULL);
}

KTEST(fs_no_errors_recorded)
{
    KT_EXPECT_EQ(root_inode->sb->errors, 0);
    /* The in-memory free count must match the on-disk bitmap. */
    KT_EXPECT_EQ(efs_free_blocks_counter(root_inode->sb), efs_free_blocks(root_inode->sb));
}
