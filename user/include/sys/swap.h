#ifndef _SYS_SWAP_H
#define _SYS_SWAP_H

/* Swap space on a volume: "/dev/disk0p5", "disk0p5" or its GPT name ("swap").
   Root only. swapoff brings every swapped-out page back first (ENOMEM if
   they don't fit). */
int swapon(const char *volume, int flags);
int swapoff(const char *volume);

#endif
