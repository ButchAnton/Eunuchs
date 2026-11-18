/*
 * fileutils: file and scripting tools in one binary, chosen by argv[0] (the
 * disk image has /bin/<tool> as hard links to it):
 *   du df dd basename dirname realpath readlink mktemp chgrp file sync truncate mount umount
 *   seq expr test [ printf
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <regex.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

static const char *prog;

static int fail(const char *what)
{
    eprintf("%s: %s: %s\n", prog, what, strerror(errno));
    return 1;
}

static void die_usage(const char *usage)
{
    eprintf("usage: %s %s\n", prog, usage);
    exit(prog[0] == 'e' || prog[0] == 't' || prog[0] == '[' ? 2 : 1);
}

/* ================================================================ du */

static int du_human, du_all, du_bytes, du_maxdepth = -1, du_summary;
static unsigned long du_seen[4096][2];  /* (dev, ino) of files with several links */
static int du_nseen;

static void du_print(unsigned long long v, const char *path)
{
    if (du_human) {
        static const char units[] = "KMGT";
        double x = du_bytes ? v / 1024.0 : (double)v;
        if (du_bytes && v < 1024) {
            printf("%llu\t%s\n", v, path);
            return;
        }
        int u = 0;
        while (x >= 1024 && u < 3)
            x /= 1024, u++;
        if (x < 10)
            printf("%.1f%c\t%s\n", x, units[u], path);
        else
            printf("%.0f%c\t%s\n", x + 0.5 - (x - (long)x == 0 ? 0.5 : 0), units[u], path);
    } else {
        printf("%llu\t%s\n", v, path);
    }
}

static unsigned long long du_walk(const char *path, int depth, int *err)
{
    struct stat st;
    if ((depth ? lstat(path, &st) : stat(path, &st)) < 0) {   /* links inside: not followed */
        fail(path);
        *err = 1;
        return 0;
    }
    if (st.st_nlink > 1 && !S_ISDIR(st.st_mode)) {
        for (int i = 0; i < du_nseen; i++)
            if (du_seen[i][0] == st.st_dev && du_seen[i][1] == st.st_ino)
                return 0;               /* a hard link counted already */
        if (du_nseen < 4096) {
            du_seen[du_nseen][0] = st.st_dev;
            du_seen[du_nseen++][1] = st.st_ino;
        }
    }
    unsigned long long total = du_bytes ? st.st_size : (st.st_blocks + 1) / 2;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) {
            fail(path);
            *err = 1;
        } else {
            struct dirent *e;
            char child[PATH_MAX];
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                    continue;
                size_t pl = strlen(path);
                snprintf(child, sizeof(child), "%s%s%s", path, pl && path[pl - 1] == '/' ? "" : "/", e->d_name);
                total += du_walk(child, depth + 1, err);
            }
            closedir(d);
        }
        if (!du_summary && (du_maxdepth < 0 || depth <= du_maxdepth))
            du_print(total, path);
    } else if (!du_summary && (depth == 0 || (du_all && (du_maxdepth < 0 || depth <= du_maxdepth)))) {
        du_print(total, path);          /* files: named ones, or all with -a */
    }
    return total;
}

static int du_main(int argc, char **argv)
{
    int c, grand = 0, err = 0;
    while ((c = getopt(argc, argv, "ahsbckd:")) != -1) {
        switch (c) {
        case 'a': du_all = 1; break;
        case 'h': du_human = 1; break;
        case 's': du_summary = 1; break;
        case 'b': du_bytes = 1; break;
        case 'c': grand = 1; break;
        case 'k': du_human = 0; break;
        case 'd': du_maxdepth = atoi(optarg); break;
        default: die_usage("[-ahsbck] [-d DEPTH] [FILE...]");
        }
    }
    static char *dot[] = { "." };
    char **paths = optind < argc ? argv + optind : dot;
    int n = optind < argc ? argc - optind : 1;
    unsigned long long sum = 0;
    for (int i = 0; i < n; i++) {
        unsigned long long t = du_walk(paths[i], 0, &err);
        if (du_summary)
            du_print(t, paths[i]);
        sum += t;
    }
    if (grand)
        du_print(sum, "total");
    return err;
}

/* ================================================================ dd */

static long long dd_size(const char *s, const char *what)
{
    char *end;
    long long v = strtoll(s, &end, 10);
    if (end == s || v < 0)
        goto bad;
    for (;;) {
        switch (*end) {
        case 0: return v;
        case 'c': end++; break;
        case 'w': v *= 2; end++; break;
        case 'b': v *= 512; end++; break;
        case 'k': case 'K': v *= 1024; end++; break;
        case 'M': v *= 1024 * 1024; end++; break;
        case 'G': v *= 1024LL * 1024 * 1024; end++; break;
        case 'x': {
            long long m = dd_size(end + 1, what);
            return v * m;
        }
        default: goto bad;
        }
        if (*end == 'B' && end[1] == 0)
            end++;
    }
bad:
    eprintf("dd: invalid number for %s: '%s'\n", what, s);
    exit(1);
}

static int dd_main(int argc, char **argv)
{
    const char *ifile = 0, *ofile = 0;
    long long bs = 0, ibs = 512, obs = 512, count = -1, skip = 0, seek = 0;
    int notrunc = 0, noerror = 0, sync_ = 0, ucase = 0, lcase = 0, quiet = 0, noxfer = 0, fsync_ = 0;
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) {
            eprintf("dd: unrecognized operand '%s'\n", argv[i]);
            return 1;
        }
        *eq = 0;
        const char *k = argv[i], *v = eq + 1;
        if (!strcmp(k, "if")) ifile = v;
        else if (!strcmp(k, "of")) ofile = v;
        else if (!strcmp(k, "bs")) bs = dd_size(v, k);
        else if (!strcmp(k, "ibs")) ibs = dd_size(v, k);
        else if (!strcmp(k, "obs")) obs = dd_size(v, k);
        else if (!strcmp(k, "count")) count = dd_size(v, k);
        else if (!strcmp(k, "skip")) skip = dd_size(v, k);
        else if (!strcmp(k, "seek")) seek = dd_size(v, k);
        else if (!strcmp(k, "status")) {
            quiet = !strcmp(v, "none");
            noxfer = !strcmp(v, "noxfer");
        } else if (!strcmp(k, "conv")) {
            char *copy = strdup(v), *save, *t;
            for (t = strtok_r(copy, ",", &save); t; t = strtok_r(0, ",", &save)) {
                if (!strcmp(t, "notrunc")) notrunc = 1;
                else if (!strcmp(t, "noerror")) noerror = 1;
                else if (!strcmp(t, "sync")) sync_ = 1;
                else if (!strcmp(t, "ucase")) ucase = 1;
                else if (!strcmp(t, "lcase")) lcase = 1;
                else if (!strcmp(t, "fsync") || !strcmp(t, "fdatasync")) fsync_ = 1;
                else {
                    eprintf("dd: invalid conversion: '%s'\n", t);
                    return 1;
                }
            }
            free(copy);
        } else {
            eprintf("dd: unrecognized operand '%s'\n", k);
            return 1;
        }
    }
    if (bs)
        ibs = obs = bs;
    if (ibs < 1 || obs < 1) {
        eprintf("dd: invalid block size\n");
        return 1;
    }
    int in = 0, out = 1;
    if (ifile && (in = open(ifile, O_RDONLY)) < 0)
        return fail(ifile);
    /* with seek (and without notrunc) the data before the seek point stays,
       and the file is cut there */
    if (ofile && (out = open(ofile, O_WRONLY | O_CREAT | (notrunc || seek ? 0 : O_TRUNC), 0666)) < 0)
        return fail(ofile);
    if (ofile && seek && !notrunc) {
        struct stat ost;
        if (fstat(out, &ost) == 0 && S_ISREG(ost.st_mode) && ftruncate(out, seek * obs) < 0)
            return fail(ofile);
    }
    if (skip && lseek(in, skip * ibs, SEEK_CUR) < 0) {
        char *tmp = malloc(ibs);
        for (long long i = 0; i < skip; i++)
            if (read(in, tmp, ibs) <= 0)
                break;
        free(tmp);
    }
    if (seek && lseek(out, seek * obs, SEEK_SET) < 0)
        return fail(ofile ? ofile : "standard output");
    char *ib = malloc(ibs), *ob = malloc(obs + ibs);
    long long in_full = 0, in_part = 0, out_full = 0, out_part = 0, bytes = 0;
    size_t on = 0;
    unsigned long t0 = uptime_ms();
    int rc = 0;
    while (count < 0 || in_full + in_part < count) {
        long n = read(in, ib, ibs);
        if (n < 0) {
            fail(ifile ? ifile : "standard input");
            rc = 1;
            if (!noerror)
                break;
            continue;
        }
        if (n == 0)
            break;
        if (n == ibs)
            in_full++;
        else
            in_part++;
        if (sync_ && n < ibs) {
            memset(ib + n, 0, ibs - n);
            n = ibs;
        }
        for (long i = 0; i < n; i++)
            ib[i] = ucase ? toupper((unsigned char)ib[i]) : lcase ? tolower((unsigned char)ib[i]) : ib[i];
        if (bs) {
            /* bs=: each input block is written as it came */
            long w = write(out, ib, n);
            if (w < 0) {
                fail(ofile ? ofile : "standard output");
                rc = 1;
                break;
            }
            if (w == obs)
                out_full++;
            else
                out_part++;
            bytes += w;
            continue;
        }
        memcpy(ob + on, ib, n);
        on += n;
        while (on >= (size_t)obs) {
            long w = write(out, ob, obs);
            if (w < 0) {
                fail(ofile ? ofile : "standard output");
                return 1;
            }
            out_full++;
            bytes += w;
            memmove(ob, ob + obs, on - obs);
            on -= obs;
        }
    }
    if (on) {
        long w = write(out, ob, on);
        if (w > 0) {
            out_part++;
            bytes += w;
        }
    }
    if (fsync_)
        fsync(out);
    if (ofile)
        close(out);
    if (!quiet) {
        eprintf("%lld+%lld records in\n%lld+%lld records out\n", in_full, in_part, out_full, out_part);
        if (!noxfer) {
            unsigned long ms = uptime_ms() - t0;
            eprintf("%lld bytes copied, %lu.%03lu s", bytes, ms / 1000, ms % 1000);
            if (ms)
                eprintf(", %lld B/s", bytes * 1000 / (long long)ms);
            eprintf("\n");
        }
    }
    return rc;
}

/* ================================================================ basename, dirname, realpath */

static void strip_trailing_slashes(char *s)
{
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '/')
        s[--n] = 0;
}

static void print_basename(const char *arg, const char *suffix)
{
    char *s = strdup(arg);
    strip_trailing_slashes(s);
    char *b = strcmp(s, "/") && strrchr(s, '/') ? strrchr(s, '/') + 1 : s;
    size_t bl = strlen(b), sl = suffix ? strlen(suffix) : 0;
    if (sl && bl > sl && !strcmp(b + bl - sl, suffix))
        b[bl - sl] = 0;
    puts(b);
    free(s);
}

static int basename_main(int argc, char **argv)
{
    int c, all = 0;
    const char *suffix = 0;
    while ((c = getopt(argc, argv, "as:")) != -1) {
        if (c == 'a') all = 1;
        else if (c == 's') suffix = optarg, all = 1;
        else die_usage("NAME [SUFFIX]  |  -a [-s SUFFIX] NAME...");
    }
    if (optind >= argc)
        die_usage("NAME [SUFFIX]  |  -a [-s SUFFIX] NAME...");
    if (!all) {
        if (argc - optind > 2)
            die_usage("NAME [SUFFIX]  |  -a [-s SUFFIX] NAME...");
        print_basename(argv[optind], optind + 1 < argc ? argv[optind + 1] : 0);
        return 0;
    }
    for (int i = optind; i < argc; i++)
        print_basename(argv[i], suffix);
    return 0;
}

static int dirname_main(int argc, char **argv)
{
    if (argc < 2)
        die_usage("NAME...");
    for (int i = 1; i < argc; i++) {
        char *s = strdup(argv[i]);
        strip_trailing_slashes(s);
        char *slash = strrchr(s, '/');
        if (!slash)
            puts(".");
        else {
            while (slash > s && slash[-1] == '/')
                slash--;
            if (slash == s)
                puts("/");
            else {
                *slash = 0;
                puts(s);
            }
        }
        free(s);
    }
    return 0;
}

/* Normalize PATH (absolute; ".", ".." and repeated slashes removed). */
static void normalize(const char *path, char *out, size_t size)
{
    char buf[PATH_MAX * 2];
    if (path[0] == '/')
        snprintf(buf, sizeof(buf), "%s", path);
    else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd)))
            strcpy(cwd, "/");
        snprintf(buf, sizeof(buf), "%s/%s", cwd, path);
    }
    size_t o = 0;
    out[o++] = '/';
    for (char *t = buf; *t;) {
        while (*t == '/')
            t++;
        if (!*t)
            break;
        char *e = t;
        while (*e && *e != '/')
            e++;
        size_t len = e - t;
        if (len == 1 && t[0] == '.') {
        } else if (len == 2 && t[0] == '.' && t[1] == '.') {
            while (o > 1 && out[o - 1] != '/')
                o--;
            if (o > 1)
                o--;
        } else if (o + len + 2 < size) {
            if (o > 1)
                out[o++] = '/';
            memcpy(out + o, t, len);
            o += len;
        }
        t = e;
    }
    out[o] = 0;
}

static int realpath_main(int argc, char **argv)
{
    int c, missing_ok = 0, all_exist = 0, quiet = 0, rc = 0;
    while ((c = getopt(argc, argv, "emq")) != -1) {
        if (c == 'e') all_exist = 1;
        else if (c == 'm') missing_ok = 1;
        else if (c == 'q') quiet = 1;
        else die_usage("[-e | -m] [-q] PATH...");
    }
    if (optind >= argc)
        die_usage("[-e | -m] [-q] PATH...");
    for (int i = optind; i < argc; i++) {
        char out[PATH_MAX];
        if (realpath(argv[i], out)) {   /* exists: links resolved */
            puts(out);
            continue;
        }
        normalize(argv[i], out, sizeof(out));
        struct stat st;
        if (!missing_ok && stat(out, &st) < 0) {
            /* without -e, the last component may be missing (GNU) */
            char parent[PATH_MAX];
            snprintf(parent, sizeof(parent), "%s", out);
            char *slash = strrchr(parent, '/');
            if (slash)
                *(slash == parent ? slash + 1 : slash) = 0;
            if (all_exist || stat(parent, &st) < 0 || !S_ISDIR(st.st_mode)) {
                if (!quiet)
                    fail(argv[i]);
                rc = 1;
                continue;
            }
        }
        puts(out);
    }
    return rc;
}

/* readlink [-f | -e | -m] [-n] FILE...: a link's target, or (-f -e -m) the canonical path */
static int readlink_main(int argc, char **argv)
{
    int c, canon = 0, no_newline = 0, rc = 0;
    while ((c = getopt(argc, argv, "femnqs")) != -1) {
        switch (c) {
        case 'f': case 'e': case 'm': canon = c; break;
        case 'n': no_newline = 1; break;
        case 'q': case 's': break;
        default: die_usage("[-f | -e | -m] [-n] FILE...");
        }
    }
    if (optind >= argc)
        die_usage("[-f | -e | -m] [-n] FILE...");
    for (int i = optind; i < argc; i++) {
        char out[PATH_MAX];
        if (canon) {
            if (realpath(argv[i], out)) {
                printf("%s%s", out, no_newline ? "" : "\n");
                continue;
            }
            if (canon == 'e') {
                rc = 1;
                continue;
            }
            /* -f: the last component may be missing; -m: any may */
            char dir[PATH_MAX], res[PATH_MAX];
            snprintf(dir, sizeof(dir), "%s", argv[i]);
            char *slash = strrchr(dir, '/');
            const char *base = slash ? slash + 1 : argv[i];
            if (slash)
                *(slash == dir ? slash + 1 : slash) = 0;
            else
                strcpy(dir, ".");
            if (realpath(dir, res)) {
                size_t l = strlen(res);
                printf("%s%s%s%s", res, l > 1 ? "/" : "", base, no_newline ? "" : "\n");
            } else if (canon == 'm') {
                normalize(argv[i], out, sizeof(out));
                printf("%s%s", out, no_newline ? "" : "\n");
            } else {
                rc = 1;
            }
            continue;
        }
        long n = readlink(argv[i], out, sizeof(out) - 1);
        if (n < 0) {
            rc = 1;                     /* not a link: silently, as GNU readlink */
            continue;
        }
        out[n] = 0;
        printf("%s%s", out, no_newline ? "" : "\n");
    }
    return rc;
}

/* ================================================================ mktemp */

static int mktemp_main(int argc, char **argv)
{
    int c, dir = 0, dry = 0, quiet = 0, tmpdir = 0;
    const char *base = 0;
    while ((c = getopt(argc, argv, "dup:qt")) != -1) {
        switch (c) {
        case 'd': dir = 1; break;
        case 'u': dry = 1; break;
        case 'p': base = optarg; tmpdir = 1; break;
        case 'q': quiet = 1; break;
        case 't': tmpdir = 1; break;
        default: die_usage("[-dqtu] [-p DIR] [TEMPLATE]");
        }
    }
    const char *tmpl = optind < argc ? argv[optind] : "tmp.XXXXXXXXXX";
    if (optind >= argc)
        tmpdir = 1;
    char path[PATH_MAX];
    if (tmpdir && !strchr(tmpl, '/')) {
        if (!base)
            base = getenv("TMPDIR");
        if (!base || !*base)
            base = "/tmp";
        size_t bl = strlen(base);
        snprintf(path, sizeof(path), "%s%s%s", base, bl && base[bl - 1] == '/' ? "" : "/", tmpl);
    } else {
        snprintf(path, sizeof(path), "%s", tmpl);
    }
    size_t n = strlen(path), x = n;
    while (x > 0 && path[x - 1] == 'X')
        x--;
    if (n - x < 3) {
        if (!quiet)
            eprintf("mktemp: too few X's in template '%s'\n", tmpl);
        return 1;
    }
    /* the library wants at least six: pad short templates while we work */
    char work[PATH_MAX];
    snprintf(work, sizeof(work), "%s%s", path, n - x < 6 ? "XXXXXX" + (n - x) : "");
    int r;
    if (dry)
        r = mktemp(work) && *work ? 0 : -1;
    else if (dir)
        r = mkdtemp(work) ? 0 : -1;
    else {
        int fd = mkstemp(work);
        r = fd < 0 ? -1 : 0;
        if (fd >= 0)
            close(fd);
    }
    if (r < 0) {
        if (!quiet)
            eprintf("mktemp: failed to create %s via template '%s': %s\n", dir ? "directory" : "file",
                    path, strerror(errno));
        return 1;
    }
    if (n - x < 6) {
        /* rename onto the requested length */
        char final[PATH_MAX];
        snprintf(final, sizeof(final), "%.*s", (int)n, work);
        if (!dry && rename(work, final) < 0) {
            fail(work);
            return 1;
        }
        puts(final);
    } else {
        puts(work);
    }
    return 0;
}

/* ================================================================ chgrp */

static int chgrp_one(const char *path, int gid, int recurse)
{
    int rc = 0;
    if (chown(path, -1, gid) < 0)
        rc = fail(path);
    struct stat st;
    if (recurse && stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d)
            return fail(path);
        struct dirent *e;
        char child[PATH_MAX];
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            rc |= chgrp_one(child, gid, 1);
        }
        closedir(d);
    }
    return rc;
}

static int chgrp_main(int argc, char **argv)
{
    int c, recurse = 0, rc = 0;
    while ((c = getopt(argc, argv, "R")) != -1) {
        if (c == 'R') recurse = 1;
        else die_usage("[-R] GROUP FILE...");
    }
    if (argc - optind < 2)
        die_usage("[-R] GROUP FILE...");
    const char *g = argv[optind];
    char *end;
    long gid = strtol(g, &end, 10);
    if (end == g || *end) {
        struct group *gr = getgrnam(g);
        if (!gr) {
            eprintf("chgrp: invalid group: '%s'\n", g);
            return 1;
        }
        gid = gr->gr_gid;
    }
    for (int i = optind + 1; i < argc; i++)
        rc |= chgrp_one(argv[i], gid, recurse);
    return rc;
}

/* ================================================================ file */

static const char *elf_machine(int m)
{
    switch (m) {
    case 243: return "UCB RISC-V";
    case 62: return "x86-64";
    case 3: return "Intel 80386";
    case 183: return "ARM aarch64";
    case 40: return "ARM";
    default: return "unknown machine";
    }
}

static void file_elf(const unsigned char *b, long n, int fd)
{
    int is64 = b[4] == 2, le = b[5] == 1;
#define U16(o) (le ? b[o] | b[(o) + 1] << 8 : b[o] << 8 | b[(o) + 1])
    int type = U16(16), mach = U16(18);
    const char *t = type == 1 ? "relocatable" : type == 2 ? "executable" : type == 3 ? "shared object"
                  : type == 4 ? "core file" : "unknown type";
    printf("ELF %s-bit %s %s, %s", is64 ? "64" : "32", le ? "LSB" : "MSB", t, elf_machine(mach));
    if (!is64 || !le || n < 64) {
        putchar('\n');
        return;
    }
    /* program headers: dynamic or static; section headers: symbols, DWARF */
    uint64_t phoff, shoff;
    memcpy(&phoff, b + 32, 8);
    memcpy(&shoff, b + 40, 8);
    int phnum = U16(56), shnum = U16(60), shentsize = U16(58), shstrndx = U16(62);
    int interp = 0, symtab = 0, debug = 0;
    for (int i = 0; i < phnum && type == 2; i++) {
        unsigned char ph[56];
        if (lseek(fd, phoff + i * 56, SEEK_SET) < 0 || read(fd, ph, 56) != 56)
            break;
        uint32_t ptype;
        memcpy(&ptype, ph, 4);
        if (ptype == 3)
            interp = 1;
    }
    unsigned char sh[64];
    uint64_t stroff = 0, strsize = 0;
    if (shstrndx < shnum && lseek(fd, shoff + shstrndx * shentsize, SEEK_SET) >= 0 && read(fd, sh, 64) == 64) {
        memcpy(&stroff, sh + 24, 8);
        memcpy(&strsize, sh + 32, 8);
    }
    char *names = strsize && strsize < 65536 ? malloc(strsize + 1) : 0;
    if (names && (lseek(fd, stroff, SEEK_SET) < 0 || read(fd, names, strsize) != (long)strsize)) {
        free(names);
        names = 0;
    }
    for (int i = 0; i < shnum; i++) {
        if (lseek(fd, shoff + (uint64_t)i * shentsize, SEEK_SET) < 0 || read(fd, sh, 64) != 64)
            break;
        uint32_t name, stype;
        memcpy(&name, sh, 4);
        memcpy(&stype, sh + 4, 4);
        if (stype == 2)
            symtab = 1;
        if (names && name < strsize) {
            names[strsize] = 0;
            if (!strncmp(names + name, ".debug_info", 11))
                debug = 1;
        }
    }
    free(names);
    if (type == 2)
        printf(", %s", interp ? "dynamically linked" : "statically linked");
    if (debug)
        printf(", with debug_info");
    printf(", %s\n", symtab ? "not stripped" : "stripped");
#undef U16
}

static int file_one(const char *path)
{
    printf("%s: ", path);
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) {
        char target[PATH_MAX];
        long n = readlink(path, target, sizeof(target) - 1);
        target[n > 0 ? n : 0] = 0;
        printf("%ssymbolic link to %s\n", stat(path, &st) < 0 ? "broken " : "", target);
        return 0;
    }
    if (stat(path, &st) < 0) {
        printf("cannot open `%s' (%s)\n", path, strerror(errno));
        return 1;
    }
    if (S_ISDIR(st.st_mode)) { puts("directory"); return 0; }
    if (S_ISCHR(st.st_mode)) { puts("character special"); return 0; }
    if (S_ISBLK(st.st_mode)) { puts("block special"); return 0; }
    if (S_ISFIFO(st.st_mode)) { puts("fifo (named pipe)"); return 0; }
    if (!st.st_size) { puts("empty"); return 0; }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("cannot open `%s' (%s)\n", path, strerror(errno));
        return 1;
    }
    unsigned char b[4096];
    long n = read(fd, b, sizeof(b));
    if (n <= 0) {
        close(fd);
        puts("data");
        return 0;
    }
    if (n >= 52 && !memcmp(b, "\177ELF", 4)) {
        file_elf(b, n, fd);
        close(fd);
        return 0;
    }
    close(fd);
#define MAGIC(s) (n >= (long)sizeof(s) - 1 && !memcmp(b, s, sizeof(s) - 1))
    if (MAGIC("\x1f\x8b")) { puts("gzip compressed data"); return 0; }
    if (n > 262 && !memcmp(b + 257, "ustar", 5)) { puts("POSIX tar archive"); return 0; }
    if (MAGIC("PK\3\4")) { puts("Zip archive data"); return 0; }
    if (MAGIC("\x89PNG\r\n\x1a\n")) { puts("PNG image data"); return 0; }
    if (MAGIC("\xff\xd8\xff")) { puts("JPEG image data"); return 0; }
    if (MAGIC("GIF8")) { puts("GIF image data"); return 0; }
    if (MAGIC("%PDF-")) { puts("PDF document"); return 0; }
    if (MAGIC("!<arch>\n")) { puts("current ar archive"); return 0; }
    if (MAGIC("BZh")) { puts("bzip2 compressed data"); return 0; }
    if (MAGIC("\xfd" "7zXZ")) { puts("XZ compressed data"); return 0; }
#undef MAGIC
    /* text? ASCII, else valid UTF-8, else data */
    int ascii = 1, utf8 = 1, crlf = 0;
    for (long i = 0; i < n; i++) {
        unsigned char c = b[i];
        if (c == '\r' && i + 1 < n && b[i + 1] == '\n')
            crlf = 1;
        if (c < 0x80) {
            if (c < 32 && !strchr("\t\n\r\f\b\033\a\v", c))
                ascii = utf8 = 0;
            continue;
        }
        ascii = 0;
        int len = c >= 0xf0 && c < 0xf8 ? 4 : c >= 0xe0 ? 3 : c >= 0xc2 && c < 0xe0 ? 2 : 0;
        if (!len) {
            utf8 = 0;
            continue;
        }
        for (int k = 1; k < len; k++)
            if (i + k < n && (b[i + k] & 0xc0) != 0x80)
                utf8 = 0;
        i += len - 1;
    }
    if (!ascii && !utf8) {
        puts("data");
        return 0;
    }
    const char *kind = ascii ? "ASCII text" : "UTF-8 Unicode text";
    const char *dot = strrchr(path, '.');
    if (n > 2 && b[0] == '#' && b[1] == '!') {
        char interp[64] = "";
        sscanf((char *)b + 2, " %63s", interp);
        const char *base = strrchr(interp, '/') ? strrchr(interp, '/') + 1 : interp;
        printf("%s script, %s executable", base, kind);
    } else if (dot && (!strcmp(dot, ".c") || !strcmp(dot, ".h"))) {
        printf("C source, %s", kind);
    } else if (dot && (!strcmp(dot, ".S") || !strcmp(dot, ".s"))) {
        printf("assembler source, %s", kind);
    } else if (n > 5 && (!strncasecmp((char *)b, "<html", 5) || !strncasecmp((char *)b, "<!doct", 6))) {
        printf("HTML document, %s", kind);
    } else {
        printf("%s", kind);
    }
    printf("%s\n", crlf ? ", with CRLF line terminators" : "");
    return 0;
}

static int file_main(int argc, char **argv)
{
    if (argc < 2)
        die_usage("FILE...");
    int rc = 0;
    for (int i = 1; i < argc; i++)
        rc |= file_one(argv[i]);
    return rc;
}

/* ================================================================ df, mount, umount */

struct mnt {
    char source[64], dir[256], type[32];
};

/* /proc/mounts */
static int read_mounts(struct mnt **out)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) {
        fail("/proc/mounts");
        return -1;
    }
    struct mnt *v = 0;
    int n = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        v = realloc(v, (n + 1) * sizeof(*v));
        if (sscanf(line, "%63s %255s %31s", v[n].source, v[n].dir, v[n].type) == 3)
            n++;
    }
    fclose(f);
    *out = v;
    return n;
}

static void human(unsigned long long bytes, char *b, size_t n)
{
    static const char units[] = "BKMGT";
    double x = bytes;
    int u = 0;
    while (x >= 1024 && u < 4)
        x /= 1024, u++;
    if (u == 0)
        snprintf(b, n, "%llu", bytes);
    else if (x < 10)
        snprintf(b, n, "%.1f%c", x, units[u]);
    else
        snprintf(b, n, "%.0f%c", x, units[u]);
}

static int df_main(int argc, char **argv)
{
    int c, hum = 0, inodes = 0, types = 0, all = 0, rc = 0;
    while ((c = getopt(argc, argv, "hikTa")) != -1) {
        switch (c) {
        case 'h': hum = 1; break;
        case 'i': inodes = 1; break;
        case 'k': hum = 0; break;
        case 'T': types = 1; break;
        case 'a': all = 1; break;
        default: die_usage("[-ahikT] [FILE...]");
        }
    }
    struct mnt *m;
    int n = read_mounts(&m);
    if (n < 0)
        return 1;
    /* which mounts: those named (the one each FILE is on), or all */
    int *show = calloc(n + 1, sizeof(int));
    if (optind < argc) {
        for (int i = optind; i < argc; i++) {
            char path[PATH_MAX];
            if (!realpath(argv[i], path)) {
                rc = fail(argv[i]);
                continue;
            }
            int best = -1;
            size_t blen = 0;
            for (int k = 0; k < n; k++) {
                size_t l = strlen(m[k].dir);
                if (!strncmp(path, m[k].dir, l) && (path[l] == '/' || !path[l] || l == 1) && l >= blen)
                    best = k, blen = l;
            }
            if (best >= 0)
                show[best] = 1;
        }
    } else {
        for (int k = 0; k < n; k++)
            show[k] = 1;
    }
    if (inodes)
        printf("%-14s%s %9s %9s %9s %5s %s\n", "Filesystem", types ? " Type " : "", "Inodes", "IUsed", "IFree",
               "IUse%", "Mounted on");
    else
        printf("%-14s%s %9s %9s %9s %5s %s\n", "Filesystem", types ? " Type " : "", hum ? "Size" : "1K-blocks",
               "Used", "Available", "Use%", "Mounted on");
    for (int k = 0; k < n; k++) {
        if (!show[k])
            continue;
        struct statfs st;
        if (statfs(m[k].dir, &st) < 0) {
            rc = fail(m[k].dir);
            continue;
        }
        if (!st.f_blocks && !all && optind >= argc)
            continue;                   /* devfs, proc, ... */
        char tcol[16] = "";
        if (types)
            snprintf(tcol, sizeof(tcol), " %-5s", m[k].type);
        if (inodes) {
            unsigned long used = st.f_files - st.f_ffree;
            char pct[8] = "-";
            if (st.f_files)
                snprintf(pct, sizeof(pct), "%lu%%", (used * 100 + st.f_files - 1) / st.f_files);
            printf("%-14s%s %9lu %9lu %9lu %5s %s\n", m[k].source, tcol, st.f_files, used, st.f_ffree, pct,
                   m[k].dir);
            continue;
        }
        unsigned long long bs = st.f_frsize ? st.f_frsize : st.f_bsize;
        unsigned long long total = st.f_blocks * bs, freeb = st.f_bavail * bs,
                           used = (st.f_blocks - st.f_bfree) * bs;
        char pct[8] = "-";
        if (used + freeb)
            snprintf(pct, sizeof(pct), "%llu%%", (used * 100 + used + freeb - 1) / (used + freeb));
        if (hum) {
            char a[16], b[16], d[16];
            human(total, a, sizeof(a));
            human(used, b, sizeof(b));
            human(freeb, d, sizeof(d));
            printf("%-14s%s %9s %9s %9s %5s %s\n", m[k].source, tcol, a, b, d, pct, m[k].dir);
        } else {
            printf("%-14s%s %9llu %9llu %9llu %5s %s\n", m[k].source, tcol, total / 1024, used / 1024,
                   freeb / 1024, pct, m[k].dir);
        }
    }
    free(show);
    free(m);
    return rc;
}

static int mount_main(int argc, char **argv)
{
    int c;
    const char *type = 0;
    while ((c = getopt(argc, argv, "t:o:")) != -1) {
        if (c == 't')
            type = optarg;
        else if (c != 'o')
            die_usage("[-t TYPE] SOURCE DIR  |  mount");
    }
    if (optind == argc) {
        struct mnt *m;
        int n = read_mounts(&m);
        if (n < 0)
            return 1;
        for (int k = 0; k < n; k++)
            printf("%s on %s type %s (rw)\n", m[k].source, m[k].dir, m[k].type);
        free(m);
        return 0;
    }
    if (argc - optind != 2)
        die_usage("[-t TYPE] SOURCE DIR  |  mount");
    const char *src = argv[optind], *dir = argv[optind + 1];
    if (!type)
        type = !strcmp(src, "proc") ? "proc" : !strcmp(src, "devfs") ? "devfs" : "efs";
    if (mount(src, dir, type, 0, 0) < 0) {
        eprintf("mount: %s on %s: %s\n", src, dir,
                errno == ENODEV ? "no such volume or filesystem type" : strerror(errno));
        return 1;
    }
    return 0;
}

static int umount_main(int argc, char **argv)
{
    if (argc < 2)
        die_usage("DIR|SOURCE...");
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        const char *target = argv[i];
        char dir[256];
        if (target[0] != '/') {
            /* a source (disk0p4): where it is mounted */
            struct mnt *m;
            int n = read_mounts(&m);
            for (int k = 0; k < n; k++)
                if (!strcmp(m[k].source, target)) {
                    snprintf(dir, sizeof(dir), "%s", m[k].dir);
                    target = dir;
                }
            if (n >= 0)
                free(m);
        }
        if (umount(target) < 0) {
            eprintf("umount: %s: %s\n", argv[i],
                    errno == EBUSY ? "target is busy" : errno == EINVAL ? "not mounted" : strerror(errno));
            rc = 1;
        }
    }
    return rc;
}

/* ================================================================ truncate */

static int truncate_main(int argc, char **argv)
{
    int c, no_create = 0, rc = 0;
    const char *size_arg = 0, *ref = 0;
    while ((c = getopt(argc, argv, "cs:r:")) != -1) {
        switch (c) {
        case 'c': no_create = 1; break;
        case 's': size_arg = optarg; break;
        case 'r': ref = optarg; break;
        default: die_usage("[-c] -s [+-<>/%]SIZE[KMG] | -r REFFILE FILE...");
        }
    }
    if ((!size_arg == !ref) || optind >= argc)
        die_usage("[-c] -s [+-<>/%]SIZE[KMG] | -r REFFILE FILE...");
    char op = 0;
    long long size = 0;
    if (ref) {
        struct stat st;
        if (stat(ref, &st) < 0)
            return fail(ref);
        size = st.st_size;
    } else {
        const char *s = size_arg;
        if (strchr("+-<>/%", *s))
            op = *s++;
        char *end;
        size = strtoll(s, &end, 10);
        if (end == s || size < 0) {
            eprintf("truncate: invalid size '%s'\n", size_arg);
            return 1;
        }
        long long unit = 1;
        if (*end) {
            int k = strchr("KMGT", toupper((unsigned char)*end)) ? (int)(strchr("KMGT", toupper((unsigned char)*end)) - "KMGT") + 1 : 0;
            if (!k) {
                eprintf("truncate: invalid size '%s'\n", size_arg);
                return 1;
            }
            int si = end[1] == 'B';     /* KB, MB: powers of 1000 */
            for (int i = 0; i < k; i++)
                unit *= si ? 1000 : 1024;
            if (end[1 + si] && !(end[1] == 'i' && end[2] == 'B' && !end[3])) {
                eprintf("truncate: invalid size '%s'\n", size_arg);
                return 1;
            }
        }
        size *= unit;
        if ((op == '/' || op == '%') && size == 0) {
            eprintf("truncate: division by zero\n");
            return 1;
        }
    }
    for (int i = optind; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            if (errno != ENOENT || no_create)
                continue;
            int fd = open(argv[i], O_WRONLY | O_CREAT, 0666);
            if (fd < 0) {
                rc = fail(argv[i]);
                continue;
            }
            close(fd);
            st.st_size = 0;
        }
        long long cur = st.st_size, want = size;
        switch (op) {
        case '+': want = cur + size; break;
        case '-': want = cur > size ? cur - size : 0; break;
        case '<': want = cur < size ? cur : size; break;
        case '>': want = cur > size ? cur : size; break;
        case '/': want = cur / size * size; break;
        case '%': want = (cur + size - 1) / size * size; break;
        }
        if (truncate(argv[i], want) < 0)
            rc = fail(argv[i]);
    }
    return rc;
}

/* ================================================================ seq */

static int decimals(const char *s)
{
    const char *dot = strchr(s, '.');
    if (!dot || strpbrk(s, "eE"))
        return 0;
    return strlen(dot + 1);
}

static int seq_main(int argc, char **argv)
{
    const char *fmt = 0, *sep = "\n";
    int equal = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] && !isdigit(argv[i][1]) && argv[i][1] != '.'; i++) {
        if (!strcmp(argv[i], "-w")) equal = 1;
        else if (!strncmp(argv[i], "-s", 2) && (argv[i][2] || i + 1 < argc)) sep = argv[i][2] ? argv[i] + 2 : argv[++i];
        else if (!strncmp(argv[i], "-f", 2) && (argv[i][2] || i + 1 < argc)) fmt = argv[i][2] ? argv[i] + 2 : argv[++i];
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else die_usage("[-w] [-s SEP] [-f FORMAT] [FIRST [INCREMENT]] LAST");
    }
    int n = argc - i;
    if (n < 1 || n > 3)
        die_usage("[-w] [-s SEP] [-f FORMAT] [FIRST [INCREMENT]] LAST");
    const char *a_first = n > 1 ? argv[i] : "1", *a_inc = n > 2 ? argv[i + 1] : "1", *a_last = argv[argc - 1];
    char *e1, *e2, *e3;
    double first = strtod(a_first, &e1), inc = strtod(a_inc, &e2), last = strtod(a_last, &e3);
    if (*e1 || *e2 || *e3) {
        eprintf("seq: invalid floating point argument\n");
        return 1;
    }
    if (inc == 0) {
        eprintf("seq: invalid Zero increment value: '%s'\n", a_inc);
        return 1;
    }
    int prec = decimals(a_first) > decimals(a_inc) ? decimals(a_first) : decimals(a_inc);
    char f[64];
    if (fmt) {
        snprintf(f, sizeof(f), "%s", fmt);
    } else if (equal) {
        /* the widest of first and last, as printed */
        char b1[64], b2[64];
        snprintf(b1, sizeof(b1), "%.*f", prec, first);
        snprintf(b2, sizeof(b2), "%.*f", prec, last);
        int w = strlen(b1) > strlen(b2) ? strlen(b1) : strlen(b2);
        snprintf(f, sizeof(f), "%%0%d.%df", w, prec);
    } else {
        snprintf(f, sizeof(f), "%%.%df", prec);
    }
    int count = 0;
    for (long k = 0;; k++) {
        double v = first + k * inc;
        if ((inc > 0 && v > last + 1e-10) || (inc < 0 && v < last - 1e-10))
            break;
        if (count++)
            fputs(sep, stdout);
        printf(f, v);
    }
    if (count)
        putchar('\n');
    return 0;
}

/* ================================================================ expr */

static char **ex_argv;
static int ex_argc, ex_i;

static void ex_syntax(void)
{
    eprintf("expr: syntax error\n");
    exit(2);
}

static int is_int(const char *s, long long *v)
{
    char *end;
    if (!*s)
        return 0;
    errno = 0;
    long long x = strtoll(s, &end, 10);
    if (*end || errno || (!isdigit(*s) && !((*s == '-' || *s == '+') && isdigit(s[1]))))
        return 0;
    if (v)
        *v = x;
    return 1;
}

static char *ex_num(long long v)
{
    char *s = malloc(24);
    snprintf(s, 24, "%lld", v);
    return s;
}

static int ex_null(const char *s)
{
    long long v;
    return !*s || (is_int(s, &v) && v == 0);
}

static char *ex_or(void);

static int ex_peek(const char *op)
{
    return ex_i < ex_argc && !strcmp(ex_argv[ex_i], op);
}

/* STRING : REGEX, anchored at the start: \(...\) gives the group, else the length matched. */
static char *ex_regmatch(const char *str, const char *pattern)
{
    regex_t re;
    char pat[1024];
    snprintf(pat, sizeof(pat), "^%s", pattern);
    if (regcomp(&re, pat, 0)) {
        eprintf("expr: invalid regular expression\n");
        exit(2);
    }
    regmatch_t m[2];
    char *r;
    if (regexec(&re, str, 2, m, 0))
        r = re.re_nsub ? strdup("") : ex_num(0);
    else if (re.re_nsub)
        r = m[1].rm_so >= 0 ? strndup(str + m[1].rm_so, m[1].rm_eo - m[1].rm_so) : strdup("");
    else
        r = ex_num(m[0].rm_eo);
    regfree(&re);
    return r;
}

static char *ex_primary(void)
{
    if (ex_i >= ex_argc)
        ex_syntax();
    const char *t = ex_argv[ex_i++];
    if (!strcmp(t, "(")) {
        char *v = ex_or();
        if (!ex_peek(")"))
            ex_syntax();
        ex_i++;
        return v;
    }
    if (!strcmp(t, "+")) {                          /* + TOKEN: the token as a string */
        if (ex_i >= ex_argc)
            ex_syntax();
        return strdup(ex_argv[ex_i++]);
    }
    if (!strcmp(t, "length")) {
        char *s = ex_primary();
        return ex_num(strlen(s));
    }
    if (!strcmp(t, "match") || !strcmp(t, "index") || !strcmp(t, "substr")) {
        char *s = ex_primary(), *a = ex_primary();
        if (t[0] == 'i') {
            size_t p = strcspn(s, a);
            return ex_num(s[p] ? (long long)p + 1 : 0);
        }
        if (t[0] == 's') {
            char *b = ex_primary();
            long long pos, len, sl = strlen(s);
            if (!is_int(a, &pos) || !is_int(b, &len) || pos < 1 || pos > sl || len < 1)
                return strdup("");
            if (len > sl - pos + 1)
                len = sl - pos + 1;
            return strndup(s + pos - 1, len);
        }
        return ex_regmatch(s, a);                   /* match: like ':' */
    }
    return strdup(t);
}

static char *ex_match(void)
{
    char *l = ex_primary();
    while (ex_peek(":")) {
        ex_i++;
        l = ex_regmatch(l, ex_primary());
    }
    return l;
}

static long long ex_int(const char *s)
{
    long long v;
    if (!is_int(s, &v)) {
        eprintf("expr: non-integer argument\n");
        exit(2);
    }
    return v;
}

static char *ex_mul(void)
{
    char *l = ex_match();
    while (ex_peek("*") || ex_peek("/") || ex_peek("%")) {
        char op = ex_argv[ex_i++][0];
        char *r = ex_match();
        long long a = ex_int(l), b = ex_int(r);
        if (op != '*' && b == 0) {
            eprintf("expr: division by zero\n");
            exit(2);
        }
        l = ex_num(op == '*' ? a * b : op == '/' ? a / b : a % b);
    }
    return l;
}

static char *ex_add(void)
{
    char *l = ex_mul();
    while (ex_peek("+") || ex_peek("-")) {
        char op = ex_argv[ex_i++][0];
        char *r = ex_mul();
        long long a = ex_int(l), b = ex_int(r);
        l = ex_num(op == '+' ? a + b : a - b);
    }
    return l;
}

static char *ex_cmp(void)
{
    char *l = ex_add();
    static const char *const ops[] = { "=", "==", "!=", "<", "<=", ">", ">=" };
    for (;;) {
        int k = -1;
        for (int i = 0; i < 7; i++)
            if (ex_peek(ops[i]))
                k = i;
        if (k < 0)
            return l;
        ex_i++;
        char *r = ex_add();
        long long a, b;
        int c = is_int(l, &a) && is_int(r, &b) ? (a > b) - (a < b) : strcmp(l, r);
        int res = k <= 1 ? c == 0 : k == 2 ? c != 0 : k == 3 ? c < 0 : k == 4 ? c <= 0 : k == 5 ? c > 0 : c >= 0;
        l = ex_num(res);
    }
}

static char *ex_and(void)
{
    char *l = ex_cmp();
    while (ex_peek("&")) {
        ex_i++;
        char *r = ex_cmp();
        l = ex_null(l) || ex_null(r) ? ex_num(0) : l;
    }
    return l;
}

static char *ex_or(void)
{
    char *l = ex_and();
    while (ex_peek("|")) {
        ex_i++;
        char *r = ex_and();
        l = !ex_null(l) ? l : !ex_null(r) ? r : ex_num(0);
    }
    return l;
}

static int expr_main(int argc, char **argv)
{
    ex_argv = argv + 1;
    ex_argc = argc - 1;
    if (ex_argc && !strcmp(ex_argv[0], "--"))
        ex_argv++, ex_argc--;
    if (!ex_argc)
        ex_syntax();
    char *v = ex_or();
    if (ex_i != ex_argc)
        ex_syntax();
    puts(v);
    return ex_null(v) ? 1 : 0;
}

/* ================================================================ test, [ */

static char **t_argv;
static int t_argc, t_i;

static void t_error(const char *msg, const char *arg)
{
    eprintf("%s: ", prog);
    eprintf(msg, arg);
    eprintf("\n");
    exit(2);
}

static long long t_int(const char *s)
{
    long long v;
    while (isspace(*s))
        s++;
    if (!is_int(s, &v))
        t_error("%s: integer expression expected", s);
    return v;
}

static int unary_op(const char *s)
{
    return s[0] == '-' && s[1] && !s[2] && strchr("bcdefgGhkLnOprsStuwxz", s[1]);
}

static int binary_op(const char *s)
{
    static const char *const ops[] = { "=", "==", "!=", "<", ">", "-eq", "-ne", "-lt", "-le",
                                       "-gt", "-ge", "-nt", "-ot", "-ef", 0 };
    for (int i = 0; ops[i]; i++)
        if (!strcmp(s, ops[i]))
            return 1;
    return 0;
}

static int t_unary(char op, const char *a)
{
    struct stat st;
    if (op == 'n') return *a != 0;
    if (op == 'z') return *a == 0;
    if (op == 't') return isatty(atoi(a));
    if (op == 'r') return access(a, R_OK) == 0;
    if (op == 'w') return access(a, W_OK) == 0;
    if (op == 'x') return access(a, X_OK) == 0;
    if (op == 'h' || op == 'L') return lstat(a, &st) == 0 && S_ISLNK(st.st_mode);
    if (stat(a, &st) < 0)
        return 0;
    switch (op) {
    case 'e': return 1;
    case 'f': return S_ISREG(st.st_mode);
    case 'd': return S_ISDIR(st.st_mode);
    case 'c': return S_ISCHR(st.st_mode);
    case 'b': return S_ISBLK(st.st_mode);
    case 'p': return S_ISFIFO(st.st_mode);
    case 'S': return S_ISSOCK(st.st_mode);
    case 's': return st.st_size > 0;
    case 'u': return (st.st_mode & S_ISUID) != 0;
    case 'g': return (st.st_mode & S_ISGID) != 0;
    case 'k': return (st.st_mode & S_ISVTX) != 0;
    case 'O': return st.st_uid == geteuid();
    case 'G': return st.st_gid == getegid();
    }
    return 0;
}

static int t_binary(const char *a, const char *op, const char *b)
{
    if (!strcmp(op, "=") || !strcmp(op, "==")) return !strcmp(a, b);
    if (!strcmp(op, "!=")) return strcmp(a, b) != 0;
    if (!strcmp(op, "<")) return strcmp(a, b) < 0;
    if (!strcmp(op, ">")) return strcmp(a, b) > 0;
    if (op[1] == 'n' && op[2] == 't' ) {
        struct stat x, y;
        if (stat(a, &x) < 0) return 0;
        if (stat(b, &y) < 0) return 1;
        return x.st_mtime > y.st_mtime || (x.st_mtime == y.st_mtime && x.st_mtim.tv_nsec > y.st_mtim.tv_nsec);
    }
    if (!strcmp(op, "-ot")) {
        struct stat x, y;
        if (stat(b, &y) < 0) return 0;
        if (stat(a, &x) < 0) return 1;
        return x.st_mtime < y.st_mtime || (x.st_mtime == y.st_mtime && x.st_mtim.tv_nsec < y.st_mtim.tv_nsec);
    }
    if (!strcmp(op, "-ef")) {
        struct stat x, y;
        return stat(a, &x) == 0 && stat(b, &y) == 0 && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
    }
    long long x = t_int(a), y = t_int(b);
    if (!strcmp(op, "-eq")) return x == y;
    if (!strcmp(op, "-ne")) return x != y;
    if (!strcmp(op, "-lt")) return x < y;
    if (!strcmp(op, "-le")) return x <= y;
    if (!strcmp(op, "-gt")) return x > y;
    return x >= y;
}

static int t_or(void);

static int t_primary(void)
{
    int left = t_argc - t_i;
    if (left <= 0)
        t_error("argument expected%s", "");
    const char *a = t_argv[t_i];
    /* a binary expression wins: test x = y, test -f = -f */
    if (left >= 3 && binary_op(t_argv[t_i + 1])) {
        t_i += 3;
        return t_binary(a, t_argv[t_i - 2], t_argv[t_i - 1]);
    }
    if (!strcmp(a, "!")) {
        t_i++;
        return !t_primary();
    }
    if (!strcmp(a, "(") && left >= 2) {
        t_i++;
        int v = t_or();
        if (t_i >= t_argc || strcmp(t_argv[t_i], ")"))
            t_error("')' expected%s", "");
        t_i++;
        return v;
    }
    if (unary_op(a) && left >= 2) {
        t_i += 2;
        return t_unary(a[1], t_argv[t_i - 1]);
    }
    t_i++;
    return *a != 0;
}

static int t_and(void)
{
    int v = t_primary();
    while (t_i < t_argc && !strcmp(t_argv[t_i], "-a")) {
        t_i++;
        int r = t_primary();
        v = v && r;
    }
    return v;
}

static int t_or(void)
{
    int v = t_and();
    while (t_i < t_argc && !strcmp(t_argv[t_i], "-o")) {
        t_i++;
        int r = t_and();
        v = v || r;
    }
    return v;
}

static int test_main(int argc, char **argv)
{
    if (prog[0] == '[') {
        if (argc < 2 || strcmp(argv[argc - 1], "]"))
            t_error("missing ']'%s", "");
        argc--;
    }
    t_argv = argv + 1;
    t_argc = argc - 1;
    t_i = 0;
    if (!t_argc)
        return 1;
    int v;
    /* POSIX: decided by the number of arguments up to four */
    if (t_argc == 1)
        v = *t_argv[0] != 0;
    else if (t_argc == 2 && !strcmp(t_argv[0], "!"))
        v = *t_argv[1] == 0;
    else if (t_argc == 2 && unary_op(t_argv[0]))
        v = t_unary(t_argv[0][1], t_argv[1]);
    else if (t_argc == 3 && binary_op(t_argv[1]))
        v = t_binary(t_argv[0], t_argv[1], t_argv[2]);
    else if (t_argc == 3 && !strcmp(t_argv[0], "!")) {
        t_i = 1;
        v = !t_primary();
        if (t_i != t_argc)
            t_error("too many arguments%s", "");
    } else {
        v = t_or();
        if (t_i != t_argc)
            t_error("%s: unexpected argument", t_argv[t_i]);
    }
    return v ? 0 : 1;
}

/* ================================================================ printf */

static int pf_status;

/* An escape at *P (just after the backslash); returns the byte, or -1 for
   \c (stop). OCT0: %b's \0NNN form. */
static int pf_escape(const char **p, int oct0)
{
    const char *s = *p;
    int c = (unsigned char)*s++;
    switch (c) {
    case 'a': c = '\a'; break;
    case 'b': c = '\b'; break;
    case 'c': *p = s; return -1;
    case 'e': c = 033; break;
    case 'f': c = '\f'; break;
    case 'n': c = '\n'; break;
    case 'r': c = '\r'; break;
    case 't': c = '\t'; break;
    case 'v': c = '\v'; break;
    case 'x': {
        int v = 0, k = 0;
        while (k < 2 && isxdigit(*s)) {
            v = v * 16 + (isdigit(*s) ? *s - '0' : tolower(*s) - 'a' + 10);
            s++, k++;
        }
        c = k ? v : 'x';
        if (!k)
            s--;
        if (!k) {
            *p = s;
            return '\\';
        }
        break;
    }
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
        int v = c - '0', max = oct0 && c == '0' ? 3 : 2;
        for (int k = 0; k < max && *s >= '0' && *s <= '7'; k++)
            v = v * 8 + (*s++ - '0');
        c = v & 0xff;
        break;
    }
    case '\\': case '"': case '\'': break;
    default:
        putchar('\\');
        break;
    }
    *p = s;
    return c;
}

static long long pf_int(const char *a)
{
    if (*a == '\'' || *a == '"')
        return (unsigned char)a[1];
    char *end;
    errno = 0;
    long long v = strtoll(a, &end, 0);
    if (end == a || *end || errno) {
        if (end == a || *end) {
            /* unsigned values past LLONG_MAX */
            unsigned long long u = strtoull(a, &end, 0);
            if (end != a && !*end)
                return (long long)u;
        }
        eprintf("printf: '%s': expected a numeric value\n", a);
        pf_status = 1;
    }
    return v;
}

static double pf_float(const char *a)
{
    if (*a == '\'' || *a == '"')
        return (unsigned char)a[1];
    char *end;
    double v = strtod(a, &end);
    if (end == a || *end) {
        eprintf("printf: '%s': expected a numeric value\n", a);
        pf_status = 1;
    }
    return v;
}

static int printf_main(int argc, char **argv)
{
    int i = 1;
    if (i < argc && !strcmp(argv[i], "--"))
        i++;
    if (i >= argc)
        die_usage("FORMAT [ARGUMENT...]");
    const char *fmt = argv[i++];
    int used;
    do {
        used = 0;
        for (const char *f = fmt; *f; f++) {
            if (*f == '\\') {
                f++;
                if (!*f) {
                    putchar('\\');
                    break;
                }
                int c = pf_escape(&f, 0);
                f--;
                if (c < 0)
                    return pf_status;
                putchar(c);
                continue;
            }
            if (*f != '%') {
                putchar(*f);
                continue;
            }
            if (f[1] == '%') {
                putchar('%');
                f++;
                continue;
            }
            /* %[flags][width][.prec]conv */
            char spec[64];
            int k = 0;
            spec[k++] = '%';
            f++;
            while (*f && strchr("-+ #0'", *f) && k < 40)
                spec[k++] = *f++;
            int have_w = 0, have_p = 0, w = 0, prec = 0;
            if (*f == '*') {
                w = i < argc ? (int)pf_int(argv[i++]) : 0;
                have_w = 1;
                f++;
            } else {
                while (isdigit(*f) && k < 50)
                    spec[k++] = *f++;
            }
            if (*f == '.') {
                f++;
                if (*f == '*') {
                    prec = i < argc ? (int)pf_int(argv[i++]) : 0;
                    have_p = 1;
                    f++;
                } else {
                    spec[k++] = '.';
                    while (isdigit(*f) && k < 58)
                        spec[k++] = *f++;
                }
            }
            while (*f == 'l' || *f == 'h' || *f == 'L' || *f == 'q' || *f == 'j' || *f == 'z' || *f == 't')
                f++;
            char conv = *f;
            if (!conv) {
                eprintf("printf: %%: invalid conversion\n");
                return 1;
            }
            const char *arg = i < argc ? argv[i++] : 0;
            if (arg)
                used = 1;
            if (have_w)
                k += snprintf(spec + k, sizeof(spec) - k, "*");
            if (have_p)
                k += snprintf(spec + k, sizeof(spec) - k, ".*");
            switch (conv) {
            case 'd': case 'i': {
                long long v = arg ? pf_int(arg) : 0;
                snprintf(spec + k, sizeof(spec) - k, "lld");
                if (have_w && have_p) printf(spec, w, prec, v);
                else if (have_w) printf(spec, w, v);
                else if (have_p) printf(spec, prec, v);
                else printf(spec, v);
                break;
            }
            case 'o': case 'u': case 'x': case 'X': {
                unsigned long long v = arg ? (unsigned long long)pf_int(arg) : 0;
                snprintf(spec + k, sizeof(spec) - k, "ll%c", conv);
                if (have_w && have_p) printf(spec, w, prec, v);
                else if (have_w) printf(spec, w, v);
                else if (have_p) printf(spec, prec, v);
                else printf(spec, v);
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                double v = arg ? pf_float(arg) : 0;
                snprintf(spec + k, sizeof(spec) - k, "%c", conv);
                if (have_w && have_p) printf(spec, w, prec, v);
                else if (have_w) printf(spec, w, v);
                else if (have_p) printf(spec, prec, v);
                else printf(spec, v);
                break;
            }
            case 'c':
                snprintf(spec + k, sizeof(spec) - k, "c");
                if (have_w) printf(spec, w, arg ? arg[0] : 0);
                else printf(spec, arg ? arg[0] : 0);
                break;
            case 's':
                snprintf(spec + k, sizeof(spec) - k, "s");
                if (have_w && have_p) printf(spec, w, prec, arg ? arg : "");
                else if (have_w) printf(spec, w, arg ? arg : "");
                else if (have_p) printf(spec, prec, arg ? arg : "");
                else printf(spec, arg ? arg : "");
                break;
            case 'b': {
                /* the argument with escapes; \c stops all output */
                size_t olen = 0;
                const char *s = arg ? arg : "";
                char *out = malloc(strlen(s) + 1);
                int stop = 0;
                for (; *s; s++) {
                    if (*s == '\\' && s[1]) {
                        s++;
                        int c = pf_escape(&s, 1);
                        s--;
                        if (c < 0) {
                            stop = 1;
                            break;
                        }
                        out[olen++] = c;
                    } else {
                        out[olen++] = *s;
                    }
                }
                out[olen] = 0;
                snprintf(spec + k, sizeof(spec) - k, "s");
                if (have_w && have_p) printf(spec, w, prec, out);
                else if (have_w) printf(spec, w, out);
                else if (have_p) printf(spec, prec, out);
                else fwrite(out, 1, olen, stdout);
                free(out);
                if (stop)
                    return pf_status;
                break;
            }
            default:
                eprintf("printf: %%%c: invalid conversion\n", conv);
                return 1;
            }
        }
    } while (used && i < argc);
    return pf_status;
}

/* ================================================================ main */

int main(int argc, char **argv)
{
    prog = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    if (!strcmp(prog, "du")) return du_main(argc, argv);
    if (!strcmp(prog, "dd")) return dd_main(argc, argv);
    if (!strcmp(prog, "basename")) return basename_main(argc, argv);
    if (!strcmp(prog, "dirname")) return dirname_main(argc, argv);
    if (!strcmp(prog, "realpath")) return realpath_main(argc, argv);
    if (!strcmp(prog, "readlink")) return readlink_main(argc, argv);
    if (!strcmp(prog, "mktemp")) return mktemp_main(argc, argv);
    if (!strcmp(prog, "chgrp")) return chgrp_main(argc, argv);
    if (!strcmp(prog, "file")) return file_main(argc, argv);
    if (!strcmp(prog, "sync")) { sync(); return 0; }
    if (!strcmp(prog, "truncate")) return truncate_main(argc, argv);
    if (!strcmp(prog, "df")) return df_main(argc, argv);
    if (!strcmp(prog, "mount")) return mount_main(argc, argv);
    if (!strcmp(prog, "umount")) return umount_main(argc, argv);
    if (!strcmp(prog, "seq")) return seq_main(argc, argv);
    if (!strcmp(prog, "expr")) return expr_main(argc, argv);
    if (!strcmp(prog, "test") || !strcmp(prog, "[")) return test_main(argc, argv);
    if (!strcmp(prog, "printf")) return printf_main(argc, argv);
    eprintf("fileutils: run me as one of: du df dd basename dirname realpath readlink mktemp chgrp file sync truncate "
            "mount umount "
            "seq expr test [ printf\n");
    return 2;
}
