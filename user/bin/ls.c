/* ls [-l] [path...] */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int lflag;

static void show(const char *dir, const char *name)
{
    if (!lflag) {
        printf("%s\n", name);
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    struct stat st;
    if (stat(path, &st) < 0) {
        printf("?????????? %s\n", name);
        return;
    }
    char mode[11] = "----------";
    mode[0] = S_ISDIR(st.st_mode) ? 'd' : S_ISCHR(st.st_mode) ? 'c' : '-';
    for (int i = 0; i < 9; i++)
        if (st.st_mode & (0400 >> i))
            mode[1 + i] = "rwx"[i % 3];
    printf("%s %3u %8lu %6u  %s\n", mode, st.st_nlink, (unsigned long)st.st_size, st.st_ino, name);
}

static int list(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        eprintf("ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        show(".", path);
        return 0;
    }
    DIR *d = opendir(path);
    if (!d) {
        eprintf("ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    struct dirent *de;
    while ((de = readdir(d)))
        show(path, de->d_name);
    closedir(d);
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1, rc = 0;
    if (argc > 1 && !strcmp(argv[1], "-l")) {
        lflag = 1;
        i++;
    }
    if (i == argc)
        return list(".");
    int many = argc - i > 1;
    for (; i < argc; i++) {
        struct stat st;
        if (many && stat(argv[i], &st) == 0 && S_ISDIR(st.st_mode))
            printf("%s:\n", argv[i]);
        rc |= list(argv[i]);
    }
    return rc;
}
