/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_HOST_FS_H
#define _ASM_FLUX_HOST_FS_H

long flux_hostfs_get_host_fd(struct file *filp);
long flux_hostfs_get_host_fd_from_fd(int fd);

#endif /* _ASM_FLUX_HOST_FS_H */
