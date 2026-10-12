#ifndef _SYS_REBOOT_H
#define _SYS_REBOOT_H

#include <uapi/reboot.h>

/* Restart (RB_AUTOBOOT) or power off (RB_POWER_OFF) the machine. Root only. */
int reboot(int how);

#endif
