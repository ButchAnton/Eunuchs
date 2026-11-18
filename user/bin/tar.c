/*
 * tar: ustar archives.
 *
 *   tar -c [-zvh] [-f ARCHIVE] [-C DIR] [--exclude PATTERN] FILE...
 *   tar -x [-zvkmpO] [-f ARCHIVE] [-C DIR] [MEMBER...]
 *   tar -t [-zv] [-f ARCHIVE] [MEMBER...]
 *
 * The first argument may also be the old bundle without a dash (tar czvf
 * a.tgz dir). Without -f the archive is standard input or output. -z runs
 * the data through /bin/gzip; when reading a file, a gzip archive is
 * recognised without -z. Names longer than ustar's 100+155 characters get a
 * GNU ././@LongLink entry; pax headers are read for their path. Hard links
 * are stored once and as links after that; symbolic links as links (followed
 * with -h). Leading '/' and ".." components are taken out of names.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BLOCK 512
#define RECORD (20 * BLOCK)

struct header {
    char name[100], mode[8], uid[8], gid[8], size[12], mtime[12], chksum[8], typeflag;
    char linkname[100], magic[6], version[2], uname[32], gname[32], devmajor[8], devminor[8];
    char prefix[155], pad[12];
};

static char mode;                       /* c x t */
static int verbose, gz, keep_old, no_mtime, to_stdout, deref, rc;
static const char *archive, *chdir_to;
static const char *excludes[32];
static int nexcludes;
static int fd = -1;                     /* the archive (or the pipe to/from gzip) */
static pid_t gz_pid;
static unsigned long long written;
static struct stat archive_st;
static int archive_is_file;

static void die(const char *fmt, const char *arg)
{
    eprintf("tar: ");
    eprintf(fmt, arg);
    eprintf("\n");
    exit(2);
}

static void usage(void)
{
    eprintf("usage: tar -c [-zvh] [-f ARCHIVE] [-C DIR] [--exclude PATTERN] FILE...\n"
            "       tar -x [-zvkmpO] [-f ARCHIVE] [-C DIR] [MEMBER...]\n"
            "       tar -t [-zv] [-f ARCHIVE] [MEMBER...]\n");
    exit(2);
}

/* ---- the archive stream ---- */

static void write_all(const void *p, size_t n)
{
    const char *c = p;
    while (n) {
        long w = write(fd, c, n);
        if (w <= 0)
            die("write error: %s", strerror(errno));
        c += w, n -= w;
    }
    written += c - (const char *)p;
}

static int read_full(void *p, size_t n)
{
    char *c = p;
    size_t got = 0;
    while (got < n) {
        long r = read(fd, c + got, n - got);
        if (r < 0)
            die("read error: %s", strerror(errno));
        if (r == 0)
            break;
        got += r;
    }
    return got;
}

/* Start gzip between us and the archive: compress (writing) or decompress. */
static void start_gzip(int raw, int writing)
{
    int p[2];
    if (pipe(p) < 0)
        die("pipe: %s", strerror(errno));
    gz_pid = fork();
    if (gz_pid < 0)
        die("fork: %s", strerror(errno));
    if (gz_pid == 0) {
        if (writing) {
            dup2(p[0], 0);
            dup2(raw, 1);
        } else {
            dup2(raw, 0);
            dup2(p[1], 1);
        }
        close(p[0]);
        close(p[1]);
        if (raw > 1)
            close(raw);
        execl("/bin/gzip", "gzip", writing ? "-c" : "-dc", (char *)0);
        execlp("gzip", "gzip", writing ? "-c" : "-dc", (char *)0);
        eprintf("tar: cannot run /bin/gzip: %s\n", strerror(errno));
        _exit(2);
    }
    if (raw > 1)
        close(raw);
    if (writing) {
        close(p[0]);
        fd = p[1];
    } else {
        close(p[1]);
        fd = p[0];
    }
}

static void finish_gzip(void)
{
    if (!gz_pid)
        return;
    close(fd);
    int st;
    waitpid(gz_pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) {
        eprintf("tar: gzip failed\n");
        rc = 2;
    }
}

static void open_archive(int writing)
{
    int raw;
    if (!archive || !strcmp(archive, "-")) {
        raw = writing ? 1 : 0;
        if (writing && isatty(1))
            die("refusing to write an archive to a terminal%s", "");
    } else {
        raw = writing ? open(archive, O_WRONLY | O_CREAT | O_TRUNC, 0644) : open(archive, O_RDONLY);
        if (raw < 0)
            die("%s: cannot open", archive);
        if (fstat(raw, &archive_st) == 0)
            archive_is_file = 1;
        if (!writing && !gz) {
            unsigned char m[2];
            if (read(raw, m, 2) == 2 && m[0] == 0x1f && m[1] == 0x8b)
                gz = 1;                 /* recognised by its magic number */
            lseek(raw, 0, SEEK_SET);
        }
    }
    if (gz)
        start_gzip(raw, writing);
    else
        fd = raw;
}

/* ---- headers ---- */

static void octal(char *field, int size, unsigned long long v)
{
    snprintf(field, size, "%0*llo", size - 1, v);
}

static unsigned long long from_octal(const char *f, int size)
{
    unsigned long long v = 0;
    int i = 0;
    while (i < size && (f[i] == ' ' || f[i] == 0))
        i++;
    for (; i < size && f[i] >= '0' && f[i] <= '7'; i++)
        v = v * 8 + (f[i] - '0');
    return v;
}

static unsigned checksum(const struct header *h)
{
    const unsigned char *p = (const unsigned char *)h;
    unsigned sum = 0;
    for (int i = 0; i < BLOCK; i++)
        sum += i >= 148 && i < 156 ? ' ' : p[i];
    return sum;
}

static void finish_header(struct header *h)
{
    memcpy(h->magic, "ustar", 6);
    memcpy(h->version, "00", 2);
    snprintf(h->chksum, sizeof(h->chksum), "%06o", checksum(h));
    h->chksum[7] = ' ';
}

/* Put NAME into the header: name, or prefix/name; 0 if it doesn't fit. */
static int set_name(struct header *h, const char *name)
{
    size_t n = strlen(name);
    if (n <= 100) {
        memcpy(h->name, name, n);
        return 1;
    }
    for (const char *s = name + n - 1; s > name; s--) {
        if (*s != '/')
            continue;
        size_t pre = s - name, rest = n - pre - 1;
        if (pre <= 155 && rest <= 100 && rest > 0) {
            memcpy(h->prefix, name, pre);
            memcpy(h->name, s + 1, rest);
            return 1;
        }
    }
    return 0;
}

static void write_longname(const char *name, char type)
{
    struct header h;
    memset(&h, 0, sizeof(h));
    strcpy(h.name, "././@LongLink");
    octal(h.mode, 8, 0644);
    octal(h.uid, 8, 0);
    octal(h.gid, 8, 0);
    octal(h.size, 12, strlen(name) + 1);
    octal(h.mtime, 12, 0);
    h.typeflag = type;
    finish_header(&h);
    write_all(&h, BLOCK);
    size_t n = strlen(name) + 1;
    char block[BLOCK];
    for (size_t off = 0; off < n; off += BLOCK) {
        memset(block, 0, BLOCK);
        memcpy(block, name + off, n - off < BLOCK ? n - off : BLOCK);
        write_all(block, BLOCK);
    }
}

/* ---- create ---- */

static struct { unsigned dev, ino; char *name; } links[1024];
static int nlinks;

static int excluded(const char *name)
{
    const char *base = strrchr(name, '/') ? strrchr(name, '/') + 1 : name;
    for (int i = 0; i < nexcludes; i++)
        if (!fnmatch(excludes[i], name, 0) || !fnmatch(excludes[i], base, 0))
            return 1;
    return 0;
}

static const char *member_name(const char *path)
{
    static int warned;
    const char *s = path;
    while (*s == '/')
        s++;
    if (s != path && !warned++)
        eprintf("tar: Removing leading `/' from member names\n");
    while (s[0] == '.' && s[1] == '/')
        s += 2;
    return *s ? s : ".";
}

static void add(const char *path)
{
    struct stat st;
    if ((deref ? stat(path, &st) : lstat(path, &st)) < 0) {
        eprintf("tar: %s: %s\n", path, strerror(errno));
        rc = 2;
        return;
    }
    if (excluded(path))
        return;
    if (archive_is_file && st.st_dev == archive_st.st_dev && st.st_ino == archive_st.st_ino) {
        eprintf("tar: %s: file is the archive; not dumped\n", path);
        return;
    }
    char name[PATH_MAX + 2];
    snprintf(name, sizeof(name), "%s%s", member_name(path), S_ISDIR(st.st_mode) ? "/" : "");
    if (S_ISDIR(st.st_mode) && !strcmp(name, "./"))
        snprintf(name, sizeof(name), "./");
    struct header h;
    memset(&h, 0, sizeof(h));
    octal(h.mode, 8, st.st_mode & 07777);
    octal(h.uid, 8, st.st_uid);
    octal(h.gid, 8, st.st_gid);
    octal(h.mtime, 12, st.st_mtime);
    struct passwd *pw = getpwuid(st.st_uid);
    if (pw)
        snprintf(h.uname, sizeof(h.uname), "%s", pw->pw_name);
    struct group *gr = getgrgid(st.st_gid);
    if (gr)
        snprintf(h.gname, sizeof(h.gname), "%s", gr->gr_name);
    unsigned long long size = 0;
    const char *link_target = 0;
    char symtarget[PATH_MAX];
    if (S_ISLNK(st.st_mode)) {
        long n = readlink(path, symtarget, sizeof(symtarget) - 1);
        if (n < 0) {
            eprintf("tar: %s: %s\n", path, strerror(errno));
            rc = 2;
            return;
        }
        symtarget[n] = 0;
        h.typeflag = '2';
        link_target = symtarget;
    } else if (S_ISDIR(st.st_mode)) {
        h.typeflag = '5';
    } else if (S_ISREG(st.st_mode)) {
        h.typeflag = '0';
        size = st.st_size;
        if (st.st_nlink > 1) {
            for (int i = 0; i < nlinks; i++)
                if (links[i].dev == st.st_dev && links[i].ino == st.st_ino)
                    link_target = links[i].name;
            if (link_target) {
                h.typeflag = '1';
                size = 0;
            } else if (nlinks < 1024) {
                links[nlinks].dev = st.st_dev;
                links[nlinks].ino = st.st_ino;
                links[nlinks++].name = strdup(name);
            }
        }
    } else if (S_ISCHR(st.st_mode) || S_ISFIFO(st.st_mode)) {
        h.typeflag = S_ISCHR(st.st_mode) ? '3' : '6';
        octal(h.devmajor, 8, st.st_rdev >> 8);
        octal(h.devminor, 8, st.st_rdev & 0xff);
    } else {
        eprintf("tar: %s: unsupported file type; not dumped\n", path);
        return;
    }
    octal(h.size, 12, size);
    if (link_target) {
        if (strlen(link_target) > 100)
            write_longname(link_target, 'K');
        memcpy(h.linkname, link_target, strlen(link_target) < 100 ? strlen(link_target) : 100);
    }
    if (!set_name(&h, name)) {
        write_longname(name, 'L');
        memcpy(h.name, name, 100);
    }
    finish_header(&h);
    int in = -1;
    if (h.typeflag == '0' && (in = open(path, O_RDONLY)) < 0) {
        eprintf("tar: %s: %s\n", path, strerror(errno));
        rc = 2;
        return;
    }
    write_all(&h, BLOCK);
    if (verbose)
        eprintf("%s\n", name);
    if (in >= 0) {
        char buf[16 * BLOCK];
        unsigned long long left = size;
        while (left) {
            size_t want = left < sizeof(buf) ? left : sizeof(buf);
            long r = read(in, buf, want);
            if (r <= 0) {
                /* shrank while we read: pad with zeros to keep the archive whole */
                eprintf("tar: %s: file shrank; padding with zeros\n", path);
                memset(buf, 0, sizeof(buf));
                r = want;
                rc = 1;
            }
            size_t pad = (r + BLOCK - 1) / BLOCK * BLOCK;
            if (pad > (size_t)r)
                memset(buf + r, 0, pad - r);
            write_all(buf, left <= (unsigned long long)r ? pad : (size_t)r);
            left -= r;
        }
        close(in);
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) {
            eprintf("tar: %s: %s\n", path, strerror(errno));
            rc = 2;
            return;
        }
        char **names = 0;
        int n = 0, cap = 0;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            if (n == cap)
                names = realloc(names, (cap = cap ? cap * 2 : 32) * sizeof(char *));
            names[n++] = strdup(e->d_name);
        }
        closedir(d);
        for (int i = 0; i < n; i++) {
            char child[PATH_MAX];
            size_t pl = strlen(path);
            snprintf(child, sizeof(child), "%s%s%s", path, pl && path[pl - 1] == '/' ? "" : "/", names[i]);
            add(child);
            free(names[i]);
        }
        free(names);
    }
}

static void create(int argc, char **argv)
{
    if (!argc)
        die("cowardly refusing to create an empty archive%s", "");
    open_archive(1);
    if (chdir_to && chdir(chdir_to) < 0)
        die("%s: cannot change directory", chdir_to);
    for (int i = 0; i < argc; i++)
        add(argv[i]);
    /* two zero blocks, then pad to a whole record */
    char zero[BLOCK] = { 0 };
    write_all(zero, BLOCK);
    write_all(zero, BLOCK);
    while (written % RECORD)
        write_all(zero, BLOCK);
    finish_gzip();
}

/* ---- extract and list ---- */

static int matches_member(const char *name, int argc, char **argv)
{
    if (!argc)
        return 1;
    for (int i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]);
        while (n > 1 && argv[i][n - 1] == '/')
            n--;
        if (!strncmp(name, argv[i], n) && (name[n] == 0 || name[n] == '/'))
            return 1;
        if (!fnmatch(argv[i], name, 0))
            return 1;
    }
    return 0;
}

/* Make NAME safe to create: no leading '/', no ".." components. */
static int safe_name(char *name)
{
    char *s = name;
    while (*s == '/')
        s++;
    if (s != name)
        memmove(name, s, strlen(s) + 1);
    for (char *p = name; *p;) {
        char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == 2 && p[0] == '.' && p[1] == '.')
            return 0;
        p += len + (e ? 1 : 0);
    }
    return 1;
}

static int mkdirs(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        struct stat st;
        if (stat(path, &st) < 0 && mkdir(path, 0755) < 0 && errno != EEXIST) {
            *p = '/';
            return -1;
        }
        *p = '/';
    }
    return 0;
}

static void list_line(const struct header *h, const char *name, unsigned long long size)
{
    if (!verbose) {
        printf("%s\n", name);
        return;
    }
    char m[11];
    unsigned mode = from_octal(h->mode, 8);
    char t = h->typeflag;
    m[0] = t == '5' ? 'd' : t == '1' ? 'h' : t == '2' ? 'l' : t == '3' ? 'c' : t == '4' ? 'b' : t == '6' ? 'p' : '-';
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        m[i + 1] = mode & (0400 >> i) ? rwx[i] : '-';
    m[10] = 0;
    char owner[80];
    if (h->uname[0])
        snprintf(owner, sizeof(owner), "%.32s/%.32s", h->uname, h->gname[0] ? h->gname : "?");
    else
        snprintf(owner, sizeof(owner), "%llu/%llu", from_octal(h->uid, 8), from_octal(h->gid, 8));
    time_t mt = from_octal(h->mtime, 12);
    char when[32];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&mt));
    printf("%s %s %8llu %s %s", m, owner, size, when, name);
    if (t == '1')
        printf(" link to %.100s", h->linkname);
    else if (t == '2')
        printf(" -> %.100s", h->linkname);
    putchar('\n');
}

struct dirtime {
    char *path;
    time_t mtime;
    unsigned mode;
};

static void extract_or_list(int argc, char **argv)
{
    open_archive(0);
    if (chdir_to && mode == 'x' && chdir(chdir_to) < 0)
        die("%s: cannot change directory", chdir_to);
    struct header h;
    char *longname = 0, *longlink = 0;
    struct dirtime *dirs = 0;
    int ndirs = 0, cap = 0, zeros = 0, got_any = 0;
    int *found = calloc(argc + 1, sizeof(int));
    for (;;) {
        int n = read_full(&h, BLOCK);
        if (n == 0)
            break;
        if (n < BLOCK)
            die("unexpected end of archive%s", "");
        static const char zero[BLOCK];
        if (!memcmp(&h, zero, BLOCK)) {
            if (++zeros == 2)
                break;
            continue;
        }
        zeros = 0;
        if ((unsigned)from_octal(h.chksum, 8) != checksum(&h)) {
            if (!got_any)
                die("this does not look like a tar archive%s", "");
            die("skipping to next header is not supported; archive damaged%s", "");
        }
        got_any = 1;
        unsigned long long size = from_octal(h.size, 12);
        if (h.typeflag == 'L' || h.typeflag == 'K' || h.typeflag == 'x' || h.typeflag == 'g') {
            /* the data is a name (GNU) or pax records */
            char *data = malloc(size + BLOCK + 1);
            unsigned long long padded = (size + BLOCK - 1) / BLOCK * BLOCK;
            if ((unsigned long long)read_full(data, padded) < padded)
                die("unexpected end of archive%s", "");
            data[size] = 0;
            if (h.typeflag == 'L') {
                free(longname);
                longname = data;
            } else if (h.typeflag == 'K') {
                free(longlink);
                longlink = data;
            } else {
                for (char *rec = data; rec < data + size;) {
                    long len = strtol(rec, 0, 10);
                    if (len <= 0)
                        break;
                    char *kv = strchr(rec, ' ');
                    if (kv && kv < rec + len) {
                        kv++;
                        if (!strncmp(kv, "path=", 5) && h.typeflag == 'x') {
                            free(longname);
                            longname = strndup(kv + 5, rec + len - kv - 6);
                        } else if (!strncmp(kv, "linkpath=", 9) && h.typeflag == 'x') {
                            free(longlink);
                            longlink = strndup(kv + 9, rec + len - kv - 10);
                        }
                    }
                    rec += len;
                }
                free(data);
            }
            continue;
        }
        char name[PATH_MAX * 2];
        if (longname)
            snprintf(name, sizeof(name), "%s", longname);
        else if (h.prefix[0] && !memcmp(h.magic, "ustar", 5))
            snprintf(name, sizeof(name), "%.155s/%.100s", h.prefix, h.name);
        else
            snprintf(name, sizeof(name), "%.100s", h.name);
        char target[PATH_MAX];
        snprintf(target, sizeof(target), "%s", longlink ? longlink : "");
        if (!longlink)
            snprintf(target, sizeof(target), "%.100s", h.linkname);
        free(longname);
        free(longlink);
        longname = longlink = 0;
        char t = h.typeflag ? h.typeflag : '0';
        if (t == '7')
            t = '0';
        unsigned long long data_size = t == '0' ? size : (t == '1' || t == '2' || t == '5') ? 0 : size;
        int want = matches_member(name, argc, argv);
        if (want)
            for (int i = 0; i < argc; i++)
                if (matches_member(name, 1, argv + i))
                    found[i] = 1;
        int out = -1;
        if (want && mode == 't')
            list_line(&h, name, size);
        if (want && mode == 'x') {
            if (!safe_name(name)) {
                eprintf("tar: %s: member name contains '..'; skipped\n", name);
                rc = 2;
                want = 0;
            }
        }
        if (want && mode == 'x') {
            if (verbose && !to_stdout)
                printf("%s\n", name);
            size_t nl = strlen(name);
            while (nl > 1 && name[nl - 1] == '/')
                name[--nl] = 0;
            unsigned m = from_octal(h.mode, 8) & 07777;
            if (!to_stdout)
                mkdirs(name);
            struct stat st;
            if (to_stdout) {
                if (t == '0')
                    out = 1;
            } else if (t == '5') {
                if (mkdir(name, 0700) < 0 && (errno != EEXIST || stat(name, &st) < 0 || !S_ISDIR(st.st_mode))) {
                    eprintf("tar: %s: cannot mkdir: %s\n", name, strerror(errno));
                    rc = 2;
                } else {
                    if (ndirs == cap)
                        dirs = realloc(dirs, (cap = cap ? cap * 2 : 32) * sizeof(*dirs));
                    dirs[ndirs].path = strdup(name);
                    dirs[ndirs].mtime = from_octal(h.mtime, 12);
                    dirs[ndirs++].mode = m;
                }
            } else if (keep_old && stat(name, &st) == 0) {
                eprintf("tar: %s: already exists; not overwritten\n", name);
                rc = 2;
            } else if (t == '2') {
                unlink(name);
                if (symlink(target, name) < 0) {
                    eprintf("tar: %s: cannot make a symbolic link: %s\n", name, strerror(errno));
                    rc = 2;
                }
            } else if (t == '1') {
                safe_name(target);
                unlink(name);
                if (link(target, name) < 0) {
                    eprintf("tar: %s: cannot link to %s: %s\n", name, target, strerror(errno));
                    rc = 2;
                }
            } else if (t == '0') {
                unlink(name);
                out = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
                if (out < 0) {
                    eprintf("tar: %s: cannot open: %s\n", name, strerror(errno));
                    rc = 2;
                }
            } else {
                eprintf("tar: %s: unsupported member type '%c'; skipped\n", name, t);
                rc = 2;
            }
            /* contents, then mode, owner and time */
            if (data_size) {
                char buf[16 * BLOCK];
                unsigned long long left = data_size;
                while (left) {
                    size_t chunk = left < sizeof(buf) ? left : sizeof(buf);
                    size_t padded = (chunk + BLOCK - 1) / BLOCK * BLOCK;
                    if ((size_t)read_full(buf, padded) < padded)
                        die("unexpected end of archive%s", "");
                    if (out >= 0 && write(out, buf, chunk) != (long)chunk) {
                        eprintf("tar: %s: write error: %s\n", name, strerror(errno));
                        rc = 2;
                    }
                    left -= chunk;
                }
                data_size = 0;
            }
            if (out > 1) {
                close(out);
                chmod(name, m);
                if (getuid() == 0) {
                    unsigned uid = from_octal(h.uid, 8), gid = from_octal(h.gid, 8);
                    struct passwd *pw = h.uname[0] ? getpwnam(h.uname) : 0;
                    struct group *gr = h.gname[0] ? getgrnam(h.gname) : 0;
                    chown(name, pw ? pw->pw_uid : uid, gr ? gr->gr_gid : gid);
                }
                if (!no_mtime) {
                    struct timespec ts[2] = { { (time_t)from_octal(h.mtime, 12), 0 },
                                              { (time_t)from_octal(h.mtime, 12), 0 } };
                    utimensat(AT_FDCWD, name, ts, 0);
                }
            }
        }
        /* skip what wasn't read */
        if (data_size) {
            char buf[BLOCK];
            for (unsigned long long b = (data_size + BLOCK - 1) / BLOCK; b > 0; b--)
                if (read_full(buf, BLOCK) < BLOCK)
                    die("unexpected end of archive%s", "");
        }
    }
    /* directories last: their contents changed their times */
    for (int i = ndirs - 1; i >= 0; i--) {
        chmod(dirs[i].path, dirs[i].mode);
        if (!no_mtime) {
            struct timespec ts[2] = { { dirs[i].mtime, 0 }, { dirs[i].mtime, 0 } };
            utimensat(AT_FDCWD, dirs[i].path, ts, 0);
        }
    }
    if (gz_pid) {
        /* let gzip finish (it may still be writing the end of the stream) */
        char buf[4096];
        while (read(fd, buf, sizeof(buf)) > 0)
            ;
    }
    finish_gzip();
    for (int i = 0; i < argc; i++)
        if (!found[i]) {
            eprintf("tar: %s: not found in archive\n", argv[i]);
            rc = 2;
        }
}

int main(int argc, char **argv)
{
    int i = 1;
    /* the first argument: an old-style bundle (no dash) or options */
    for (; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "--")) {
            i++;
            break;
        }
        if (!strncmp(a, "--exclude", 9)) {
            const char *pat = a[9] == '=' ? a + 10 : i + 1 < argc ? argv[++i] : 0;
            if (!pat)
                usage();
            if (nexcludes < 32)
                excludes[nexcludes++] = pat;
            continue;
        }
        if (!strcmp(a, "--gzip")) { gz = 1; continue; }
        if (!strcmp(a, "--verbose")) { verbose = 1; continue; }
        int bundle = i == 1 && a[0] != '-';
        if (a[0] != '-' && !bundle)
            break;
        for (char *f = a + (bundle ? 0 : 1); *f; f++) {
            switch (*f) {
            case 'c': case 'x': case 't':
                if (mode && mode != *f)
                    die("you may not specify more than one of -c, -x and -t%s", "");
                mode = *f;
                break;
            case 'v': verbose = 1; break;
            case 'z': gz = 1; break;
            case 'k': keep_old = 1; break;
            case 'm': no_mtime = 1; break;
            case 'p': break;            /* modes are always restored */
            case 'O': to_stdout = 1; break;
            case 'h': deref = 1; break; /* follow symbolic links */
            case 'f':
            case 'C': {
                const char *v = f[1] && !bundle ? f + 1 : i + 1 < argc ? argv[++i] : 0;
                if (!v)
                    usage();
                if (*f == 'f')
                    archive = v;
                else
                    chdir_to = v;
                if (f[1] && !bundle)
                    f += strlen(f) - 1;
                break;
            }
            default:
                eprintf("tar: unknown option -- '%c'\n", *f);
                usage();
            }
        }
    }
    (void)deref;
    if (!mode)
        usage();
    if (mode == 'c')
        create(argc - i, argv + i);
    else
        extract_or_list(argc - i, argv + i);
    fflush(stdout);
    return rc;
}
