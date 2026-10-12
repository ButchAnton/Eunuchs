/* edit: the prompt line (ex commands, searches, file names, M-x), and
 * running shell commands. */
#include "edit.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

const char *const *mx_commands;

#define NHIST 32
struct history {
    char label[32];
    char *v[NHIST];
    int n;
};
static struct history hists[6];

static struct history *hist_for(const char *label)
{
    for (int i = 0; i < 6; i++)
        if (!strncmp(hists[i].label, label, sizeof(hists[i].label) - 1))
            return &hists[i];
    for (int i = 0; i < 6; i++)
        if (!hists[i].label[0]) {
            snprintf(hists[i].label, sizeof(hists[i].label), "%s", label);
            return &hists[i];
        }
    return &hists[0];
}

static void hist_add(struct history *h, const char *s)
{
    if (!*s || (h->n && !strcmp(h->v[h->n - 1], s)))
        return;
    if (h->n == NHIST) {
        free(h->v[0]);
        memmove(h->v, h->v + 1, (NHIST - 1) * sizeof(char *));
        h->n--;
    }
    h->v[h->n++] = xstrdup(s);
}

/* Complete the word ending at buf[n]: file names or M-x commands. */
static void complete(char *buf, int *n, int size, int kind)
{
    char cands[64][64];
    int nc = 0;
    if (kind == 'c' && mx_commands) {
        for (int i = 0; mx_commands[i] && nc < 64; i++)
            if (!strncmp(mx_commands[i], buf, *n))
                snprintf(cands[nc++], 64, "%s", mx_commands[i]);
    } else if (kind == 'f') {
        buf[*n] = 0;
        int start = *n;
        while (start > 0 && buf[start - 1] != ' ')
            start--;
        char *word = buf + start;
        char dir[256] = ".";
        const char *base = word;
        const char *slash = strrchr(word, '/');
        if (slash) {
            snprintf(dir, sizeof(dir), "%.*s", (int)(slash - word) ? (int)(slash - word) : 1, word);
            base = slash + 1;
        }
        DIR *d = opendir(dir);
        struct dirent *de;
        while (d && (de = readdir(d)) && nc < 64) {
            if (de->d_name[0] == '.' && base[0] != '.')
                continue;
            if (strncmp(de->d_name, base, strlen(base)))
                continue;
            char full[320];
            snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
            struct stat st;
            int isdir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
            snprintf(cands[nc++], 64, "%.*s%s%s", (int)(base - buf), buf, de->d_name, isdir ? "/" : "");
        }
        if (d)
            closedir(d);
    }
    if (!nc) {
        msg("[No match]");
        return;
    }
    /* longest common prefix */
    int lcp = strlen(cands[0]);
    for (int i = 1; i < nc; i++) {
        int k = 0;
        while (k < lcp && cands[i][k] == cands[0][k])
            k++;
        lcp = k;
    }
    if (lcp > *n && lcp < size) {
        memcpy(buf, cands[0], lcp);
        *n = lcp;
        buf[*n] = 0;
    }
    if (nc > 1) {
        char list[256];
        int k = 0;
        for (int i = 0; i < nc && k < (int)sizeof(list) - 20; i++) {
            const char *c = cands[i];
            const char *sl = strrchr(c, '/');
            if (sl && sl[1])
                c = sl + 1;
            k += snprintf(list + k, sizeof(list) - k, "%s%s", i ? " " : "", c);
        }
        msg("%s", list);
    }
}

char *prompt(const char *label, const char *initial, int kind)
{
    static char buf[512];
    snprintf(buf, sizeof(buf), "%s", initial ? initial : "");
    int n = strlen(buf), pos = n;
    struct history *h = hist_for(label);
    int hidx = h->n;
    for (;;) {
        draw();
        /* the prompt replaces the bottom line */
        outf("\033[%d;1H\033[K%s", rows, label);
        int lw = strlen(label);
        int start = 0;
        if (lw + n >= cols - 1)
            start = lw + n - (cols - 2);    /* keep the end visible */
        outn(buf + start, n - start);
        outf("\033[%d;%dH", rows, lw + pos - start + 1);
        out_flush();
        message[0] = 0;
        int k = term_getkey();
        if (k == '\r' || k == '\n') {
            buf[n] = 0;
            hist_add(h, buf);
            return buf;
        }
        if (k == K_ESC || k == CTRL('G') || k == CTRL('C'))
            return 0;
        if (k == 127 || k == CTRL('H')) {
            if (!pos) {
                if (!n)
                    return 0;           /* backspace past the start cancels, as in vi */
                continue;
            }
            memmove(buf + pos - 1, buf + pos, n - pos);
            n--, pos--;
        } else if (k == K_DEL || k == CTRL('D')) {
            if (pos < n) {
                memmove(buf + pos, buf + pos + 1, n - pos - 1);
                n--;
            }
        } else if (k == K_LEFT || k == CTRL('B')) {
            if (pos)
                pos--;
        } else if (k == K_RIGHT || k == CTRL('F')) {
            if (pos < n)
                pos++;
        } else if (k == K_HOME || k == CTRL('A')) {
            pos = 0;
        } else if (k == K_END || k == CTRL('E')) {
            pos = n;
        } else if (k == CTRL('U')) {
            memmove(buf, buf + pos, n - pos);
            n -= pos;
            pos = 0;
        } else if (k == CTRL('K')) {
            n = pos;
        } else if (k == CTRL('W')) {
            int p = pos;
            while (p > 0 && buf[p - 1] == ' ')
                p--;
            while (p > 0 && buf[p - 1] != ' ' && buf[p - 1] != '/')
                p--;
            memmove(buf + p, buf + pos, n - pos);
            n -= pos - p;
            pos = p;
        } else if (k == K_UP || k == K_DOWN || k == CTRL('P') || k == CTRL('N')) {
            int dir = k == K_UP || k == CTRL('P') ? -1 : 1;
            if (hidx + dir < 0 || hidx + dir > h->n)
                continue;
            hidx += dir;
            snprintf(buf, sizeof(buf), "%s", hidx < h->n ? h->v[hidx] : "");
            n = pos = strlen(buf);
        } else if (k == '\t' && kind) {
            buf[n] = 0;
            complete(buf, &n, sizeof(buf) - 1, kind);
            pos = n;
        } else if (k == K_RESIZE) {
            continue;
        } else if (k >= 32 && k < 256 && n < (int)sizeof(buf) - 1) {
            memmove(buf + pos + 1, buf + pos, n - pos);
            buf[pos++] = (char)k;
            n++;
        }
        buf[n] = 0;
    }
}

int prompt_yes_no(const char *question)
{
    for (;;) {
        draw();
        outf("\033[%d;1H\033[K%s", rows, question);
        out_flush();
        int k = term_getkey();
        if (k == 'y' || k == 'Y')
            return 1;
        if (k == 'n' || k == 'N')
            return 0;
        if (k == K_ESC || k == CTRL('G') || k == CTRL('C'))
            return -1;
    }
}

int run_shell_command(const char *cmd, char **output, int *outlen)
{
    int fds[2];
    if (pipe(fds) < 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[1]);
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) {
            dup2(null, 0);
            close(null);
        }
        char *argv[] = { "sh", "-c", (char *)cmd, 0 };
        execv("/bin/sh", argv);
        _exit(127);
    }
    close(fds[1]);
    int cap = 4096, n = 0;
    char *s = xmalloc(cap);
    for (;;) {
        if (n + 1024 > cap) {
            cap *= 2;
            s = xrealloc(s, cap);
        }
        long r = read(fds[0], s + n, cap - n - 1);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        n += r;
    }
    close(fds[0]);
    s[n] = 0;
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    *output = s;
    *outlen = n;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
