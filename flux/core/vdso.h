#ifndef _FLUX_CORE_VDSO_H
#define _FLUX_CORE_VDSO_H

int flux_vdso_init(void);

#ifdef CONFIG_FLUX_MPK
int flux_vdso_protect_shared(void);
#endif

void flux_vdso_fini(void);
void *flux_vdso_ehdr(void);
void *flux_vdso_data(void);

#endif /* _FLUX_CORE_VDSO_H */
