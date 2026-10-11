/*
 * esh: job control.
 *
 * Every pipeline started by an interactive shell is a job: its own process
 * group, which owns the terminal while it runs in the foreground. Messages
 * follow zsh ("[1]  + done       sleep 1", "esh: suspended  vi").
 */
#include "esh.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>

struct job *jobtab[MAXJOBS];
int cur_job = -1, prev_job = -1;
static struct termios shell_modes;
static int have_shell_modes;

int job_index(struct job *j)
{
    for (int i = 0; i < MAXJOBS; i++)
        if (jobtab[i] == j)
            return i;
    return -1;
}

struct job *job_by_index(int i)
{
    return i >= 0 && i < MAXJOBS ? jobtab[i] : 0;
}

static void make_current(int i)
{
    if (cur_job == i)
        return;
    prev_job = cur_job;
    cur_job = i;
}

struct job *job_create(const char *text)
{
    for (int i = 0; i < MAXJOBS; i++)
        if (!jobtab[i]) {
            struct job *j = xmalloc(sizeof(*j));
            memset(j, 0, sizeof(*j));
            j->id = i + 1;
            j->text = xstrdup(text ? text : "");
            j->started = now_ms();
            jobtab[i] = j;
            return j;
        }
    sh_error("too many jobs");
    return 0;
}

void job_add_proc(struct job *j, pid_t pid)
{
    if (j->nprocs < MAXPROCS) {
        j->pids[j->nprocs] = pid;
        j->done[j->nprocs] = 0;
        j->status[j->nprocs] = 0;
        j->nprocs++;
    }
    if (!j->pgid)
        j->pgid = pid;
}

void job_free(struct job *j)
{
    int i = job_index(j);
    if (i >= 0)
        jobtab[i] = 0;
    if (cur_job == i) {
        cur_job = prev_job;
        prev_job = -1;
    } else if (prev_job == i) {
        prev_job = -1;
    }
    if (prev_job < 0) {
        /* the most recent other job becomes the previous one */
        for (int k = MAXJOBS - 1; k >= 0; k--)
            if (jobtab[k] && k != cur_job) {
                prev_job = k;
                break;
            }
    }
    if (cur_job < 0 && prev_job >= 0) {
        cur_job = prev_job;
        prev_job = -1;
    }
    free(j->text);
    free(j);
}

/* Any process not yet exited (running or stopped)? */
static int job_live(struct job *j)
{
    for (int i = 0; i < j->nprocs; i++)
        if (j->done[i] != 1)
            return 1;
    return 0;
}

static int job_all_stopped(struct job *j)
{
    int any = 0;
    for (int i = 0; i < j->nprocs; i++) {
        if (j->done[i] == 0)
            return 0;
        if (j->done[i] == 2)
            any = 1;
    }
    return any;
}

/* Record a status from waitpid; done[i]: 0 running, 1 exited, 2 stopped. */
static struct job *record(pid_t pid, int st)
{
    for (int k = 0; k < MAXJOBS; k++) {
        struct job *j = jobtab[k];
        if (!j)
            continue;
        for (int i = 0; i < j->nprocs; i++)
            if (j->pids[i] == pid) {
                if (WIFSTOPPED(st)) {
                    j->done[i] = 2;
                    j->status[i] = st;
                    if (job_all_stopped(j) && !j->stopped) {
                        j->stopped = 1;
                        j->notified = 0;
                    }
                } else {
                    j->done[i] = 1;
                    j->status[i] = st;
                    if (!job_live(j)) {
                        j->stopped = 0;
                        j->notified = 0;
                    }
                }
                return j;
            }
    }
    return 0;
}

static int status_code(int st)
{
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    if (WIFSTOPPED(st))
        return 128 + WSTOPSIG(st);
    return 128 + WTERMSIG(st);
}

/* The last process's status decides $? (and the job's state text). */
static int job_status(struct job *j)
{
    return j->nprocs ? j->status[j->nprocs - 1] : 0;
}

const char *job_state_text(struct job *j)
{
    static char buf[48];
    if (j->stopped) {
        for (int i = 0; i < j->nprocs; i++)
            if (j->done[i] == 2)
                return sigdesc(WSTOPSIG(j->status[i]));
        return "suspended";
    }
    if (job_live(j))
        return "running";
    int st = job_status(j);
    /* a signal anywhere in the pipeline is more interesting than an exit */
    for (int i = 0; i < j->nprocs; i++)
        if (WIFSIGNALED(j->status[i]) && WTERMSIG(j->status[i]) != SIGPIPE)
            st = j->status[i];
    if (WIFSIGNALED(st)) {
        snprintf(buf, sizeof(buf), "%s%s", sigdesc(WTERMSIG(st)),
                 WCOREDUMP(st) ? " (core dumped)" : "");
        return buf;
    }
    if (WEXITSTATUS(st)) {
        snprintf(buf, sizeof(buf), "exit %d", WEXITSTATUS(st));
        return buf;
    }
    return "done";
}

void job_print(struct job *j, int longfmt, int pids)
{
    int i = job_index(j);
    char mark = i == cur_job ? '+' : i == prev_job ? '-' : ' ';
    const char *state = job_state_text(j);
    int pad = 11 - (int)strlen(state);
    if (pad < 2)
        pad = 2;
    if (pids) {
        eprintf("%d\n", j->pgid);
        return;
    }
    if (longfmt) {
        eprintf("[%d]  %c %d %s%*s%s\n", j->id, mark, j->pids[0], state, pad, "", j->text);
        return;
    }
    eprintf("[%d]  %c %s%*s%s\n", j->id, mark, state, pad, "", j->text);
}

static int reap_one(int flags)
{
    int st;
    pid_t pid = waitpid(-1, &st, flags);
    if (pid <= 0)
        return 0;
    record(pid, st);
    return 1;
}

int jobs_reap(int report)
{
    while (reap_one(WNOHANG | WUNTRACED))
        ;
    got_sigchld = 0;
    if (!report)
        return 0;
    int n = 0;
    for (int i = 0; i < MAXJOBS; i++) {
        struct job *j = jobtab[i];
        if (!j || j->fg)
            continue;
        if (!job_live(j)) {
            if (sh.interactive && !j->disowned)
                job_print(j, 0, 0), n++;
            job_free(j);
        } else if (j->stopped && !j->notified) {
            j->notified = 1;
            if (sh.interactive)
                job_print(j, 0, 0), n++;
        }
    }
    return n;
}

int jobs_count(void)
{
    int n = 0;
    for (int i = 0; i < MAXJOBS; i++)
        if (jobtab[i])
            n++;
    return n;
}

int jobs_any_stopped(void)
{
    for (int i = 0; i < MAXJOBS; i++)
        if (jobtab[i] && jobtab[i]->stopped)
            return 1;
    return 0;
}

void jobs_hup_all(void)
{
    for (int i = 0; i < MAXJOBS; i++) {
        struct job *j = jobtab[i];
        if (!j || j->disowned)
            continue;
        kill(-j->pgid, SIGHUP);
        if (j->stopped)
            kill(-j->pgid, SIGCONT);
    }
}

void term_save_shell_modes(void)
{
    if (sh.tty_fd >= 0 && tcgetattr(sh.tty_fd, &shell_modes) == 0)
        have_shell_modes = 1;
}

void term_take(void)
{
    if (!opts[OPT_MONITOR] || sh.tty_fd < 0)
        return;
    tcsetpgrp(sh.tty_fd, sh.pgid);
    if (have_shell_modes)
        tcsetattr(sh.tty_fd, TCSADRAIN, &shell_modes);
}

int job_wait_fg(struct job *j)
{
    j->fg = 1;
    while (job_live(j) && !j->stopped) {
        int st;
        pid_t pid = waitpid(-1, &st, WUNTRACED);
        if (pid < 0) {
            if (errno == EINTR)
                continue;
            /* no children left: treat the rest as gone */
            for (int i = 0; i < j->nprocs; i++)
                if (!j->done[i])
                    j->done[i] = 1;
            break;
        }
        record(pid, st);
    }
    j->fg = 0;
    if (opts[OPT_MONITOR] && sh.tty_fd >= 0) {
        if (j->stopped && tcgetattr(sh.tty_fd, &j->tmodes) == 0)
            j->has_tmodes = 1;
        term_take();
    }

    /* $pipestatus */
    char *ps[MAXPROCS];
    char bufs[MAXPROCS][8];
    for (int i = 0; i < j->nprocs; i++) {
        snprintf(bufs[i], sizeof(bufs[i]), "%d", status_code(j->status[i]));
        ps[i] = bufs[i];
    }
    var_set_array("pipestatus", ps, j->nprocs, 0);

    int st = job_status(j);
    if (j->stopped) {
        make_current(job_index(j));
        j->notified = 1;
        if (sh.interactive)     /* (the terminal already echoed ^Z and a newline) */
            eprintf("%s: %s  %s\n", "esh", job_state_text(j), j->text);
        return 128 + SIGTSTP;
    }
    int code = status_code(st);
    if (sh.interactive) {
        int sig = 0, core = 0;
        for (int i = 0; i < j->nprocs; i++)
            if (WIFSIGNALED(j->status[i])) {
                sig = WTERMSIG(j->status[i]);
                core = WCOREDUMP(j->status[i]);
            }
        if (sig == SIGINT || sig == SIGQUIT)
            got_sigint = 1;     /* as if we were interrupted: stop the rest of the line */
        else if (sig && sig != SIGPIPE)
            eprintf("%s: %s%s  %s\n", "esh", sigdesc(sig), core ? " (core dumped)" : "", j->text);
    }
    job_free(j);
    return code;
}

void job_launch_done(struct job *j, int bg)
{
    if (!bg)
        return;
    make_current(job_index(j));
    sh.last_bg = j->pids[j->nprocs - 1];
    if (sh.interactive) {
        char buf[160];
        int n = snprintf(buf, sizeof(buf), "[%d]", j->id);
        for (int i = 0; i < j->nprocs && n < (int)sizeof(buf) - 12; i++)
            n += snprintf(buf + n, sizeof(buf) - n, " %d", j->pids[i]);
        eprintf("%s\n", buf);
    }
}

int job_continue(struct job *j, int fg)
{
    for (int i = 0; i < j->nprocs; i++)
        if (j->done[i] == 2)
            j->done[i] = 0;
    j->stopped = 0;
    j->notified = 0;
    if (fg) {
        if (opts[OPT_MONITOR] && sh.tty_fd >= 0) {
            if (j->has_tmodes)
                tcsetattr(sh.tty_fd, TCSADRAIN, &j->tmodes);
            tcsetpgrp(sh.tty_fd, j->pgid);
        }
        if (kill(-j->pgid, SIGCONT) < 0)
            kill(j->pgid, SIGCONT);
        return job_wait_fg(j);
    }
    make_current(job_index(j));
    if (kill(-j->pgid, SIGCONT) < 0)
        kill(j->pgid, SIGCONT);
    return 0;
}

/* %n %+ %% %- %string %?string, or a pid. NULL spec: the current job. */
struct job *job_find(const char *spec)
{
    if (!spec || !strcmp(spec, "%") || !strcmp(spec, "%%") || !strcmp(spec, "%+") || !*spec)
        return job_by_index(cur_job);
    if (!strcmp(spec, "%-"))
        return job_by_index(prev_job);
    if (spec[0] != '%') {
        if (!is_number(spec))
            return 0;
        int pid = atoi(spec);
        for (int i = 0; i < MAXJOBS; i++)
            if (jobtab[i])
                for (int k = 0; k < jobtab[i]->nprocs; k++)
                    if (jobtab[i]->pids[k] == pid)
                        return jobtab[i];
        return 0;
    }
    spec++;
    if (is_number(spec)) {
        int n = atoi(spec);
        return n >= 1 && n <= MAXJOBS ? jobtab[n - 1] : 0;
    }
    int contains = *spec == '?';
    if (contains)
        spec++;
    for (int i = MAXJOBS - 1; i >= 0; i--) {
        struct job *j = jobtab[i];
        if (!j)
            continue;
        if (contains ? strstr(j->text, spec) != 0 : !strncmp(j->text, spec, strlen(spec)))
            return j;
    }
    return 0;
}

/* Jobs whose state change has not been reported yet (for the line editor). */
int jobs_pending_report(void)
{
    int n = 0;
    for (int i = 0; i < MAXJOBS; i++) {
        struct job *j = jobtab[i];
        if (!j || j->fg || j->disowned)
            continue;
        if (!job_live(j) || (j->stopped && !j->notified))
            n++;
    }
    return n;
}
