/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _ASM_X86_STAT_H
#define _ASM_X86_STAT_H

struct stat {
	unsigned long long st_dev;
	unsigned long long st_ino;
	unsigned long long st_nlink;

	unsigned int st_mode;
	unsigned int st_uid;
	unsigned int st_gid;
	unsigned int __pad0;
	unsigned long long st_rdev;
	long long st_size;
	long long st_blksize;
	long long st_blocks; /* Number 512-byte blocks allocated. */

	unsigned long long st_atime;
	unsigned long long st_atime_nsec;
	unsigned long long st_mtime;
	unsigned long long st_mtime_nsec;
	unsigned long long st_ctime;
	unsigned long long st_ctime_nsec;
	long long __unused[3];
};

/* We don't need to memset the whole thing just to initialize the padding */
#define INIT_STRUCT_STAT_PADDING(st) do {	\
	st.__pad0 = 0;				\
	st.__unused[0] = 0;			\
	st.__unused[1] = 0;			\
	st.__unused[2] = 0;			\
} while (0)

#endif /* _ASM_X86_STAT_H */