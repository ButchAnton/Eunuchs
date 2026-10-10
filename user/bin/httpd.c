/*
 * httpd [-p port] [-r root]: a small HTTP/1.0 server (default port 80,
 * root /www; make run forwards localhost:8080 on the Mac to it).
 *   /            root/index.html
 *   /proc/...    live kernel state (any procfs file), as text
 *   /path        a file under root; a directory gets a listing
 * One child process per connection; one log line per request.
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char body[65536];

static int send_all(int c, const char *p, long n)
{
    while (n > 0) {
        long w = write(c, p, (size_t)n);
        if (w <= 0)
            return -1;
        p += w;
        n -= w;
    }
    return 0;
}

static void respond(int c, int code, const char *type, const char *data, long len)
{
    char hdr[256];
    const char *msg = code == 200 ? "OK" : code == 404 ? "Not Found" : code == 403 ? "Forbidden"
                                                                                  : "Bad Request";
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.0 %d %s\r\nServer: Eunuchs httpd\r\nContent-Type: %s\r\n"
                     "Content-Length: %ld\r\nConnection: close\r\n\r\n", code, msg, type, len);
    send_all(c, hdr, n);
    send_all(c, data, len);
}

static long slurp(const char *path, char *buf, long size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    long n = 0, r;
    while (n < size && (r = read(fd, buf + n, (size_t)(size - n))) > 0)
        n += r;
    close(fd);
    return n;
}

static const char *type_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot && (!strcmp(dot, ".html") || !strcmp(dot, ".htm")))
        return "text/html";
    if (dot && !strcmp(dot, ".css"))
        return "text/css";
    return "text/plain; charset=utf-8";
}

static int handle(int c, const char *root, const char *peer)
{
    char req[2048];
    int n = 0;
    while (n < (int)sizeof(req) - 1) {
        long r = read(c, req + n, sizeof(req) - 1 - (size_t)n);
        if (r <= 0)
            break;
        n += (int)r;
        req[n] = '\0';
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n"))
            break;
    }
    req[n] = '\0';
    char method[8] = "", path[256] = "";
    char *sp = strchr(req, ' ');
    if (!sp || sp - req >= (long)sizeof(method)) {
        respond(c, 400, "text/plain", "bad request\n", 12);
        return 400;
    }
    memcpy(method, req, (size_t)(sp - req));
    char *p = sp + 1, *end = p;
    while (*end && *end != ' ' && *end != '?' && *end != '\r' && *end != '\n')
        end++;
    if (end - p >= (long)sizeof(path))
        end = p + sizeof(path) - 1;
    memcpy(path, p, (size_t)(end - p));
    int code;
    long len = 0;
    if (strcmp(method, "GET") && strcmp(method, "HEAD")) {
        respond(c, 400, "text/plain", "only GET\n", 9);
        code = 400;
    } else if (strstr(path, "..")) {
        respond(c, 403, "text/plain", "forbidden\n", 10);
        code = 403;
    } else {
        char file[320];
        if (!strncmp(path, "/proc/", 6))
            snprintf(file, sizeof(file), "%s", path);
        else
            snprintf(file, sizeof(file), "%s%s", root, strcmp(path, "/") ? path : "/index.html");
        struct stat st;
        if (stat(file, &st) == 0 && S_ISDIR(st.st_mode)) {
            DIR *d = opendir(file);
            struct dirent *de;
            len = snprintf(body, sizeof(body), "<html><body><h1>%s</h1><ul>\n", path);
            while (d && (de = readdir(d)) != NULL && len < (long)sizeof(body) - 300)
                len += snprintf(body + len, sizeof(body) - (size_t)len,
                                "<li><a href=\"%s%s%s\">%s</a></li>\n", path,
                                path[strlen(path) - 1] == '/' ? "" : "/", de->d_name, de->d_name);
            if (d)
                closedir(d);
            len += snprintf(body + len, sizeof(body) - (size_t)len, "</ul></body></html>\n");
            respond(c, 200, "text/html", body, len);
            code = 200;
        } else if ((len = slurp(file, body, sizeof(body))) >= 0) {
            respond(c, 200, type_of(file), method[0] == 'H' ? "" : body, method[0] == 'H' ? 0 : len);
            code = 200;
        } else {
            len = snprintf(body, sizeof(body), "404: %s not found\n", path);
            respond(c, 404, "text/plain", body, len);
            code = 404;
        }
    }
    printf("httpd: %s %s %s -> %d, %ld bytes\n", peer, method, path, code, len);
    fflush_stdout();
    return code;
}

int main(int argc, char **argv)
{
    int port = 80;
    const char *root = "/www";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "-p"))
            port = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-r"))
            root = argv[i + 1];
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 16) < 0) {
        printf("httpd: port %d: %s\n", port, strerror(errno));
        return 1;
    }
    printf("httpd: serving %s on port %d\n", root, port);
    fflush_stdout();
    for (;;) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof(peer);
        int c = accept(s, (struct sockaddr *)&peer, &pl);
        while (waitpid(-1, 0, WNOHANG) > 0)
            ;
        if (c < 0) {
            if (errno != EINTR)
                printf("httpd: accept: %s\n", strerror(errno));
            continue;
        }
        pid_t pid = fork();
        if (pid == 0) {
            close(s);
            char who[32];
            snprintf(who, sizeof(who), "%s:%d", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
            handle(c, root, who);
            close(c);
            _exit(0);
        }
        close(c);
    }
}
