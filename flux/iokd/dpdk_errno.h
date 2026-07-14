#ifndef _FLUX_IOKD_DPDK_ERRNO_H
#define _FLUX_IOKD_DPDK_ERRNO_H

#include <kernel/linux/errno.h>

#ifndef EBUSY
#define EBUSY FLUX_EBUSY
#endif

#ifndef EINVAL
#define EINVAL FLUX_EINVAL
#endif

#ifndef ENOENT
#define ENOENT FLUX_ENOENT
#endif

#ifndef EEXIST
#define EEXIST FLUX_EEXIST
#endif

#ifndef ENOBUFS
#define ENOBUFS FLUX_ENOBUFS
#endif

#ifndef ENOMEM
#define ENOMEM FLUX_ENOMEM
#endif

#ifndef ENOSYS
#define ENOSYS FLUX_ENOSYS
#endif

#ifndef ENOSPC
#define ENOSPC FLUX_ENOSPC
#endif

#ifndef EOPNOTSUPP
#define EOPNOTSUPP FLUX_EOPNOTSUPP
#endif

#ifndef ENOTSUP
#define ENOTSUP FLUX_EOPNOTSUPP
#endif

#ifndef EOVERFLOW
#define EOVERFLOW FLUX_EOVERFLOW
#endif

#endif /* _FLUX_IOKD_DPDK_ERRNO_H */
