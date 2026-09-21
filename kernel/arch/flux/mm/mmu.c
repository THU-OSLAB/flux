// SPDX-License-Identifier: GPL-2.0
#include <linux/memory.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/pgtable.h>

#include <asm/fault_stats.h>
#include <asm/host_dev.h>
#include <asm/host_ops.h>
#include <asm/mmu.h>
#include <asm/page.h>
#include <asm/processor.h>
#include <asm/smp.h>
#include <asm/tlbflush.h>
#include <asm/xol.h>

pgd_t swapper_pg_dir[PTRS_PER_PGD];

static const pgprot_t protection_map[16] = {
	[VM_NONE] = PAGE_NONE,
	[VM_READ] = PAGE_READONLY,
	[VM_WRITE] = PAGE_COPY,
	[VM_WRITE | VM_READ] = PAGE_COPY,
	[VM_EXEC] = PAGE_READONLY_EXEC,
	[VM_EXEC | VM_READ] = PAGE_READONLY_EXEC,
	[VM_EXEC | VM_WRITE] = PAGE_COPY_EXEC,
	[VM_EXEC | VM_WRITE | VM_READ] = PAGE_COPY_EXEC,
	[VM_SHARED] = PAGE_NONE,
	[VM_SHARED | VM_READ] = PAGE_READONLY,
	[VM_SHARED | VM_WRITE] = PAGE_SHARED,
	[VM_SHARED | VM_WRITE | VM_READ] = PAGE_SHARED,
	[VM_SHARED | VM_EXEC] = PAGE_READONLY_EXEC,
	[VM_SHARED | VM_EXEC | VM_READ] = PAGE_READONLY_EXEC,
	[VM_SHARED | VM_EXEC | VM_WRITE] = PAGE_SHARED_EXEC,
	[VM_SHARED | VM_EXEC | VM_WRITE | VM_READ] = PAGE_SHARED_EXEC
};
pgprot_t vm_get_page_prot(unsigned long flags)
{
	pgprot_t prot = protection_map[flags &
		(VM_READ | VM_WRITE | VM_EXEC | VM_SHARED)];

	if (flux_xol_required(flags))
		prot = __pgprot(pgprot_val(prot) | _PAGE_XOL);
	return prot;
}
EXPORT_SYMBOL(vm_get_page_prot);

pgd_t *pgd_alloc(struct mm_struct *mm)
{
	/* Each LibOS mm owns its complete software page-table tree. */
	return (pgd_t *)get_zeroed_page(GFP_KERNEL);
}

/*
 * Application aliases project the owning Linux PTEs. Kernel vmalloc aliases
 * project init_mm into each host execution mm; the direct map is host-backed.
 */
static pte_t *flux_walk_user_pte(struct mm_struct *mm, unsigned long addr);

/*
 * TASK_SIZE includes Flux's kernel direct map and vmalloc window.  Neither is
 * SKAS user memory: the direct map is already present in every host mm with
 * pkey 0, while vmalloc aliases are handled explicitly through init_mm.
 */
bool flux_user_alias_addr_valid(unsigned long addr)
{
	return addr < TASK_SIZE && !virt_addr_valid((void *)addr) &&
	       (addr < VMALLOC_START || addr >= VMALLOC_END);
}

static bool flux_user_alias_get_shared(struct mm_struct *mm,
				       unsigned long addr, bool *shared)
{
	struct vm_area_struct *vma;

	if (!mm || addr >= TASK_SIZE)
		return false;
	vma = find_vma(mm, addr);
	if (!vma || addr < vma->vm_start)
		return false;

	*shared = vma->vm_flags & VM_SHARED;
	return true;
}

struct flux_user_alias_state {
	unsigned long addr;
	unsigned long src;
	unsigned int nr;
	int prot;
};

static int flux_unalias_user_range(struct mm_struct *mm,
				   unsigned long addr, unsigned int nr)
{
	int proc_key;

	if (!nr)
		return 0;

	proc_key = mm ? READ_ONCE(mm->context.proc_key) : 0;
	if (mm)
		return flux_host_dev_unalias_pages(proc_key, addr, nr);

	return -EINVAL;
}

static bool flux_read_user_alias_state(struct mm_struct *mm,
				       unsigned long addr,
				       struct flux_user_alias_state *state)
{
	pte_t *ptep;
	pte_t pte;

	addr &= PAGE_MASK;
	ptep = flux_walk_user_pte(mm, addr);
	if (!ptep)
		return false;

	pte = READ_ONCE(*ptep);
	if (!pte_present(pte) || !pte_read(pte))
		return false;

	state->addr = addr;
	state->nr = 1;
	state->prot = PROT_READ;
	if (pte_exec(pte) && !(pte_val(pte) & _PAGE_XOL))
		state->prot |= PROT_EXEC;
	if (pte_write(pte) && pte_dirty(pte))
		state->prot |= PROT_WRITE;
	if (pte_huge(pte)) {
		state->addr &= PMD_MASK;
		state->nr = PMD_SIZE / PAGE_SIZE;
	}
	state->src = (unsigned long)page_address(pte_page(pte));

	return state->src != 0;
}

/*
 * The Flux PTE is the authority. After installing a host alias, re-read the
 * PTE and correct the alias if another CPU changed the PFN or permission.
 */
static int flux_revalidate_user_alias(struct mm_struct *mm,
				      unsigned long expected_addr,
				      unsigned long expected_src,
				      unsigned int expected_nr,
				      int expected_prot, bool shared,
				      bool nowait)
{
	struct flux_user_alias_state state;
	unsigned int attempt;
	int ret;

	for (attempt = 0; attempt < 8; attempt++) {
		smp_rmb();
		if (!flux_read_user_alias_state(mm, expected_addr, &state))
			return flux_unalias_user_range(mm, expected_addr,
						       expected_nr);

		if (state.addr == expected_addr &&
		    state.src == expected_src &&
		    state.nr == expected_nr &&
		    state.prot == expected_prot)
			return 0;

		ret = flux_host_dev_alias_pages(state.addr, state.src, state.nr,
						state.prot, shared, nowait);
		if (ret)
			return ret;

		expected_addr = state.addr;
		expected_src = state.src;
		expected_nr = state.nr;
		expected_prot = state.prot;
	}

	return flux_unalias_user_range(mm, expected_addr, expected_nr);
}

static int flux_install_user_alias_stable(struct mm_struct *mm,
					  unsigned long addr,
					  unsigned long src,
					  unsigned int nr,
					  int prot, bool huge, bool nowait,
					  bool repeat_source)
{
	bool shared;
	unsigned int i;
	int ret;

	if (!flux_user_alias_get_shared(mm, addr, &shared))
		return -EAGAIN;

	if (repeat_source)
		ret = flux_host_dev_alias_repeated_pages(addr, src, nr, prot, shared);
	else
		ret = flux_host_dev_alias_pages(addr, src, nr, prot, shared, nowait);
	if (ret)
		return ret;

	if (huge)
		return flux_revalidate_user_alias(mm, addr, src, nr, prot,
						  shared, nowait);

	for (i = 0; i < nr; i++) {
		ret = flux_revalidate_user_alias(mm,
						 addr + (unsigned long)i * PAGE_SIZE,
						 src + (repeat_source ? 0 :
							(unsigned long)i * PAGE_SIZE),
						 1, prot, shared, nowait);
		if (ret)
			return ret;
	}

	return 0;
}

static int __flux_mmap_pages_for_ptes(struct mm_struct *mm, unsigned long va,
				      unsigned int nr, pte_t pte,
				      pte_t *expected_ptep)
{
	int prot = PROT_READ;
	/*
	 * A Flux task switch does not consume a suspended Linux signal frame on
	 * this CPU worker pthread.  Keep every alias operation nonblocking until
	 * that frame's libc restorer and rt_sigreturn have completed.
	 */
	bool nowait = this_cpu_read(tls_pcpu.host_signal_depth) != 0;

	if (pte_exec(pte) && !(pte_val(pte) & _PAGE_XOL))
		prot |= PROT_EXEC;
	if (pte_flags(pte) & _PAGE_RW)
		prot |= PROT_WRITE;

	/*
	 * Kernel vmalloc/module window: alias each VA to the Flux PTE's buddy
	 * page.  This keeps one backing page across host mms and lets a missing
	 * alias be reconstructed lazily by the synchronous-fault handler. Check
	 * this FIRST:
	 * VMALLOC_START..END lies BELOW TASK_SIZE, so the user test below would
	 * otherwise wrongly grab it.
	 */
	if (va >= VMALLOC_START && va < VMALLOC_END) {
		unsigned long src = (unsigned long)page_address(pte_page(pte));

		if (WARN_ON_ONCE(expected_ptep && nr != 1))
			return -EINVAL;
		if (!src)
			return -EFAULT;
		/*
		 * The kmod owns reservation plus installation under its alias
		 * lock. Do not precede it with an independent per-page mmap.
		 * set_ptes supplies consecutive PFNs, so retain its range here;
		 * checked fault repair keeps its single-PTE validation contract.
		 */
		if (expected_ptep)
			return flux_host_dev_alias_kernel_page_checked(
				va, src, prot, nowait,
				(unsigned long)expected_ptep, pte_val(pte),
				~(u64)0);
		return flux_host_dev_alias_pages(va, src, nr, prot, false, nowait);
	}

	/*
	 * skas Flux USER memory: alias the user VA to the SAME host page that
	 * backs the Flux buddy page (via its direct-map alias), so there is ONE
	 * physical backing shared with the Flux kernel view. Keeps mm features
	 * (numa/ksm) and host access consistent; a separate anon page diverges.
	 */
	/*
	 * Gate skas user aliasing on mm == current->mm: the alias goes into the
	 * ACTIVE host mm (current's). During fork, copy_pte_range() sets the
	 * CHILD mm's ptes while current is the PARENT -> aliasing here would
	 * corrupt the parent's host mm. The child is aliased lazily when it runs
	 * and faults (flux_ensure_alias, which passes current->mm).
	 */
	if (mm && flux_user_alias_addr_valid(va) &&
	    mm == current->mm) {
		if (!pte_read(pte))
			return flux_unalias_user_range(mm, va, nr);

		unsigned long src = (unsigned long)page_address(pte_page(pte));
		bool huge = pte_huge(pte);

		/*
		 * The host PTE cannot update the Flux PTE's hardware dirty bit.
		 * Keep a clean, writable Flux PTE read-only in the host so its
		 * first write faults through handle_mm_fault(), which marks the
		 * Flux PTE dirty before restoring the writable alias.  In
		 * particular, MADV_FREE depends on this transition to distinguish
		 * pages that userspace touched again from reclaimable lazy-free pages.
		 */
		if (!pte_dirty(pte))
			prot &= ~PROT_WRITE;

		/*
		 * A hugetlb entry lives at the PMD level and covers PMD_SIZE
		 * (2MB = 512 constituent 4K pages, physically contiguous). It is
		 * installed via set_huge_pte_at -> set_pte_at -> here with nr=1,
		 * so expand it to the full 512-page span; va is HPAGE-aligned.
		 */
		if (huge) {
			nr = PMD_SIZE / PAGE_SIZE;
			va &= PMD_MASK;
			src = (unsigned long)page_address(pte_page(pte));
		}

		if (src) {
			/*
			 * Let the kmod create any missing host reservation and install
			 * the alias while holding flux_alias_pages_lock. A separate raw
			 * host mmap here would expose an ordinary pkey-0 PROT_NONE VMA
			 * before the alias ioctl can acquire that lock, so a concurrent
			 * app-range validator could reject the safe transition.
			 */
			return flux_install_user_alias_stable(mm, va, src, nr,
							      prot, huge, nowait, false);
		}
		return -EFAULT;
	}

	return 0;
}

static int flux_mmap_pages_for_ptes(struct mm_struct *mm, unsigned long va,
				    unsigned int nr, pte_t pte)
{
	return __flux_mmap_pages_for_ptes(mm, va, nr, pte, NULL);
}

void mmap_pages_for_ptes(struct mm_struct *mm, unsigned long va,
			 unsigned int nr, pte_t pte)
{
	/*
	 * Setters cannot return alias errors. A missing projection faults and is
	 * rebuilt from the Linux PTE; revocation never depends on a PTE marker.
	 */
	flux_mmap_pages_for_ptes(mm, va, nr, pte);
}

/*
 * vmalloc owns these empty kernel PTE slots until this call returns. Unlike
 * generic set_ptes(), this interface may span separate folios: validate each
 * page, publish its own PTE, then submit a bounded linear source run.
 * No mapping is deferred past the existing vmap completion point.
 */
int flux_vmap_pte_range(pte_t *ptep, unsigned long addr, unsigned long end,
		       pgprot_t prot, struct page **pages, int *index)
{
	while (addr < end) {
		struct page *page = pages[*index];
		unsigned int run = 1, i;

		if (WARN_ON(!pte_none(ptep_get(ptep))))
			return -EBUSY;
		if (WARN_ON(!page))
			return -ENOMEM;
		if (WARN_ON(!pfn_valid(page_to_pfn(page))))
			return -EINVAL;

		while (run < 32 && run < ((end - addr) >> PAGE_SHIFT)) {
			struct page *next = pages[*index + run];

			/* Leave an invalid/conflicting slot for the next iteration,
			 * after publishing the same valid prefix as the scalar path.
			 */
			if (!next || !pfn_valid(page_to_pfn(next)) ||
			    page_to_pfn(next) != page_to_pfn(page) + run ||
			    !pte_none(ptep_get(ptep + run)))
				break;
			run++;
		}
		for (i = 0; i < run; i++)
			__set_pte(ptep + i, mk_pte(pages[*index + i], prot));
		mmap_pages_for_ptes(&init_mm, addr, run, mk_pte(page, prot));
		*index += run;
		ptep += run;
		addr += (unsigned long)run << PAGE_SHIFT;
	}
	return 0;
}

/*
 * Ensure the host alias exists for a faulting user address. handle_mm_fault
 * only calls mmap_pages_for_ptes() via set_ptes() when it installs a NEW pte.
 * A fork child inherits the host alias snapshot taken before Flux dup_mmap(),
 * but an alias installed in the parent after that snapshot can still be absent
 * while the child's Flux PTE is already PRESENT.  In that case a read fault
 * resolves in handle_mm_fault without touching set_ptes.  Walk the Flux PTE
 * here and install the missing alias explicitly.
 */
static pte_t *flux_walk_user_pte(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;

	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd))
		return NULL;
	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d))
		return NULL;
	pud = pud_offset(p4d, addr);
	if (pud_none(*pud))
		return NULL;
	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd) || !pmd_present(*pmd))
		return NULL;
	/* A hugetlb pmd is itself the leaf entry (no pte table below it). */
	if (pmd_leaf(*pmd))
		return (pte_t *)pmd;
	return pte_offset_kernel(pmd, addr);
}

/* Is the application pte for addr already present (before handle_mm_fault)? */
bool flux_user_pte_present(struct mm_struct *mm, unsigned long addr)
{
	pte_t *ptep;

	if (!(mm &&
	      flux_user_alias_addr_valid(addr)))
		return false;
	ptep = flux_walk_user_pte(mm, addr & PAGE_MASK);
	return ptep && pte_present(*ptep);
}

bool flux_user_pte_uffd_wp(struct mm_struct *mm, unsigned long addr)
{
#ifdef CONFIG_HAVE_ARCH_USERFAULTFD_WP
	pte_t *ptep;
	pte_t pte;

	if (!(mm &&
	      flux_user_alias_addr_valid(addr)))
		return false;
	ptep = flux_walk_user_pte(mm, addr & PAGE_MASK);
	if (!ptep)
		return false;
	pte = READ_ONCE(*ptep);
	return pte_present(pte) && pte_uffd_wp(pte);
#else
	return false;
#endif
}

/*
 * A fault can only repair permissions already granted by the Linux PTE.
 * Revalidate source and permissions after installation before retrying the
 * faulting instruction.
 */
static bool flux_alias_repair_result(bool nowait, enum flux_fault_event event)
{
	bool repaired = event == FLUX_FAULT_ATOMIC_REPAIRED;

	flux_fault_note(nowait ? event : repaired ?
			FLUX_FAULT_BLOCKING_REPAIRED : FLUX_FAULT_BLOCKING_FAILED);
	return repaired;
}

static bool flux_repair_present_user_alias(struct mm_struct *mm,
					   unsigned long addr, bool nowait,
					   unsigned long fault_err)
{
	struct flux_user_alias_state before, after;
	int ret;

	if (!mm || mm != current->mm ||
	    !flux_user_alias_addr_valid(addr) ||
	    !flux_read_user_alias_state(mm, addr, &before))
		return flux_alias_repair_result(nowait, FLUX_FAULT_ATOMIC_NO_STATE);
	if (((fault_err & 0x2) && !(before.prot & PROT_WRITE)) ||
	    ((fault_err & 0x10) && !(before.prot & PROT_EXEC)))
		return flux_alias_repair_result(nowait, FLUX_FAULT_ATOMIC_ACCESS_BLOCKED);
	ret = flux_install_user_alias_stable(mm, before.addr, before.src,
					    before.nr, before.prot, before.nr > 1,
					    nowait, false);
	if (ret)
		return flux_alias_repair_result(nowait, FLUX_FAULT_ATOMIC_INSTALL_FAILED);
	if (!flux_read_user_alias_state(mm, addr, &after) ||
	    before.addr != after.addr || before.src != after.src ||
	    before.nr != after.nr || before.prot != after.prot)
		return flux_alias_repair_result(nowait, FLUX_FAULT_ATOMIC_STATE_CHANGED);
	return flux_alias_repair_result(nowait, FLUX_FAULT_ATOMIC_REPAIRED);
}

bool flux_repair_present_user_alias_atomic(struct mm_struct *mm,
					   unsigned long addr, unsigned long err)
{
	return flux_repair_present_user_alias(mm, addr, true, err);
}

bool flux_repair_present_user_alias_blocking(struct mm_struct *mm,
					     unsigned long addr)
{
	return flux_repair_present_user_alias(mm, addr, false, 0);
}

void flux_ensure_alias(struct mm_struct *mm, unsigned long addr)
{
	pte_t *ptep;

	if (!(mm &&
	      flux_user_alias_addr_valid(addr)))
		return;
	addr &= PAGE_MASK;
	ptep = flux_walk_user_pte(mm, addr);
	if (ptep && pte_present(*ptep))
		flux_mmap_pages_for_ptes(mm, addr, 1, *ptep);
}

/*
 * Recreate a dynamic kernel-vmalloc alias in the active host mm.  Multiproc
 * host address spaces inherit boot-time kernel mappings, but a vmalloc made
 * later by a user task (for example a BPF program) initially exists only in
 * that task's host mm.  Kernel workers can subsequently run in proc0, so a
 * synchronous host fault must install the same Flux-PTE-backed page there.
 */
int flux_ensure_kernel_vmalloc_alias(unsigned long addr, bool write)
{
	pte_t *ptep;
	pte_t pte;

	if (addr < VMALLOC_START || addr >= VMALLOC_END)
		return -EINVAL;
	addr &= PAGE_MASK;
	ptep = flux_walk_user_pte(&init_mm, addr);
	if (!ptep)
		return -ENOENT;
	pte = READ_ONCE(*ptep);
	if (!pte_present(pte))
		return -ENOENT;
	/* Do not turn a genuine Flux read-only mapping into a writable alias. */
	if (write && !(pte_flags(pte) & _PAGE_RW))
		return -EACCES;

	return __flux_mmap_pages_for_ptes(&init_mm, addr, 1, pte, ptep);
}

void munmap_page_for_pte(unsigned long addr, pte_t *ptep)
{
	if (addr >= VMALLOC_START && addr < VMALLOC_END)
		BUG_ON(flux_host_dev_unalias_kernel_pages(addr & PAGE_MASK, 1));
}

/*
 * Retire bounded Flux-PTE groups before revoking every host-mm projection.
 * vmalloc retains backing pages until this call returns. The host operation
 * completes its TLB invalidation before releasing the corresponding pins.
 */
void flux_vunmap_pte_range(pte_t *ptep, unsigned long addr, unsigned long end)
{
	while (addr < end) {
		unsigned int nr = min_t(unsigned long, 32,
					    (end - addr) >> PAGE_SHIFT);
		unsigned long first = 0, last = 0;
		unsigned int i;

		for (i = 0; i < nr; i++) {
			unsigned long va = addr + (unsigned long)i * PAGE_SIZE;
			pte_t old = flux_pte_clear_raw(ptep + i);

			WARN_ON(!pte_none(old) && !pte_present(old));
			if (pte_present(old) &&
			    va >= VMALLOC_START && va < VMALLOC_END) {
				if (!last)
					first = va;
				last = va + PAGE_SIZE;
			}
		}
		if (last)
			BUG_ON(flux_host_dev_unalias_kernel_pages(first,
						(last - first) >> PAGE_SHIFT));
		ptep += nr;
		addr += (unsigned long)nr << PAGE_SHIFT;
	}
}

void flush_tlb_all(void)
{
}

/*
 * A Linux TLB flush revokes projections, including holes left by removed PTE
 * tables. Rebuilding belongs to the fault path; never infer invalidation from
 * a second snapshot of the already modified page tables.
 */
void flush_tlb_range(struct vm_area_struct *vma, unsigned long start,
		     unsigned long end)
{
	flux_flush_tlb_gather(vma->vm_mm, start & PAGE_MASK, PAGE_ALIGN(end));
}

void flush_tlb_mm(struct mm_struct *mm)
{
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, 0);

	if (!mm)
		return;
	/*
	 * arch_exit_mmap() revokes each VMA before page release. An exiting mm
	 * must not be walked again; live inactive mms still need revocation.
	 */
	if (!atomic_read(&mm->mm_users))
		return;
#ifdef CONFIG_FLUX_HOST_FAULT_REPAIR
	/* A full flush must fence host walkers even after the last VMA vanished. */
	if (mm->context.proc_key > 0) {
		int ret = flux_host_dev_flush_user_mm(mm->context.proc_key);

		if (!ret)
			return;
		/* Old modules cannot bind roots and retain the legacy flush path. */
		BUG_ON(ret != -ENOTTY && ret != -EINVAL);
	}
#endif
	for_each_vma(vmi, vma)
		flux_flush_tlb_gather(mm, vma->vm_start, vma->vm_end);
}

void flush_tlb_page(struct vm_area_struct *vma, unsigned long address)
{
	if (vma)
		flush_tlb_range(vma, address, address + PAGE_SIZE);
}

void flush_tlb_kernel_vm(void)
{
}

void flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
}

void __flush_tlb_one(unsigned long addr)
{
}

void flux_flush_tlb_gather(struct mm_struct *mm, unsigned long start,
			   unsigned long end)
{
	if (!mm || mm->context.proc_key <= 0 || start >= end)
		return;
	end = min_t(unsigned long, end, TASK_SIZE);
	while (start < end) {
		unsigned long pages = min_t(unsigned long,
					    (end - start) >> PAGE_SHIFT, 1UL << 20);
		int ret;

		if (!pages)
			break;
		ret = flux_host_dev_unalias_pages(mm->context.proc_key, start, pages);
		/* A failed revoke cannot acknowledge reuse of the physical pages. */
		if (WARN_ON_ONCE(ret))
			BUG();
		start += pages << PAGE_SHIFT;
	}
}
