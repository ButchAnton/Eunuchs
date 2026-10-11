/*
 * ls: list directory contents, in the style of GNU coreutils.
 *
 *   -a all   -A almost all (no . ..)   -d directories themselves   -R recursive
 *   -l long   -o long without group   -g long without owner   -n numeric ids
 *   -1 one per line   -C columns   -x columns, across   -m commas
 *   -t by time   -S by size   -X by extension   -v natural (version) order
 *   -U directory order   -r reverse   -c use ctime   --group-directories-first
 *   -h human sizes   -k 1K blocks   -s sizes in blocks   -i inodes
 *   -F classify (/ * |)   -p slash after directories   --color[=auto|always|never], -G
 *   -Q quote names   -N literal names   -b C escapes   -q '?' for unprintable
 *   -B no backups (~)   -I PATTERN, --hide=PATTERN   -w COLS   -T (accepted)
 *   --full-time, --time-style=full-iso|long-iso|iso|locale   --help
 *
 * Times are shown in UTC. Exit status: 0, 1 for minor problems (an
 * unreadable subdirectory), 2 when a command-line argument can't be used.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ---- options ---- */

enum { F_COLUMNS, F_ACROSS, F_ONE, F_LONG, F_COMMAS };
enum { S_NAME, S_TIME, S_SIZE, S_EXT, S_VERSION, S_NONE };
enum { Q_LITERAL, Q_SHELL, Q_C, Q_ESCAPE, Q_HIDE };
enum { T_DEFAULT, T_FULL, T_LONG, T_ISO };

static int format = -1, sort = S_NAME, reverse, show_all, almost_all, dir_itself, recursive;
static int no_owner, no_group, numeric, human, kblocks = 1, show_blocks, show_inode;
static int classify, slash_dirs, file_type, color, use_ctime, dirs_first, quoting = -1;
static int ignore_backups, time_style = T_DEFAULT, width;
static char *hide[16];
static int nhide;
static int status;

/* ---- entries ---- */

struct ent {
    char *name;         /* as listed */
    char *path;         /* for stat */
    struct stat st;
    int ok;             /* stat worked */
};

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        eprintf("ls: out of memory\n");
        exit(2);
    }
    return p;
}

static char *xstrdup(const char *s)
{
    char *d = xmalloc(strlen(s) + 1);
    strcpy(d, s);
    return d;
}

static char *join(const char *dir, const char *name)
{
    size_t a = strlen(dir), b = strlen(name);
    char *p = xmalloc(a + b + 2);
    memcpy(p, dir, a);
    int slash = a && dir[a - 1] != '/';
    if (slash)
        p[a] = '/';
    memcpy(p + a + slash, name, b + 1);
    return p;
}

/* ---- users and groups (files read once) ---- */

struct idname {
    int id;
    char name[32];
};
static struct idname users[64], groups[64];
static int nusers = -1, ngroups = -1;

static int load_ids(const char *file, struct idname *v, int max)
{
    int fd = open(file, O_RDONLY);
    if (fd < 0)
        return 0;
    static char buf[8192];
    long n = 0, r;
    while (n < (long)sizeof(buf) - 1 && (r = read(fd, buf + n, sizeof(buf) - 1 - n)) > 0)
        n += r;
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    int k = 0;
    for (char *line = buf; line && *line && k < max;) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        char *c1 = strchr(line, ':');
        char *c2 = c1 ? strchr(c1 + 1, ':') : 0;
        if (c2) {
            v[k].id = atoi(c2 + 1);
            snprintf(v[k].name, sizeof(v[k].name), "%.*s", (int)(c1 - line), line);
            k++;
        }
        line = next;
    }
    return k;
}

static const char *id_name(int id, int is_group)
{
    static char num[4][16];
    static int slot;
    if (!numeric) {
        struct idname *v = is_group ? groups : users;
        int *n = is_group ? &ngroups : &nusers;
        if (*n < 0)
            *n = load_ids(is_group ? "/etc/group" : "/etc/passwd", v, 64);
        for (int i = 0; i < *n; i++)
            if (v[i].id == id)
                return v[i].name;
    }
    char *b = num[slot++ % 4];
    snprintf(b, 16, "%d", id);
    return b;
}

/* ---- colors (LS_COLORS, with GNU's defaults) ---- */

struct colorrule {
    char key[24];
    char seq[24];
};
static struct colorrule crules[64];
static int ncrules;

static void add_crule(const char *key, const char *seq)
{
    for (int i = 0; i < ncrules; i++)
        if (!strcmp(crules[i].key, key)) {
            snprintf(crules[i].seq, sizeof(crules[i].seq), "%s", seq);
            return;
        }
    if (ncrules < 64) {
        snprintf(crules[ncrules].key, sizeof(crules[ncrules].key), "%s", key);
        snprintf(crules[ncrules].seq, sizeof(crules[ncrules].seq), "%s", seq);
        ncrules++;
    }
}

static void load_colors(void)
{
    static const char *const dflt =
        "di=01;34:ex=01;32:cd=40;33;01:bd=40;33;01:pi=40;33:so=01;35:ln=01;36:"
        "su=37;41:sg=30;43:tw=30;42:ow=34;42:"
        "*.tar=01;31:*.gz=01;31:*.zip=01;31:*.img=01;31:"
        "*.jpg=01;35:*.png=01;35:*.gif=01;35:*.html=00;33:*.md=00;33";
    const char *env = getenv("LS_COLORS");
    for (int pass = 0; pass < 2; pass++) {
        const char *s = pass ? env : dflt;
        if (!s)
            continue;
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s", s);
        for (char *t = buf; t && *t;) {
            char *e = strchr(t, ':');
            if (e)
                *e = 0;
            char *eq = strchr(t, '=');
            if (eq) {
                *eq = 0;
                add_crule(t, eq + 1);
            }
            t = e ? e + 1 : 0;
        }
    }
}

static const char *crule(const char *key)
{
    for (int i = 0; i < ncrules; i++)
        if (!strcmp(crules[i].key, key))
            return crules[i].seq;
    return 0;
}

static const char *color_for(struct ent *e)
{
    if (!e->ok)
        return crule("or") ? crule("or") : 0;
    unsigned m = e->st.st_mode;
    if (S_ISDIR(m)) {
        if ((m & 0002) && crule("ow"))
            return crule("ow");
        return crule("di");
    }
    if (S_ISCHR(m))
        return crule("cd");
    if (S_ISFIFO(m))
        return crule("pi");
    if ((m & S_ISUID) && crule("su"))
        return crule("su");
    if ((m & S_ISGID) && crule("sg"))
        return crule("sg");
    if (m & 0111)
        return crule("ex");
    const char *dot = strrchr(e->name, '.');
    if (dot) {
        char key[32];
        snprintf(key, sizeof(key), "*%s", dot);
        if (crule(key))
            return crule(key);
    }
    return crule("fi");
}

/* ---- names ---- */

static int safe_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           strchr("._+,:@%/=-", c);
}

/* The name as displayed (without color); width in *w. */
static char *quoted(const char *s, int *w)
{
    size_t n = strlen(s);
    char *o = xmalloc(n * 4 + 8);
    int k = 0;
    switch (quoting) {
    case Q_LITERAL:
        strcpy(o, s);
        k = n;
        break;
    case Q_HIDE:
        for (; *s; s++)
            o[k++] = (unsigned char)*s < 32 || *s == 127 ? '?' : *s;
        break;
    case Q_C:
    case Q_ESCAPE: {
        if (quoting == Q_C)
            o[k++] = '"';
        for (; *s; s++) {
            unsigned char c = *s;
            if (c == '\n') { o[k++] = '\\'; o[k++] = 'n'; }
            else if (c == '\t') { o[k++] = '\\'; o[k++] = 't'; }
            else if (c == '\\' || (c == '"' && quoting == Q_C)) { o[k++] = '\\'; o[k++] = c; }
            else if (c == ' ' && quoting == Q_ESCAPE) { o[k++] = '\\'; o[k++] = ' '; }
            else if (c < 32 || c == 127) k += snprintf(o + k, 8, "\\%03o", c);
            else o[k++] = c;
        }
        if (quoting == Q_C)
            o[k++] = '"';
        break;
    }
    case Q_SHELL: {
        int plain = *s != 0;
        for (const char *p = s; *p; p++)
            if (!safe_char(*p) && (unsigned char)*p < 0x80)
                plain = 0;
        if (plain) {
            strcpy(o, s);
            k = n;
            break;
        }
        o[k++] = '\'';
        for (; *s; s++) {
            if (*s == '\'') {
                memcpy(o + k, "'\\''", 4);
                k += 4;
            } else if ((unsigned char)*s < 32 || *s == 127) {
                k += snprintf(o + k, 16, "'$'\\%03o''", (unsigned char)*s);
            } else {
                o[k++] = *s;
            }
        }
        o[k++] = '\'';
        break;
    }
    }
    o[k] = 0;
    /* display width: UTF-8 continuation bytes take no column */
    int cw = 0;
    for (int i = 0; i < k; i++)
        if (((unsigned char)o[i] & 0xc0) != 0x80)
            cw++;
    *w = cw;
    return o;
}

static char type_char(struct ent *e)
{
    if (!e->ok)
        return 0;
    unsigned m = e->st.st_mode;
    if (S_ISDIR(m))
        return classify || slash_dirs || file_type ? '/' : 0;
    if (!classify && !file_type)
        return 0;
    if (S_ISFIFO(m))
        return '|';
    if (classify && S_ISREG(m) && (m & 0111))
        return '*';
    return 0;
}

/* Name with color and indicator; *w gets the display width. */
static char *decorated(struct ent *e, int *w)
{
    int qw;
    char *q = quoted(e->name, &qw);
    const char *c = color ? color_for(e) : 0;
    char t = type_char(e);
    size_t size = strlen(q) + 64;
    char *o = xmalloc(size);
    char ind[2] = { t, 0 };
    if (c && *c)
        snprintf(o, size, "\033[%sm%s\033[0m%s", c, q, ind);
    else
        snprintf(o, size, "%s%s", q, ind);
    free(q);
    *w = qw + (t != 0);
    return o;
}

/* ---- sizes and times ---- */

static void human_size(char *b, size_t n, unsigned long long v)
{
    static const char units[] = "BKMGTP";
    if (v < 1024) {
        snprintf(b, n, "%llu", v);
        return;
    }
    int u = 0;
    unsigned long long x = v * 10;     /* tenths */
    while (x >= 10240 && u < 5) {
        x = (x + 1023) / 1024;         /* round up, like GNU */
        u++;
    }
    if (x < 100)
        snprintf(b, n, "%llu.%llu%c", x / 10, x % 10, units[u]);
    else
        snprintf(b, n, "%llu%c", (x + 9) / 10, units[u]);
}

static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static void fmt_time(char *b, size_t n, unsigned long long ns)
{
    time_t t = (time_t)(ns / 1000000000ull);
    struct tm tm;
    gmtime_r(&t, &tm);
    time_t now = time(0);
    int recent = t <= now + 60 && now - t < 182 * 24 * 3600L;   /* six months */
    switch (time_style) {
    case T_FULL:
        snprintf(b, n, "%d-%02d-%02d %02d:%02d:%02d.%09llu +0000", tm.tm_year + 1900,
                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ns % 1000000000ull);
        break;
    case T_LONG:
        snprintf(b, n, "%d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min);
        break;
    case T_ISO:
        if (recent)
            snprintf(b, n, "%02d-%02d %02d:%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
        else
            snprintf(b, n, "%d-%02d-%02d ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        break;
    default:
        if (recent)
            snprintf(b, n, "%s %2d %02d:%02d", months[tm.tm_mon % 12], tm.tm_mday, tm.tm_hour,
                     tm.tm_min);
        else
            snprintf(b, n, "%s %2d  %d", months[tm.tm_mon % 12], tm.tm_mday, tm.tm_year + 1900);
    }
}

static unsigned long long blocks_of(struct ent *e)
{
    /* st_blocks is in 512-byte units */
    unsigned long long b = e->st.st_blocks;
    return kblocks ? (b + 1) / 2 : b;
}

/* ---- sorting ---- */

static const char *ext_of(const char *s)
{
    const char *d = strrchr(s, '.');
    return d && d != s ? d : "";
}

/* Natural order: runs of digits compare as numbers. */
static int vercmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
            while (*a == '0')
                a++;
            while (*b == '0')
                b++;
            int la = 0, lb = 0;
            while (a[la] >= '0' && a[la] <= '9')
                la++;
            while (b[lb] >= '0' && b[lb] <= '9')
                lb++;
            if (la != lb)
                return la - lb;
            int c = strncmp(a, b, la);
            if (c)
                return c;
            a += la;
            b += lb;
            continue;
        }
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static unsigned long long etime(struct ent *e)
{
    return use_ctime ? e->st.st_ctime : e->st.st_mtime;
}

static int cmp(struct ent *a, struct ent *b)
{
    if (dirs_first) {
        int da = a->ok && S_ISDIR(a->st.st_mode), db = b->ok && S_ISDIR(b->st.st_mode);
        if (da != db)
            return db - da;
    }
    int r = 0;
    switch (sort) {
    case S_TIME: {
        unsigned long long x = etime(a), y = etime(b);
        r = x > y ? -1 : x < y;
        break;
    }
    case S_SIZE:
        r = a->st.st_size > b->st.st_size ? -1 : a->st.st_size < b->st.st_size;
        break;
    case S_EXT:
        r = strcmp(ext_of(a->name), ext_of(b->name));
        break;
    case S_VERSION:
        r = vercmp(a->name, b->name);
        break;
    }
    if (!r && sort != S_VERSION)
        r = strcmp(a->name, b->name);
    return reverse ? -r : r;
}

static void sort_ents(struct ent *v, int n)
{
    if (sort == S_NONE && !dirs_first) {
        if (reverse)
            for (int i = 0, j = n - 1; i < j; i++, j--) {
                struct ent t = v[i];
                v[i] = v[j];
                v[j] = t;
            }
        return;
    }
    /* merge sort (stable) */
    if (n < 2)
        return;
    struct ent *tmp = xmalloc(n * sizeof(*tmp));
    for (int w = 1; w < n; w *= 2) {
        for (int lo = 0; lo < n; lo += 2 * w) {
            int mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi)
                tmp[k++] = cmp(&v[j], &v[i]) < 0 ? v[j++] : v[i++];
            while (i < mid)
                tmp[k++] = v[i++];
            while (j < hi)
                tmp[k++] = v[j++];
        }
        memcpy(v, tmp, n * sizeof(*v));
    }
    free(tmp);
}

/* ---- output ---- */

static int out_tty;

static int term_width(void)
{
    if (width > 0)
        return width;
    const char *c = getenv("COLUMNS");
    if (c && atoi(c) > 0)
        return atoi(c);
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col)
        return ws.ws_col;
    return 80;
}

static void print_prefix(struct ent *e, int iw, int bw, char *buf, size_t n)
{
    int k = 0;
    buf[0] = 0;
    if (show_inode)
        k += snprintf(buf + k, n - k, "%*u ", iw, e->ok ? e->st.st_ino : 0);
    if (show_blocks) {
        char b[24];
        if (human)
            human_size(b, sizeof(b), blocks_of(e) * (kblocks ? 1024 : 512));
        else
            snprintf(b, sizeof(b), "%llu", e->ok ? blocks_of(e) : 0);
        k += snprintf(buf + k, n - k, "%*s ", bw, b);
    }
}

static void list_long(struct ent *v, int n)
{
    int lw = 1, uw = 1, gw = 1, sw = 1, iw = 1, bw = 1, majw = 0, minw = 0;
    char b[64];
    for (int i = 0; i < n; i++) {
        struct ent *e = &v[i];
        if (!e->ok)
            continue;
        int l = snprintf(b, sizeof(b), "%u", e->st.st_nlink);
        if (l > lw) lw = l;
        l = strlen(id_name(e->st.st_uid, 0));
        if (l > uw) uw = l;
        l = strlen(id_name(e->st.st_gid, 1));
        if (l > gw) gw = l;
        if (S_ISCHR(e->st.st_mode)) {
            l = snprintf(b, sizeof(b), "%u", e->st.st_rdev >> 8);
            if (l > majw) majw = l;
            l = snprintf(b, sizeof(b), "%u", e->st.st_rdev & 0xff);
            if (l > minw) minw = l;
        } else {
            if (human)
                human_size(b, sizeof(b), e->st.st_size);
            else
                snprintf(b, sizeof(b), "%llu", (unsigned long long)e->st.st_size);
            l = strlen(b);
            if (l > sw) sw = l;
        }
        l = snprintf(b, sizeof(b), "%u", e->st.st_ino);
        if (l > iw) iw = l;
        if (human)
            human_size(b, sizeof(b), blocks_of(e) * (kblocks ? 1024 : 512));
        else
            snprintf(b, sizeof(b), "%llu", blocks_of(e));
        l = strlen(b);
        if (l > bw) bw = l;
    }
    if (majw && majw + minw + 2 > sw)
        sw = majw + minw + 2;
    for (int i = 0; i < n; i++) {
        struct ent *e = &v[i];
        char pre[64];
        print_prefix(e, iw, bw, pre, sizeof(pre));
        int w;
        char *name = decorated(e, &w);
        if (!e->ok) {
            printf("%s?????????? ? %*s %*s %*s            ? %s\n", pre, uw, "?", gw, "?", sw, "?", name);
            free(name);
            continue;
        }
        unsigned m = e->st.st_mode;
        char mode[11];
        mode[0] = S_ISDIR(m) ? 'd' : S_ISCHR(m) ? 'c' : S_ISFIFO(m) ? 'p' : '-';
        for (int k = 0; k < 9; k++)
            mode[1 + k] = m & (0400 >> k) ? "rwxrwxrwx"[k] : '-';
        if (m & S_ISUID)
            mode[3] = mode[3] == 'x' ? 's' : 'S';
        if (m & S_ISGID)
            mode[6] = mode[6] == 'x' ? 's' : 'S';
        mode[10] = 0;
        char size[48];
        if (S_ISCHR(m))
            snprintf(size, sizeof(size), "%*u, %*u", majw, e->st.st_rdev >> 8, minw,
                     e->st.st_rdev & 0xff);
        else if (human)
            human_size(size, sizeof(size), e->st.st_size);
        else
            snprintf(size, sizeof(size), "%llu", (unsigned long long)e->st.st_size);
        char tbuf[48];
        fmt_time(tbuf, sizeof(tbuf), etime(e));
        printf("%s%s %*u ", pre, mode, lw, e->st.st_nlink);
        if (!no_owner)
            printf("%-*s ", uw, id_name(e->st.st_uid, 0));
        if (!no_group)
            printf("%-*s ", gw, id_name(e->st.st_gid, 1));
        printf("%*s %s %s\n", sw, size, tbuf, name);
        free(name);
    }
}

static void list_columns(struct ent *v, int n)
{
    int iw = 1, bw = 1;
    char b[32];
    for (int i = 0; i < n; i++) {
        int l = snprintf(b, sizeof(b), "%u", v[i].ok ? v[i].st.st_ino : 0);
        if (l > iw) iw = l;
        if (human)
            human_size(b, sizeof(b), blocks_of(&v[i]) * (kblocks ? 1024 : 512));
        else
            snprintf(b, sizeof(b), "%llu", blocks_of(&v[i]));
        l = strlen(b);
        if (l > bw) bw = l;
    }
    char **cells = xmalloc(n * sizeof(char *));
    int *cw = xmalloc(n * sizeof(int));
    for (int i = 0; i < n; i++) {
        char pre[64];
        print_prefix(&v[i], iw, bw, pre, sizeof(pre));
        int w;
        char *name = decorated(&v[i], &w);
        cells[i] = xmalloc(strlen(pre) + strlen(name) + 1);
        strcpy(cells[i], pre);
        strcat(cells[i], name);
        cw[i] = strlen(pre) + w;
        free(name);
    }
    if (format == F_ONE) {
        for (int i = 0; i < n; i++)
            printf("%s\n", cells[i]);
    } else if (format == F_COMMAS) {
        int tw = term_width(), col = 0;
        for (int i = 0; i < n; i++) {
            int need = cw[i] + (i + 1 < n ? 1 : 0);
            if (col && col + 1 + need > tw) {
                printf("\n");
                col = 0;
            } else if (col) {
                printf(" ");
                col++;
            }
            printf("%s%s", cells[i], i + 1 < n ? "," : "");
            col += need;
        }
        if (n)
            printf("\n");
    } else {
        /* GNU's layout: as many columns as fit, each as wide as its widest entry + 2 */
        int tw = term_width(), across = format == F_ACROSS;
        int best_cols = 1;
        int *colw = xmalloc((n + 1) * sizeof(int));
        for (int ncol = n < tw / 3 + 1 ? n : tw / 3 + 1; ncol > 1; ncol--) {
            int nrow = (n + ncol - 1) / ncol;
            int real_cols = across ? ncol : (n + nrow - 1) / nrow;
            int total = 0;
            for (int c = 0; c < real_cols; c++) {
                int w = 0;
                for (int r = 0; r < nrow; r++) {
                    int i = across ? r * ncol + c : c * nrow + r;
                    if (i < n && cw[i] > w)
                        w = cw[i];
                }
                total += w + (c + 1 < real_cols ? 2 : 0);
            }
            if (total <= tw) {
                best_cols = ncol;
                break;
            }
        }
        int nrow = (n + best_cols - 1) / best_cols;
        int ncol = across ? best_cols : (nrow ? (n + nrow - 1) / nrow : 0);
        for (int c = 0; c < ncol; c++) {
            colw[c] = 0;
            for (int r = 0; r < nrow; r++) {
                int i = across ? r * best_cols + c : c * nrow + r;
                if (i < n && cw[i] > colw[c])
                    colw[c] = cw[i];
            }
        }
        for (int r = 0; r < nrow; r++) {
            for (int c = 0; c < ncol; c++) {
                int i = across ? r * best_cols + c : c * nrow + r;
                if (i >= n)
                    continue;
                int next = across ? i + 1 : i + nrow;
                int last = c + 1 == ncol || next >= n || (across && c + 1 >= best_cols);
                printf("%s", cells[i]);
                if (!last)
                    printf("%*s", colw[c] - cw[i] + 2, "");
            }
            printf("\n");
        }
        free(colw);
    }
    for (int i = 0; i < n; i++)
        free(cells[i]);
    free(cells);
    free(cw);
}

static void list_ents(struct ent *v, int n)
{
    if (format == F_LONG)
        list_long(v, n);
    else
        list_columns(v, n);
}

/* ---- directories ---- */

static int hidden(const char *name)
{
    if (name[0] == '.') {
        if (!show_all && !almost_all)
            return 1;
        if (almost_all && !show_all && (!strcmp(name, ".") || !strcmp(name, "..")))
            return 1;
    }
    size_t n = strlen(name);
    if (ignore_backups && n && name[n - 1] == '~')
        return 1;
    for (int i = 0; i < nhide; i++) {
        /* simple glob: * and ? */
        const char *p = hide[i], *s = name, *star = 0, *ss = 0;
        while (*s) {
            if (*p == '*') {
                star = ++p;
                ss = s;
            } else if (*p == '?' || *p == *s) {
                p++;
                s++;
            } else if (star) {
                p = star;
                s = ++ss;
            } else {
                break;
            }
        }
        while (*p == '*')
            p++;
        if (!*s && !*p)
            return 1;
    }
    return 0;
}

static int need_stat(void)
{
    return format == F_LONG || sort == S_TIME || sort == S_SIZE || classify || slash_dirs ||
           file_type || color || show_blocks || recursive || dirs_first;
}

static void free_ents(struct ent *v, int n)
{
    for (int i = 0; i < n; i++) {
        free(v[i].name);
        free(v[i].path);
    }
    free(v);
}

static void list_dir(const char *path, int header, int *first)
{
    DIR *d = opendir(path);
    if (!d) {
        eprintf("ls: cannot open directory '%s': %s\n", path, strerror(errno));
        status = status > 1 ? status : 1;
        return;
    }
    int cap = 32, n = 0;
    struct ent *v = xmalloc(cap * sizeof(*v));
    struct dirent *de;
    int st_needed = need_stat();
    while ((de = readdir(d))) {
        if (hidden(de->d_name))
            continue;
        if (n == cap) {
            cap *= 2;
            struct ent *nv = xmalloc(cap * sizeof(*nv));
            memcpy(nv, v, n * sizeof(*v));
            free(v);
            v = nv;
        }
        struct ent *e = &v[n++];
        memset(e, 0, sizeof(*e));
        e->name = xstrdup(de->d_name);
        e->path = join(path, de->d_name);
        if (st_needed || show_inode)
            e->ok = stat(e->path, &e->st) == 0;
        else {
            e->ok = 1;
            e->st.st_ino = de->d_ino;
            e->st.st_mode = de->d_type == DT_DIR ? S_IFDIR : de->d_type == DT_CHR ? S_IFCHR : S_IFREG;
        }
    }
    closedir(d);
    sort_ents(v, n);
    if (header) {
        if (!*first)
            printf("\n");
        int w;
        char *q = quoted(path, &w);
        printf("%s:\n", q);
        free(q);
    }
    *first = 0;
    if (format == F_LONG || show_blocks) {
        unsigned long long total = 0;
        for (int i = 0; i < n; i++)
            if (v[i].ok)
                total += blocks_of(&v[i]);
        if (human) {
            char b[24];
            human_size(b, sizeof(b), total * (kblocks ? 1024 : 512));
            printf("total %s\n", b);
        } else {
            printf("total %llu\n", total);
        }
    }
    list_ents(v, n);
    if (recursive)
        for (int i = 0; i < n; i++)
            if (v[i].ok && S_ISDIR(v[i].st.st_mode) && strcmp(v[i].name, ".") &&
                strcmp(v[i].name, ".."))
                list_dir(v[i].path, 1, first);
    free_ents(v, n);
}

/* ---- arguments ---- */

static void usage(int full)
{
    printf("usage: ls [OPTION]... [FILE]...\n");
    if (!full) {
        printf("Try 'ls --help' for more information.\n");
        return;
    }
    printf("List information about the FILEs (the current directory by default).\n\n"
           "  -a, --all            include entries starting with .\n"
           "  -A, --almost-all     ... except . and ..\n"
           "  -d, --directory      list directories themselves, not their contents\n"
           "  -R, --recursive      list subdirectories recursively\n"
           "  -l                   long listing      -o / -g  without group / owner\n"
           "  -n, --numeric-uid-gid                  numeric user and group ids\n"
           "  -1  one per line   -C  columns   -x  columns across   -m  commas\n"
           "  -t  by time   -S  by size   -X  by extension   -v  natural order   -U  unsorted\n"
           "  -r, --reverse        reverse the order\n"
           "  -c                   ctime instead of modification time\n"
           "      --group-directories-first\n"
           "  -h, --human-readable sizes like 1.5K 23M   -k  1K blocks (default)\n"
           "  -s, --size           allocated blocks     -i, --inode   inode numbers\n"
           "  -F, --classify       append / * |          -p  append / to directories\n"
           "      --file-type      like -F without *\n"
           "      --color[=WHEN]   colors: always, auto (if a terminal), never; -G = auto\n"
           "  -Q, --quote-name     \"names\"  -N, --literal   -b, --escape   -q  ? for unprintable\n"
           "  -B, --ignore-backups              -I, --ignore=PATTERN, --hide=PATTERN\n"
           "      --full-time      --time-style=full-iso|long-iso|iso|locale\n"
           "  -w, --width=COLS\n\n"
           "Colors come from LS_COLORS (di= ex= cd= pi= ow= *.ext=...). Times are UTC.\n"
           "Exit status: 0 if OK, 1 for minor problems, 2 for serious trouble.\n");
}

static int set_color(const char *when)
{
    if (!when || !strcmp(when, "always") || !strcmp(when, "yes") || !strcmp(when, "force"))
        return 1;
    if (!strcmp(when, "never") || !strcmp(when, "no") || !strcmp(when, "none"))
        return 0;
    if (!strcmp(when, "auto") || !strcmp(when, "tty") || !strcmp(when, "if-tty"))
        return isatty(1);
    eprintf("ls: invalid argument '%s' for '--color'\n", when);
    exit(2);
}

static int long_opt(const char *a, int *i, int argc, char **argv)
{
    const char *eq = strchr(a, '=');
    char key[40];
    snprintf(key, sizeof(key), "%.*s", eq ? (int)(eq - a) : (int)strlen(a), a);
    const char *val = eq ? eq + 1 : 0;
#define NEEDVAL()                                                       \
    do {                                                                \
        if (!val) {                                                     \
            if (*i + 1 >= argc) {                                       \
                eprintf("ls: option '--%s' requires an argument\n", key); \
                exit(2);                                                \
            }                                                           \
            val = argv[++*i];                                           \
        }                                                               \
    } while (0)
    if (!strcmp(key, "all")) show_all = 1;
    else if (!strcmp(key, "almost-all")) almost_all = 1;
    else if (!strcmp(key, "directory")) dir_itself = 1;
    else if (!strcmp(key, "recursive")) recursive = 1;
    else if (!strcmp(key, "reverse")) reverse = 1;
    else if (!strcmp(key, "human-readable")) human = 1;
    else if (!strcmp(key, "size")) show_blocks = 1;
    else if (!strcmp(key, "inode")) show_inode = 1;
    else if (!strcmp(key, "classify")) classify = 1;
    else if (!strcmp(key, "file-type")) file_type = 1;
    else if (!strcmp(key, "numeric-uid-gid")) { numeric = 1; format = F_LONG; }
    else if (!strcmp(key, "color") || !strcmp(key, "colour")) color = set_color(val);
    else if (!strcmp(key, "quote-name")) quoting = Q_C;
    else if (!strcmp(key, "literal")) quoting = Q_LITERAL;
    else if (!strcmp(key, "escape")) quoting = Q_ESCAPE;
    else if (!strcmp(key, "hide-control-chars")) quoting = Q_HIDE;
    else if (!strcmp(key, "ignore-backups")) ignore_backups = 1;
    else if (!strcmp(key, "ignore") || !strcmp(key, "hide")) {
        NEEDVAL();
        if (nhide < 16)
            hide[nhide++] = xstrdup(val);
    } else if (!strcmp(key, "group-directories-first")) dirs_first = 1;
    else if (!strcmp(key, "full-time")) { format = F_LONG; time_style = T_FULL; }
    else if (!strcmp(key, "time-style")) {
        NEEDVAL();
        if (val[0] == '+')
            val++;
        time_style = !strcmp(val, "full-iso") ? T_FULL : !strcmp(val, "long-iso") ? T_LONG
                   : !strcmp(val, "iso") ? T_ISO : T_DEFAULT;
    } else if (!strcmp(key, "sort")) {
        NEEDVAL();
        sort = !strcmp(val, "time") ? S_TIME : !strcmp(val, "size") ? S_SIZE
             : !strcmp(val, "extension") ? S_EXT : !strcmp(val, "version") ? S_VERSION
             : !strcmp(val, "none") ? S_NONE : S_NAME;
    } else if (!strcmp(key, "format")) {
        NEEDVAL();
        format = !strcmp(val, "long") || !strcmp(val, "verbose") ? F_LONG
               : !strcmp(val, "single-column") ? F_ONE : !strcmp(val, "commas") ? F_COMMAS
               : !strcmp(val, "across") || !strcmp(val, "horizontal") ? F_ACROSS : F_COLUMNS;
    } else if (!strcmp(key, "width")) {
        NEEDVAL();
        width = atoi(val);
    } else if (!strcmp(key, "help")) {
        usage(1);
        exit(0);
    } else if (!strcmp(key, "version")) {
        printf("ls (Eunuchs) 1.0, GNU-style\n");
        exit(0);
    } else {
        eprintf("ls: unrecognized option '--%s'\n", key);
        usage(0);
        exit(2);
    }
#undef NEEDVAL
    return 0;
}

int main(int argc, char **argv)
{
    out_tty = isatty(1);
    char **files = xmalloc((argc + 1) * sizeof(char *));
    int nfiles = 0, opts_done = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (opts_done || a[0] != '-' || !a[1]) {
            files[nfiles++] = argv[i];
            continue;
        }
        if (!strcmp(a, "--")) {
            opts_done = 1;
            continue;
        }
        if (a[1] == '-') {
            long_opt(a + 2, &i, argc, argv);
            continue;
        }
        for (const char *c = a + 1; *c; c++) {
            switch (*c) {
            case 'a': show_all = 1; break;
            case 'A': almost_all = 1; break;
            case 'd': dir_itself = 1; break;
            case 'R': recursive = 1; break;
            case 'l': format = F_LONG; break;
            case 'o': format = F_LONG; no_group = 1; break;
            case 'g': format = F_LONG; no_owner = 1; break;
            case 'n': format = F_LONG; numeric = 1; break;
            case '1': format = F_ONE; break;
            case 'C': format = F_COLUMNS; break;
            case 'x': format = F_ACROSS; break;
            case 'm': format = F_COMMAS; break;
            case 't': sort = S_TIME; break;
            case 'S': sort = S_SIZE; break;
            case 'X': sort = S_EXT; break;
            case 'v': sort = S_VERSION; break;
            case 'U': sort = S_NONE; break;
            case 'f': sort = S_NONE; show_all = 1; color = 0; break;
            case 'r': reverse = 1; break;
            case 'c': use_ctime = 1; break;
            case 'u': break;                    /* access time: not kept; use mtime */
            case 'h': human = 1; break;
            case 'k': kblocks = 1; break;
            case 's': show_blocks = 1; break;
            case 'i': show_inode = 1; break;
            case 'F': classify = 1; break;
            case 'p': slash_dirs = 1; break;
            case 'G': color = isatty(1); break;
            case 'Q': quoting = Q_C; break;
            case 'N': quoting = Q_LITERAL; break;
            case 'b': quoting = Q_ESCAPE; break;
            case 'q': quoting = Q_HIDE; break;
            case 'B': ignore_backups = 1; break;
            case 'L': case 'H': case 'T': break;    /* no symbolic links; tab size */
            case 'I':
            case 'w': {
                char opt = *c;
                const char *v = c[1] ? c + 1 : i + 1 < argc ? argv[++i] : 0;
                if (!v) {
                    eprintf("ls: option requires an argument -- '%c'\n", opt);
                    usage(0);
                    return 2;
                }
                if (opt == 'I') {
                    if (nhide < 16)
                        hide[nhide++] = xstrdup(v);
                } else {
                    width = atoi(v);
                }
                c += strlen(c) - 1;
                break;
            }
            default:
                eprintf("ls: invalid option -- '%c'\n", *c);
                usage(0);
                return 2;
            }
        }
    }
    if (format < 0)
        format = out_tty ? F_COLUMNS : F_ONE;
    if (quoting < 0)
        quoting = out_tty ? Q_SHELL : Q_LITERAL;
    if (color)
        load_colors();
    if (use_ctime && format != F_LONG && sort == S_NAME)
        sort = S_TIME;                          /* GNU: -c alone sorts by ctime */
    if (!nfiles)
        files[nfiles++] = ".";

    /* Command-line arguments: files first, then directories */
    struct ent *fv = xmalloc(nfiles * sizeof(*fv)), *dv = xmalloc(nfiles * sizeof(*dv));
    int nf = 0, nd = 0;
    for (int i = 0; i < nfiles; i++) {
        struct ent e;
        memset(&e, 0, sizeof(e));
        e.name = xstrdup(files[i]);
        e.path = xstrdup(files[i]);
        if (stat(files[i], &e.st) < 0) {
            eprintf("ls: cannot access '%s': %s\n", files[i], strerror(errno));
            status = 2;
            free(e.name);
            free(e.path);
            continue;
        }
        e.ok = 1;
        if (S_ISDIR(e.st.st_mode) && !dir_itself)
            dv[nd++] = e;
        else
            fv[nf++] = e;
    }
    sort_ents(fv, nf);
    sort_ents(dv, nd);
    int first = 1;
    if (nf) {
        list_ents(fv, nf);
        first = 0;
    }
    int headers = nf + nd > 1 || recursive || status;
    for (int i = 0; i < nd; i++)
        list_dir(dv[i].path, headers, &first);
    free_ents(fv, nf);
    free_ents(dv, nd);
    free(files);
    return status;
}
