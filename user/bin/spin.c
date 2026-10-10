/* spin [ms]: burn CPU in user mode (watch preemption with ps / trace). */
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    unsigned long end = uptime_ms() + (argc > 1 ? (unsigned long)atoi(argv[1]) : 2000);
    volatile unsigned long n = 0;
    while (uptime_ms() < end)
        n++;
    return n == 0;          /* use n so the loop is real work */
}
