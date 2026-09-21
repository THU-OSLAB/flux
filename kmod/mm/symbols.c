/* Host symbol resolution. */

#define pr_fmt(fmt) "flux_mm: " fmt

#include "internal.h"

const struct vm_special_mapping *flux_vdso_mapping;
const struct vm_special_mapping *flux_vvar_mapping;
const struct vm_special_mapping *flux_vclock_mapping;
const struct vm_operations_struct *flux_vma_dummy_vm_ops;
flux_host_munmap_fn flux_do_munmap;
long (*flux_sys_execve)(struct pt_regs *);
struct mm_struct *(*flux_dup_mm)(struct task_struct *,
				 struct mm_struct *);
void (*flux_switch_mm_irqs_off)(struct mm_struct *, struct mm_struct *,
			 struct task_struct *);
void (*flux_flush_tlb_mm_range)(struct mm_struct *, unsigned long,
				 unsigned long, unsigned int, bool);
#ifdef CONFIG_SCHED_MM_CID
void (*flux_sched_mm_cid_before_execve)(struct task_struct *);
#endif
#ifdef CONFIG_MEMBARRIER
void (*flux_membarrier_update_current_mm)(struct mm_struct *);
#endif

static void *flux_resolve_dup_mm_symbol(void)
{
	static const char * const candidates[] = {
		"dup_mm",
		"dup_mm.constprop.0",
		"dup_mm.constprop.1",
		"dup_mm.constprop.2",
	};
	struct kprobe dup_mm_kp = {};
	void *dup_mm;
	int i;

	for (i = 0; i < ARRAY_SIZE(candidates); i++) {
		dup_mm = (void *)flux_lookup_symbol(candidates[i]);
		if (dup_mm)
			return dup_mm;

		dup_mm_kp.symbol_name = candidates[i];
		if (register_kprobe(&dup_mm_kp))
			continue;
		dup_mm = dup_mm_kp.addr;
		unregister_kprobe(&dup_mm_kp);
		if (dup_mm)
			return dup_mm;
	}

	pr_err("failed to resolve dup_mm symbol\n");
	return NULL;
}

int flux_mm_resolve_symbols(void)
{
	void **sys_call_table;

	sys_call_table = (void **)flux_lookup_symbol("sys_call_table");
	if (!sys_call_table) {
		pr_err("failed to resolve sys_call_table symbol\n");
		return -ENOENT;
	}
	flux_sys_execve = sys_call_table[__NR_execve];
	if (!flux_sys_execve) {
		pr_err("failed to resolve sys_execve symbol\n");
		return -ENOENT;
	}

	/* Optional: no cache when the host does not expose this helper. */
	flux_vvar_mapping = (void *)flux_lookup_symbol("vvar_mapping");
	if (!flux_vvar_mapping)
		flux_vvar_mapping = (void *)flux_lookup_symbol("vdso_vvar_mapping");
	flux_vclock_mapping = (void *)flux_lookup_symbol("vvar_vclock_mapping");
	flux_vma_dummy_vm_ops = (void *)flux_lookup_symbol("vma_dummy_vm_ops");
	flux_vdso_mapping = (void *)flux_lookup_symbol("vdso_mapping");
	flux_do_munmap = (void *)flux_lookup_symbol("do_munmap");
	flux_dup_mm = flux_resolve_dup_mm_symbol();
	if (!flux_dup_mm)
		return -ENOENT;

	flux_switch_mm_irqs_off =
		(void *)flux_lookup_symbol("switch_mm_irqs_off");
	if (!flux_switch_mm_irqs_off) {
		pr_err("failed to resolve switch_mm_irqs_off symbol\n");
		return -ENOENT;
	}

	flux_walk_page_vma = (void *)flux_lookup_symbol("walk_page_vma");
	flux_smaps_pte_range =
		(void *)flux_lookup_symbol("smaps_pte_range");
	if (flux_walk_page_vma && flux_smaps_pte_range) {
		flux_smaps_walk_ops.pmd_entry = flux_smaps_pte_range;
		pr_info("application-only smaps fast path enabled\n");
	} else {
		pr_warn("application-only smaps fast path unavailable\n");
	}

#ifdef CONFIG_SCHED_MM_CID
	flux_sched_mm_cid_before_execve =
		(void *)flux_lookup_symbol("sched_mm_cid_before_execve");
	if (!flux_sched_mm_cid_before_execve) {
		pr_err("failed to resolve scheduler mm CID symbols\n");
		return -ENOENT;
	}
#endif

#ifdef CONFIG_MEMBARRIER
	flux_membarrier_update_current_mm =
		(void *)flux_lookup_symbol("membarrier_update_current_mm");
	if (!flux_membarrier_update_current_mm) {
		pr_err("failed to resolve membarrier_update_current_mm symbol\n");
		return -ENOENT;
	}
#endif

	return 0;
}
