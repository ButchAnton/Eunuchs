#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    printf("hello from pid %d (parent %d), %d args:", getpid(), getppid(), argc);
    for (int i = 0; i < argc; i++)
        printf(" [%s]", argv[i]);
    printf("\n");
    return 0;
}
