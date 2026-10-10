/*
 * TTY: the line discipline for the console, plus job control.
 *
 * Input (from the UART interrupt): ICRNL; ISIG characters (^C SIGINT,
 * ^\ SIGQUIT, ^Z SIGTSTP) signal the foreground process group; in canonical
 * mode a line is edited (erase, ^U kill, ^W word erase) and becomes readable
 * at newline, ^D ends it (^D on an empty line reads as EOF); ECHO echoes,
 * control characters as ^X. Raw mode delivers bytes as they come.
 *
 * Job control: the tty belongs to a session (its controlling terminal) and
 * has a foreground process group. A read from a background group stops it
 * with SIGTTIN; tcsetpgrp changes the foreground.
 *
 * Debugging: `tty` in the monitor and /proc/tty show the termios flags, the
 * foreground group and session, the line being edited and queued input.
 */
#define LOG_SUBSYS SUBSYS_DRV
#include <console.h>
#include <debug.h>
#include <fmt.h>
#include <fs.h>
#include <kstring.h>
#include <printk.h>
#include <proc.h>
#include <spinlock.h>
#include <trace.h>
#include <uapi/errno.h>
#include <uapi/net.h>
#include <uapi/termios.h>

#define LINE_MAX  255
#define QUEUE     1024

struct tty {
    spinlock_t lock;
    struct termios t;
    char line[LINE_MAX + 1];
    int line_len;
    char q[QUEUE];              /* readable input */
    u32 qhead, qtail;
    int eofs;                   /* pending ^D-on-empty-line markers */
    int lines;                  /* complete lines in q (canonical mode) */
    struct waitqueue rwait;
    int sid, pgrp;
    struct winsize ws;
    u64 in, out, dropped, sigs;
};

static struct tty console_tty = {
    .lock = SPINLOCK_INIT("tty"),
    .rwait = WAITQUEUE_INIT(console_tty.rwait, "tty-read"),
    .ws = { 24, 80, 0, 0 },
};

static void tty_defaults(struct tty *tty)
{
    struct termios *t = &tty->t;
    memset(t, 0, sizeof(*t));
    t->c_iflag = ICRNL | IXON;
    t->c_oflag = OPOST | ONLCR;
    t->c_cflag = CS8 | CREAD;
    t->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | IEXTEN;
    t->c_cc[VINTR] = 3;         /* ^C */
    t->c_cc[VQUIT] = 0x1c;      /* ^\ */
    t->c_cc[VERASE] = 0x7f;     /* DEL */
    t->c_cc[VKILL] = 0x15;      /* ^U */
    t->c_cc[VEOF] = 4;          /* ^D */
    t->c_cc[VMIN] = 1;
    t->c_cc[VSUSP] = 0x1a;      /* ^Z */
    t->c_cc[VWERASE] = 0x17;    /* ^W */
}

void tty_init(void)
{
    tty_defaults(&console_tty);
}

struct tty *tty_console(void) { return &console_tty; }

static void out_raw(struct tty *tty, const char *s, size_t n)
{
    (void)tty;
    console_write_locked(s, n);
}

static void echo_char(struct tty *tty, char c)
{
    if (!(tty->t.c_lflag & ECHO)) {
        if (c == '\n' && (tty->t.c_lflag & ECHONL))
            out_raw(tty, "\n", 1);
        return;
    }
    if ((tty->t.c_lflag & ECHOCTL) && (unsigned char)c < 0x20 && c != '\n' && c != '\t') {
        char s[2] = { '^', (char)(c + '@') };
        out_raw(tty, s, 2);
    } else {
        out_raw(tty, &c, 1);
    }
}

static void erase_chars(struct tty *tty, int n)
{
    while (n-- > 0 && tty->line_len > 0) {
        char c = tty->line[--tty->line_len];
        if (!(tty->t.c_lflag & ECHO) || !(tty->t.c_lflag & ECHOE))
            continue;
        int width = (tty->t.c_lflag & ECHOCTL) && (unsigned char)c < 0x20 ? 2 : 1;
        while (width--)
            out_raw(tty, "\b \b", 3);
    }
}

static void q_push(struct tty *tty, char c)
{
    if (tty->qhead - tty->qtail >= QUEUE) {
        tty->dropped++;
        return;
    }
    tty->q[tty->qhead++ % QUEUE] = c;
}

static void flush_input(struct tty *tty)
{
    tty->line_len = 0;
    tty->qhead = tty->qtail;
    tty->lines = tty->eofs = 0;
}

static void commit_line(struct tty *tty, bool newline)
{
    for (int i = 0; i < tty->line_len; i++)
        q_push(tty, tty->line[i]);
    if (newline)
        q_push(tty, '\n');
    if (!newline && !tty->line_len)
        tty->eofs++;
    else
        tty->lines++;
    tty->line_len = 0;
}

/* One input character, from the UART interrupt. */
void tty_input(char c)
{
    struct tty *tty = &console_tty;
    struct termios *t = &tty->t;
    int sig = 0;

    unsigned long flags = spin_lock_irqsave(&tty->lock);
    tty->in++;
    if (c == '\r' && (t->c_iflag & IGNCR))
        goto out;
    if (c == '\r' && (t->c_iflag & ICRNL))
        c = '\n';

    if (t->c_lflag & ISIG) {
        if (c == t->c_cc[VINTR])
            sig = SIGINT;
        else if (c == t->c_cc[VQUIT])
            sig = SIGQUIT;
        else if (c == t->c_cc[VSUSP])
            sig = SIGTSTP;
        if (sig) {
            if (!(t->c_lflag & NOFLSH))
                flush_input(tty);
            echo_char(tty, c);
            if (t->c_lflag & ECHO)
                out_raw(tty, "\n", 1);
            goto out;
        }
    }

    if (t->c_lflag & ICANON) {
        if (c == t->c_cc[VERASE] || c == '\b') {
            erase_chars(tty, 1);
        } else if (c == t->c_cc[VKILL]) {
            erase_chars(tty, tty->line_len);
        } else if (c == t->c_cc[VWERASE]) {
            while (tty->line_len && tty->line[tty->line_len - 1] == ' ')
                erase_chars(tty, 1);
            while (tty->line_len && tty->line[tty->line_len - 1] != ' ')
                erase_chars(tty, 1);
        } else if (c == t->c_cc[VEOF]) {
            commit_line(tty, false);
        } else if (c == '\n') {
            echo_char(tty, c);
            commit_line(tty, true);
        } else if (tty->line_len < LINE_MAX) {
            tty->line[tty->line_len++] = c;
            echo_char(tty, c);
        } else {
            tty->dropped++;
        }
    } else {
        q_push(tty, c);
        echo_char(tty, c);
    }
out:
    spin_unlock_irqrestore(&tty->lock, flags);
    if (sig) {
        tty->sigs++;
        trace(tty_signal, sig, tty->pgrp, 0);
        if (tty->pgrp)
            signal_send_pgrp(tty->pgrp, sig, 0);
    }
    wake_up(&tty->rwait);
}

static bool readable(struct tty *tty)
{
    if (tty->t.c_lflag & ICANON)
        return tty->lines > 0 || tty->eofs > 0;
    return tty->qhead != tty->qtail;
}

/* Background process group touching the terminal? Stop it (SIGTTIN). */
static long job_control(struct tty *tty)
{
    struct proc *p = current_proc();
    if (!p || p->ctty != tty || !tty->pgrp || p->pgid == tty->pgrp)
        return 0;
    if (p->sigactions[SIGTTIN].handler == (uintptr_t)SIG_IGN || (p->sig_blocked & SIGBIT(SIGTTIN)))
        return -EIO;
    signal_send_pgrp(p->pgid, SIGTTIN, p->pid);
    return -ERESTARTSYS;
}

static long tty_read(struct file *f, void *buf, size_t n, bool user)
{
    (void)f;
    struct tty *tty = &console_tty;
    long r = job_control(tty);
    if (r)
        return r;
    if (!(tty->t.c_lflag & ICANON) && tty->t.c_cc[VMIN] == 0 && !readable(tty))
        return 0;                       /* raw mode, VMIN 0: don't block */
    current->wait_what = "tty read";
    if (wait_event_interruptible(&tty->rwait, readable(tty)))
        return -ERESTARTSYS;

    char tmp[256];
    size_t got = 0;
    unsigned long flags = spin_lock_irqsave(&tty->lock);
    bool canon = tty->t.c_lflag & ICANON;
    if (canon && tty->lines == 0 && tty->eofs > 0) {
        tty->eofs--;                    /* ^D on an empty line: EOF */
    } else {
        while (got < MIN(n, sizeof(tmp)) && tty->qtail != tty->qhead) {
            char c = tty->q[tty->qtail++ % QUEUE];
            tmp[got++] = c;
            if (canon && c == '\n') {
                tty->lines--;
                break;
            }
        }
        if (canon && got && tmp[got - 1] != '\n' && tty->qtail == tty->qhead && tty->lines > 0)
            tty->lines--;               /* a ^D-terminated line was consumed */
    }
    spin_unlock_irqrestore(&tty->lock, flags);
    if (user ? copy_to_user((uintptr_t)buf, tmp, got) : (memcpy(buf, tmp, got), 0))
        return -EFAULT;
    return got;
}

static long tty_write(struct file *f, const void *buf, size_t n, bool user)
{
    (void)f;
    struct tty *tty = &console_tty;
    char tmp[256];
    size_t done = 0;
    while (done < n) {
        size_t c = MIN(n - done, sizeof(tmp));
        if (user ? copy_from_user(tmp, (uintptr_t)buf + done, c)
                 : (memcpy(tmp, (const u8 *)buf + done, c), 0))
            return done ? (long)done : -EFAULT;
        out_raw(tty, tmp, c);
        done += c;
    }
    tty->out += n;
    return n;
}

static long tty_ioctl(struct file *f, unsigned long req, uintptr_t arg)
{
    (void)f;
    struct tty *tty = &console_tty;
    struct proc *p = current_proc();
    switch (req) {
    case TCGETS:
        return copy_to_user(arg, &tty->t, sizeof(tty->t));
    case TCSETSF:
    case TCSETS:
    case TCSETSW: {
        struct termios t;
        if (copy_from_user(&t, arg, sizeof(t)))
            return -EFAULT;
        unsigned long flags = spin_lock_irqsave(&tty->lock);
        if (req == TCSETSF)
            flush_input(tty);
        bool was_canon = tty->t.c_lflag & ICANON;
        tty->t = t;
        if (was_canon && !(t.c_lflag & ICANON) && tty->line_len) {
            for (int i = 0; i < tty->line_len; i++)      /* hand over a partial line */
                q_push(tty, tty->line[i]);
            tty->line_len = 0;
        }
        spin_unlock_irqrestore(&tty->lock, flags);
        wake_up(&tty->rwait);
        return 0;
    }
    case TIOCGPGRP:
        return copy_to_user(arg, &tty->pgrp, sizeof(int));
    case TIOCSPGRP: {
        int pgrp;
        if (copy_from_user(&pgrp, arg, sizeof(pgrp)))
            return -EFAULT;
        if (!p || p->ctty != tty || p->sid != tty->sid)
            return -ENOTTY;
        tty->pgrp = pgrp;
        return 0;
    }
    case TIOCSCTTY:
        if (!p || p->sid != p->pid)
            return -EPERM;
        p->ctty = tty;
        tty->sid = p->sid;
        tty->pgrp = p->pgid;
        return 0;
    case TIOCNOTTY:
        if (p && p->ctty == tty)
            p->ctty = NULL;
        return 0;
    case TIOCGWINSZ:
        return copy_to_user(arg, &tty->ws, sizeof(tty->ws));
    case FIONREAD: {
        int n = (int)(tty->qhead - tty->qtail);
        return copy_to_user(arg, &n, sizeof(n));
    }
    }
    return -ENOTTY;
}

/* A new session leader gets the console as its controlling terminal. */
void tty_set_ctty(struct proc *p)
{
    p->ctty = &console_tty;
    console_tty.sid = p->sid;
    console_tty.pgrp = p->pgid;
}

static u32 tty_poll(struct file *f, struct poll_table *pt)
{
    (void)f;
    struct tty *tty = &console_tty;
    poll_wait(pt, &tty->rwait);
    return (readable(tty) ? POLLIN : 0) | POLLOUT;
}

const struct chr_ops tty_ops = { .read = tty_read, .write = tty_write, .ioctl = tty_ioctl,
                                 .poll = tty_poll };

struct flagname {
    tcflag_t bit;
    const char *name;
};

static void flagstr(print_fn print, const char *what, tcflag_t v, const struct flagname *names, int n)
{
    char buf[160];
    size_t len = ksnprintf(buf, sizeof(buf), "  %-6s", what);
    for (int i = 0; i < n; i++)
        len += ksnprintf(buf + len, sizeof(buf) - len, " %s%s", v & names[i].bit ? "" : "-",
                         names[i].name);
    print("%s\n", buf);
}

void tty_print(print_fn print)
{
    struct tty *tty = &console_tty;
    static const struct flagname lf[] = {
        { ISIG, "isig" }, { ICANON, "icanon" }, { ECHO, "echo" }, { ECHOE, "echoe" },
        { ECHOK, "echok" }, { ECHONL, "echonl" }, { NOFLSH, "noflsh" }, { ECHOCTL, "echoctl" },
    }, inf[] = { { ICRNL, "icrnl" }, { IGNCR, "igncr" }, { IXON, "ixon" } },
       of[] = { { OPOST, "opost" }, { ONLCR, "onlcr" } };
    print("console tty: session %d, foreground process group %d, %dx%d\n", tty->sid, tty->pgrp,
          tty->ws.ws_col, tty->ws.ws_row);
    flagstr(print, "lflag", tty->t.c_lflag, lf, ARRAY_SIZE(lf));
    flagstr(print, "iflag", tty->t.c_iflag, inf, ARRAY_SIZE(inf));
    flagstr(print, "oflag", tty->t.c_oflag, of, ARRAY_SIZE(of));
    print("  line being edited: \"%.*s\" (%d chars); queued %u bytes, %d lines, %d EOFs\n",
          tty->line_len, tty->line, tty->line_len, tty->qhead - tty->qtail, tty->lines, tty->eofs);
    print("  in %lu, out %lu, dropped %lu, signals sent %lu\n", (unsigned long)tty->in,
          (unsigned long)tty->out, (unsigned long)tty->dropped, (unsigned long)tty->sigs);
}
