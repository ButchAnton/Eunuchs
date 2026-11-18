/*
 * diff: compare files line by line.
 *
 *   diff [-u | -U N | -c | -C N] [-q] [-r] [-N] [-i] [-b] [-w] [-B] [-a]
 *        [--label NAME]... FILE1 FILE2
 *
 * Output is the normal format by default, unified with -u, context with -c.
 * Either file may be - (standard input) or a directory (then the file of
 * the same name in it); two directories are compared entry by entry, and
 * recursively with -r. Exit status: 0 no differences, 1 differences, 2
 * trouble.
 *
 * Lines are numbered by content (a hash table after the -i/-b/-w
 * normalisation), then compared with Myers' O(ND) algorithm in its linear
 * space, divide-and-conquer form.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fmt;                         /* 0 normal, 'u' unified, 'c' context */
static int ctx = 3, brief, recursive, new_file, icase, ign_space_change, ign_all_space, ign_blank, text;
static const char *labels[2];
static int nlabels;
static char opts_text[256];             /* the options, for "diff -r" headers */

/* ---- files as lines ---- */

struct file {
    const char *name;
    char *data;
    size_t size;
    char **line;
    size_t *len;
    int *id;
    long n;
    int no_eol;                         /* the last line has no newline */
    struct stat st;
    int is_stdin;
};

static int load(struct file *f, const char *name)
{
    memset(f, 0, sizeof(*f));
    f->name = name;
    int fd;
    if (!strcmp(name, "-")) {
        fd = 0;
        f->is_stdin = 1;
        f->st.st_mtim.tv_sec = time(0);
    } else {
        fd = open(name, O_RDONLY);
        if (fd < 0)
            return -1;
        fstat(fd, &f->st);
    }
    size_t cap = 0;
    for (;;) {
        if (f->size + 4096 > cap) {
            cap = cap ? cap * 2 : 65536;
            f->data = realloc(f->data, cap + 1);
        }
        long r = read(fd, f->data + f->size, cap - f->size);
        if (r < 0) {
            if (fd)
                close(fd);
            return -1;
        }
        if (r == 0)
            break;
        f->size += r;
    }
    if (fd)
        close(fd);
    if (!f->data)
        f->data = malloc(1);
    f->data[f->size] = 0;
    long cap_lines = 0;
    for (size_t off = 0; off < f->size;) {
        char *nl = memchr(f->data + off, '\n', f->size - off);
        size_t end = nl ? (size_t)(nl - f->data) : f->size;
        if (f->n == cap_lines) {
            cap_lines = cap_lines ? cap_lines * 2 : 1024;
            f->line = realloc(f->line, cap_lines * sizeof(char *));
            f->len = realloc(f->len, cap_lines * sizeof(size_t));
        }
        f->line[f->n] = f->data + off;
        f->len[f->n++] = end - off;
        if (!nl)
            f->no_eol = 1;
        off = end + 1;
    }
    return 0;
}

static void unload(struct file *f)
{
    free(f->data);
    free(f->line);
    free(f->len);
    free(f->id);
}

static int binary(const struct file *f)
{
    if (text)
        return 0;
    size_t n = f->size < 8192 ? f->size : 8192;
    return memchr(f->data, 0, n) != 0;
}

/* ---- line ids ---- */

struct entry {
    char *key;
    size_t len;
    unsigned hash;
    int id;
    struct entry *next;
};

static struct entry **table;
static unsigned tsize;
static int next_id;

/* The comparison form of a line: case folded, white space squeezed or dropped. */
static size_t normalize(const char *s, size_t n, char *out)
{
    size_t o = 0;
    int space = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (ign_all_space && isspace(c))
            continue;
        if (ign_space_change && isspace(c)) {
            space = 1;
            continue;
        }
        if (space) {
            out[o++] = ' ';
            space = 0;
        }
        out[o++] = icase ? tolower(c) : c;
    }
    return o;                           /* trailing white space dropped with -b */
}

static int line_id(const char *s, size_t n, int no_eol)
{
    char *buf = 0;
    if (icase || ign_space_change || ign_all_space) {
        buf = malloc(n + 2);
        n = normalize(s, n, buf);
        s = buf;
    }
    unsigned h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    h ^= no_eol;
    struct entry *e;
    for (e = table[h % tsize]; e; e = e->next)
        if (e->hash == h && e->len == n + no_eol && !memcmp(e->key, s, n) && (!no_eol || e->key[n] == 1))
            break;
    if (!e) {
        e = malloc(sizeof(*e));
        e->key = malloc(n + 1);
        memcpy(e->key, s, n);
        e->key[n] = no_eol ? 1 : 0;
        e->len = n + no_eol;
        e->hash = h;
        e->id = next_id++;
        e->next = table[h % tsize];
        table[h % tsize] = e;
    }
    free(buf);
    return e->id;
}

static void number_lines(struct file *a, struct file *b)
{
    tsize = (a->n + b->n) | 1;
    if (tsize < 1021)
        tsize = 1021;
    table = calloc(tsize, sizeof(*table));
    next_id = 0;
    struct file *fs[2] = { a, b };
    for (int k = 0; k < 2; k++) {
        struct file *f = fs[k];
        f->id = malloc((f->n + 1) * sizeof(int));
        for (long i = 0; i < f->n; i++)
            f->id[i] = line_id(f->line[i], f->len[i], f->no_eol && i == f->n - 1);
    }
    for (unsigned i = 0; i < tsize; i++)
        for (struct entry *e = table[i], *nx; e; e = nx) {
            nx = e->next;
            free(e->key);
            free(e);
        }
    free(table);
}

/* ---- Myers ---- */

static int *A, *B;
static char *del, *ins;                 /* del[i]: line i of A is not in B; ins[j] the reverse */
static long *vf, *vb;
static long voff;

static void middle(long a0, long a1, long b0, long b1, long *mx, long *my)
{
    long n = a1 - a0, m = b1 - b0, delta = n - m, odd = delta & 1, max = (n + m + 1) / 2;
    vf[voff + 1] = 0;
    vb[voff + 1] = 0;
    for (long d = 0; d <= max; d++) {
        for (long k = -d; k <= d; k += 2) {
            long x = k == -d || (k != d && vf[voff + k - 1] < vf[voff + k + 1]) ? vf[voff + k + 1]
                                                                             : vf[voff + k - 1] + 1;
            long y = x - k;
            while (x < n && y < m && A[a0 + x] == B[b0 + y])
                x++, y++;
            vf[voff + k] = x;
            if (odd && delta - k >= -(d - 1) && delta - k <= d - 1 && x + vb[voff + delta - k] >= n) {
                *mx = a0 + x;
                *my = b0 + y;
                return;
            }
        }
        for (long k = -d; k <= d; k += 2) {
            long x = k == -d || (k != d && vb[voff + k - 1] < vb[voff + k + 1]) ? vb[voff + k + 1]
                                                                             : vb[voff + k - 1] + 1;
            long y = x - k;
            while (x < n && y < m && A[a1 - 1 - x] == B[b1 - 1 - y])
                x++, y++;
            vb[voff + k] = x;
            if (!odd && delta - k >= -d && delta - k <= d && x + vf[voff + delta - k] >= n) {
                *mx = a1 - x;
                *my = b1 - y;
                return;
            }
        }
    }
    *mx = a0;                           /* not reached */
    *my = b0;
}

static void compare(long a0, long a1, long b0, long b1)
{
    while (a0 < a1 && b0 < b1 && A[a0] == B[b0])
        a0++, b0++;
    while (a0 < a1 && b0 < b1 && A[a1 - 1] == B[b1 - 1])
        a1--, b1--;
    if (a0 == a1) {
        for (long j = b0; j < b1; j++)
            ins[j] = 1;
        return;
    }
    if (b0 == b1) {
        for (long i = a0; i < a1; i++)
            del[i] = 1;
        return;
    }
    long x, y;
    middle(a0, a1, b0, b1, &x, &y);
    if ((x == a0 && y == b0) || (x == a1 && y == b1)) {
        /* cannot split (should not happen): call it all changed */
        for (long i = a0; i < a1; i++)
            del[i] = 1;
        for (long j = b0; j < b1; j++)
            ins[j] = 1;
        return;
    }
    compare(a0, x, b0, y);
    compare(x, a1, y, b1);
}

/* ---- groups of changes ---- */

struct group {
    long a0, a1, b0, b1;
};

static struct group *groups;
static long ngroups;

static void find_groups(const struct file *a, const struct file *b)
{
    long i = 0, j = 0, cap = 0;
    ngroups = 0;
    while (i < a->n || j < b->n) {
        if (i < a->n && j < b->n && !del[i] && !ins[j]) {
            i++, j++;
            continue;
        }
        struct group g = { i, i, j, j };
        while (g.a1 < a->n && del[g.a1])
            g.a1++;
        while (g.b1 < b->n && ins[g.b1])
            g.b1++;
        if (g.a1 == g.a0 && g.b1 == g.b0) {
            i++, j++;                   /* (unreachable) */
            continue;
        }
        if (ign_blank) {
            int all_blank = 1;
            for (long k = g.a0; k < g.a1 && all_blank; k++)
                for (size_t c = 0; c < a->len[k]; c++)
                    if (!isspace((unsigned char)a->line[k][c]))
                        all_blank = 0;
            for (long k = g.b0; k < g.b1 && all_blank; k++)
                for (size_t c = 0; c < b->len[k]; c++)
                    if (!isspace((unsigned char)b->line[k][c]))
                        all_blank = 0;
            if (all_blank) {
                i = g.a1, j = g.b1;
                continue;
            }
        }
        if (ngroups == cap)
            groups = realloc(groups, (cap = cap ? cap * 2 : 64) * sizeof(*groups));
        groups[ngroups++] = g;
        i = g.a1, j = g.b1;
    }
}

/* ---- output ---- */

static void put_line(const struct file *f, long i, const char *prefix)
{
    fputs(prefix, stdout);
    fwrite(f->line[i], 1, f->len[i], stdout);
    putchar('\n');
    if (f->no_eol && i == f->n - 1)
        puts("\\ No newline at end of file");
}

static void range(long a, long b, char sep)
{
    /* 1-based lines a+1..b; an empty range names the line before */
    if (b - a <= 1)
        printf("%ld", b - a == 1 ? a + 1 : a);
    else
        printf("%ld%c%ld", a + 1, sep, b);
}

static void normal_output(const struct file *a, const struct file *b)
{
    for (long g = 0; g < ngroups; g++) {
        struct group *x = &groups[g];
        if (x->a0 == x->a1) {
            printf("%ld", x->a0);
            putchar('a');
            range(x->b0, x->b1, ',');
        } else if (x->b0 == x->b1) {
            range(x->a0, x->a1, ',');
            putchar('d');
            printf("%ld", x->b0);
        } else {
            range(x->a0, x->a1, ',');
            putchar('c');
            range(x->b0, x->b1, ',');
        }
        putchar('\n');
        for (long i = x->a0; i < x->a1; i++)
            put_line(a, i, "< ");
        if (x->a0 != x->a1 && x->b0 != x->b1)
            puts("---");
        for (long j = x->b0; j < x->b1; j++)
            put_line(b, j, "> ");
    }
}

static void stamp(const struct file *f, int which, const char *mark)
{
    char when[64];
    time_t t = f->st.st_mtim.tv_sec;
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&t));
    if (which < nlabels)
        printf("%s %s\n", mark, labels[which]);
    else
        printf("%s %s\t%s.%09ld +0000\n", mark, f->name, when, (long)f->st.st_mtim.tv_nsec);
}

/* Hunks: groups whose context would touch merge. */
static long hunk_end(long g)
{
    long e = g;
    while (e + 1 < ngroups && groups[e + 1].a0 - groups[e].a1 <= 2 * ctx)
        e++;
    return e;
}

static void unified_output(const struct file *a, const struct file *b)
{
    stamp(a, 0, "---");
    stamp(b, 1, "+++");
    for (long g = 0; g < ngroups;) {
        long e = hunk_end(g);
        long as = groups[g].a0 - ctx < 0 ? 0 : groups[g].a0 - ctx;
        long ae = groups[e].a1 + ctx > a->n ? a->n : groups[e].a1 + ctx;
        long bs = groups[g].b0 - (groups[g].a0 - as);
        long be = groups[e].b1 + (ae - groups[e].a1);
        printf("@@ -");
        if (ae - as == 1) printf("%ld", as + 1); else printf("%ld,%ld", ae - as ? as + 1 : as, ae - as);
        printf(" +");
        if (be - bs == 1) printf("%ld", bs + 1); else printf("%ld,%ld", be - bs ? bs + 1 : bs, be - bs);
        printf(" @@\n");
        long i = as;
        for (long k = g; k <= e; k++) {
            for (; i < groups[k].a0; i++)
                put_line(a, i, " ");
            for (long d = groups[k].a0; d < groups[k].a1; d++)
                put_line(a, d, "-");
            for (long n = groups[k].b0; n < groups[k].b1; n++)
                put_line(b, n, "+");
            i = groups[k].a1;
        }
        for (; i < ae; i++)
            put_line(a, i, " ");
        g = e + 1;
    }
}

static void context_output(const struct file *a, const struct file *b)
{
    stamp(a, 0, "***");
    stamp(b, 1, "---");
    for (long g = 0; g < ngroups;) {
        long e = hunk_end(g);
        long as = groups[g].a0 - ctx < 0 ? 0 : groups[g].a0 - ctx;
        long ae = groups[e].a1 + ctx > a->n ? a->n : groups[e].a1 + ctx;
        long bs = groups[g].b0 - (groups[g].a0 - as);
        long be = groups[e].b1 + (ae - groups[e].a1);
        puts("***************");
        printf("*** ");
        range(as, ae, ',');
        printf(" ****\n");
        int any_del = 0, any_ins = 0;
        for (long k = g; k <= e; k++) {
            any_del |= groups[k].a1 > groups[k].a0;
            any_ins |= groups[k].b1 > groups[k].b0;
        }
        if (any_del) {
            long i = as;
            for (long k = g; k <= e; k++) {
                for (; i < groups[k].a0; i++)
                    put_line(a, i, "  ");
                const char *p = groups[k].b1 > groups[k].b0 ? "! " : "- ";
                for (; i < groups[k].a1; i++)
                    put_line(a, i, p);
            }
            for (; i < ae; i++)
                put_line(a, i, "  ");
        }
        printf("--- ");
        range(bs, be, ',');
        printf(" ----\n");
        if (any_ins) {
            long j = bs;
            for (long k = g; k <= e; k++) {
                for (; j < groups[k].b0; j++)
                    put_line(b, j, "  ");
                const char *p = groups[k].a1 > groups[k].a0 ? "! " : "+ ";
                for (; j < groups[k].b1; j++)
                    put_line(b, j, p);
            }
            for (; j < be; j++)
                put_line(b, j, "  ");
        }
        g = e + 1;
    }
}

/* Compare two files (either may be absent with -N). 0, 1 or 2. */
static int diff_files(const char *p1, const char *p2, int absent1, int absent2, int show_cmd)
{
    struct file a, b;
    if (absent1) {
        memset(&a, 0, sizeof(a));
        a.name = p1;
        a.data = malloc(1);
    } else if (load(&a, p1) < 0) {
        eprintf("diff: %s: %s\n", p1, strerror(errno));
        return 2;
    }
    if (absent2) {
        memset(&b, 0, sizeof(b));
        b.name = p2;
        b.data = malloc(1);
    } else if (load(&b, p2) < 0) {
        eprintf("diff: %s: %s\n", p2, strerror(errno));
        unload(&a);
        return 2;
    }
    int r = 0;
    if (a.size == b.size && !memcmp(a.data, b.data, a.size)) {
        r = 0;
    } else if (binary(&a) || binary(&b)) {
        printf("Binary files %s and %s differ\n", p1, p2);
        r = 1;
    } else {
        number_lines(&a, &b);
        A = a.id;
        B = b.id;
        del = calloc(a.n + 1, 1);
        ins = calloc(b.n + 1, 1);
        long nm = a.n + b.n + 3;
        vf = malloc((2 * nm + 1) * sizeof(long));
        vb = malloc((2 * nm + 1) * sizeof(long));
        voff = nm;
        compare(0, a.n, 0, b.n);
        find_groups(&a, &b);
        if (ngroups) {
            r = 1;
            if (brief) {
                printf("Files %s and %s differ\n", p1, p2);
            } else {
                if (show_cmd)
                    printf("diff%s %s %s\n", opts_text, p1, p2);
                if (fmt == 'u')
                    unified_output(&a, &b);
                else if (fmt == 'c')
                    context_output(&a, &b);
                else
                    normal_output(&a, &b);
            }
        }
        free(del);
        free(ins);
        free(vf);
        free(vb);
    }
    unload(&a);
    unload(&b);
    return r;
}

static int name_cmp(const void *x, const void *y)
{
    return strcmp(*(char *const *)x, *(char *const *)y);
}

static char **list_dir(const char *path, int *n)
{
    DIR *d = opendir(path);
    *n = 0;
    if (!d)
        return 0;
    char **v = 0;
    int cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (*n == cap)
            v = realloc(v, (cap = cap ? cap * 2 : 32) * sizeof(char *));
        v[(*n)++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(v, *n, sizeof(char *), name_cmp);
    return v;
}

static int diff_paths(const char *p1, const char *p2, int top);

static int diff_dirs(const char *d1, const char *d2)
{
    int n1, n2, r = 0;
    char **v1 = list_dir(d1, &n1), **v2 = list_dir(d2, &n2);
    if ((!v1 && n1 == 0 && access(d1, R_OK)) || (!v2 && n2 == 0 && access(d2, R_OK))) {
        eprintf("diff: cannot read a directory\n");
        return 2;
    }
    int i = 0, j = 0;
    while (i < n1 || j < n2) {
        int c = i >= n1 ? 1 : j >= n2 ? -1 : strcmp(v1[i], v2[j]);
        char a[PATH_MAX], b[PATH_MAX];
        if (c == 0) {
            snprintf(a, sizeof(a), "%s/%s", d1, v1[i]);
            snprintf(b, sizeof(b), "%s/%s", d2, v2[j]);
            int x = diff_paths(a, b, 0);
            if (x > r)
                r = x;
            i++, j++;
        } else if (c < 0) {
            snprintf(a, sizeof(a), "%s/%s", d1, v1[i]);
            snprintf(b, sizeof(b), "%s/%s", d2, v1[i]);
            struct stat st;
            if (new_file && stat(a, &st) == 0 && S_ISREG(st.st_mode)) {
                int x = diff_files(a, b, 0, 1, 1);
                if (x > r) r = x;
            } else {
                printf("Only in %s: %s\n", d1, v1[i]);
                if (!r) r = 1;
            }
            i++;
        } else {
            snprintf(a, sizeof(a), "%s/%s", d1, v2[j]);
            snprintf(b, sizeof(b), "%s/%s", d2, v2[j]);
            struct stat st;
            if (new_file && stat(b, &st) == 0 && S_ISREG(st.st_mode)) {
                int x = diff_files(a, b, 1, 0, 1);
                if (x > r) r = x;
            } else {
                printf("Only in %s: %s\n", d2, v2[j]);
                if (!r) r = 1;
            }
            j++;
        }
    }
    for (int k = 0; k < n1; k++)
        free(v1[k]);
    for (int k = 0; k < n2; k++)
        free(v2[k]);
    free(v1);
    free(v2);
    return r;
}

static int diff_paths(const char *p1, const char *p2, int top)
{
    struct stat s1, s2;
    int e1 = strcmp(p1, "-") ? stat(p1, &s1) : (memset(&s1, 0, sizeof(s1)), s1.st_mode = S_IFREG, 0);
    int e2 = strcmp(p2, "-") ? stat(p2, &s2) : (memset(&s2, 0, sizeof(s2)), s2.st_mode = S_IFREG, 0);
    if (e1 < 0 && !(new_file && e2 == 0)) {
        eprintf("diff: %s: %s\n", p1, strerror(errno));
        return 2;
    }
    if (e2 < 0 && !(new_file && e1 == 0)) {
        eprintf("diff: %s: %s\n", p2, strerror(errno));
        return 2;
    }
    int dir1 = e1 == 0 && S_ISDIR(s1.st_mode), dir2 = e2 == 0 && S_ISDIR(s2.st_mode);
    if (dir1 && dir2) {
        if (!top && !recursive) {
            printf("Common subdirectories: %s and %s\n", p1, p2);
            return 0;
        }
        return diff_dirs(p1, p2);
    }
    if (top && (dir1 || dir2)) {
        /* a file and a directory: the file of the same name in it */
        char path[PATH_MAX];
        const char *file = dir1 ? p2 : p1, *dir = dir1 ? p1 : p2;
        const char *base = strrchr(file, '/') ? strrchr(file, '/') + 1 : file;
        snprintf(path, sizeof(path), "%s/%s", dir, base);
        return dir1 ? diff_paths(path, p2, 0) : diff_paths(p1, path, 0);
    }
    if (dir1 || dir2) {
        printf("File %s is a %s while file %s is a %s\n", p1, dir1 ? "directory" : "regular file", p2,
               dir2 ? "directory" : "regular file");
        return 1;
    }
    return diff_files(p1, p2, e1 < 0, e2 < 0, !top);
}

static void usage(void)
{
    eprintf("usage: diff [-u | -U N | -c | -C N] [-qrNibwBa] [--label NAME]... FILE1 FILE2\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int i = 1;
    size_t ol = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) {
            i++;
            break;
        }
        if (!strcmp(a, "--label") && i + 1 < argc) {
            if (nlabels < 2)
                labels[nlabels++] = argv[++i];
            continue;
        }
        if (!strncmp(a, "--label=", 8)) {
            if (nlabels < 2)
                labels[nlabels++] = a + 8;
            continue;
        }
        if (!strcmp(a, "--brief")) { brief = 1; continue; }
        if (!strcmp(a, "--recursive")) { recursive = 1; continue; }
        if (!strcmp(a, "--unified")) { fmt = 'u'; continue; }
        if (!strcmp(a, "--new-file")) { new_file = 1; continue; }
        if (!strcmp(a, "--text")) { text = 1; continue; }
        ol += snprintf(opts_text + ol, sizeof(opts_text) - ol, " %s", a);
        for (const char *f = a + 1; *f; f++) {
            switch (*f) {
            case 'u': fmt = 'u'; break;
            case 'c': fmt = 'c'; break;
            case 'U':
            case 'C': {
                const char *v = f[1] ? f + 1 : i + 1 < argc ? argv[++i] : 0;
                if (!v)
                    usage();
                fmt = *f == 'U' ? 'u' : 'c';
                ctx = atoi(v);
                f += strlen(f) - 1;
                break;
            }
            case 'q': brief = 1; break;
            case 'r': recursive = 1; break;
            case 'N': new_file = 1; break;
            case 'i': icase = 1; break;
            case 'b': ign_space_change = 1; break;
            case 'w': ign_all_space = 1; break;
            case 'B': ign_blank = 1; break;
            case 'a': text = 1; break;
            default:
                if (isdigit(*f)) {      /* -NUM: context lines */
                    ctx = strtol(f, (char **)&f, 10);
                    f--;
                    if (!fmt)
                        fmt = 'c';
                    break;
                }
                usage();
            }
        }
    }
    if (argc - i != 2)
        usage();
    int r = diff_paths(argv[i], argv[i + 1], 1);
    fflush(stdout);
    return r;
}
