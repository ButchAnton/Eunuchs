/* exitcode N [expected-args...]: exit with N; with extra args, also check argv. */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    /* exitcode fdopen N: 0 if descriptor N is open, 1 if not */
    if (argc == 3 && !strcmp(argv[1], "fdopen")) {
        struct stat st;
        return fstat(atoi(argv[2]), &st) == 0 ? 0 : 1;
    }
    /* exitcode env NAME VALUE: 0 if getenv(NAME) == VALUE */
    if (argc == 4 && !strcmp(argv[1], "env")) {
        const char *v = getenv(argv[2]);
        return v && !strcmp(v, argv[3]) ? 0 : 1;
    }
    if (argc < 2)
        return 100;
    if (argc > 2 && strcmp(argv[2], "check-args") == 0 && argc != 5)
        return 101;
    if (argc == 5 && (strcmp(argv[3], "two words") || strcmp(argv[4], "")))
        return 102;
    return atoi(argv[1]);
}
