/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FLUX_KMOD_COMPAT_SYMBOLS_H
#define _FLUX_KMOD_COMPAT_SYMBOLS_H

int flux_compat_symbols_init(void);
unsigned long flux_lookup_symbol(const char *name);

#endif /* _FLUX_KMOD_COMPAT_SYMBOLS_H */
