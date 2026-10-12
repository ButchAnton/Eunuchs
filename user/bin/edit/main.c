/* edit: choose the personality by the name we were run as. */
#include "edit.h"

#include <string.h>

int main(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "vi") || !strcmp(name, "vim") || !strcmp(name, "ex"))
        return vi_main(argc, argv, 0);
    if (!strcmp(name, "view"))
        return vi_main(argc, argv, 1);
    return emacs_main(argc, argv);
}
