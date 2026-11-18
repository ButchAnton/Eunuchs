#ifndef _SYS_MOUNT_H
#define _SYS_MOUNT_H

#include <uapi/statfs.h>                /* MS_*, MNT_* */

/* Root only. FSTYPE: efs (SOURCE: a volume, disk0p4 or its label), devfs,
   proc, 9p (SOURCE: the share's tag). FLAGS and DATA are ignored. */
int mount(const char *source, const char *target, const char *fstype, unsigned long flags,
          const void *data);
int umount(const char *target);
int umount2(const char *target, int flags);

#endif
