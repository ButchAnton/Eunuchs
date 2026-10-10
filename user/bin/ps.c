/* ps: processes, from /proc/<pid>/status. */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *field(char *buf, const char *key)
{
    static char val[64];
    size_t n = strlen(key);
    for (char *line = buf; line && *line;) {
        char *next = strchr(line, '\n');
        if (!strncmp(line, key, n) && line[n] == ' ') {
            size_t len = next ? (size_t)(next - line - n - 1) : strlen(line + n + 1);
            snprintf(val, sizeof(val), "%.*s", (int)len, line + n + 1);
            return val;
        }
        line = next ? next + 1 : 0;
    }
    return "?";
}

int main(void)
{
    DIR *d = opendir("/proc");
    struct dirent *de;
    printf("  PID  PPID STAT WAIT         CMD\n");
    while (d && (de = readdir(d))) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9')
            continue;
        char path[64], buf[512];
        snprintf(path, sizeof(path), "/proc/%s/status", de->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        long n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        buf[n > 0 ? n : 0] = 0;
        char pid[16], ppid[16], state[16], wait[32];
        snprintf(pid, sizeof(pid), "%s", field(buf, "pid"));
        snprintf(ppid, sizeof(ppid), "%s", field(buf, "ppid"));
        snprintf(state, sizeof(state), "%s", field(buf, "state"));
        snprintf(wait, sizeof(wait), "%s", field(buf, "waiting"));
        printf("%5s %5s %-4s %-12s %s\n", pid, ppid, state, wait, field(buf, "name"));
    }
    if (d)
        closedir(d);
    return 0;
}
