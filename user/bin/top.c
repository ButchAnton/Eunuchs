/* top: refresh the thread table every second; q (or ^C) quits. */
#include <fcntl.h>
#include <stdio.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static void show(const char *path, int max_lines)
{
    char buf[4096];
    int fd = open(path, O_RDONLY);
    long n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : 0;
    if (fd >= 0)
        close(fd);
    buf[n > 0 ? n : 0] = 0;
    int lines = 0;
    for (char *p = buf; *p && lines < max_lines; p++) {
        if (*p == '\n') {
            write(1, "\r\n", 2);                 /* raw mode: no output translation */
            lines++;
        } else {
            write(1, p, 1);
        }
    }
}

int main(void)
{
    struct termios saved, raw;
    tcgetattr(0, &saved);
    raw = saved;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 0;                         /* non-blocking reads */
    tcsetattr(0, TCSANOW, &raw);
    for (;;) {
        printf("\033[H\033[2Jtop: q to quit    uptime ");
        fflush_stdout();
        show("/proc/uptime", 1);
        show("/proc/cpus", 10);             /* one line per CPU, then IPI totals */
        show("/proc/threads", 20);
        char c;
        int quit = 0;
        for (int i = 0; i < 10 && !quit; i++) {
            struct timespec ts = { 0, 100000000 };
            nanosleep(&ts, 0);
            if (read(0, &c, 1) == 1 && (c == 'q' || c == 3))
                quit = 1;
        }
        if (quit)
            break;
    }
    tcsetattr(0, TCSAFLUSH, &saved);    /* drop keys typed for us (e.g. Enter after q) */
    printf("\n");
    return 0;
}
