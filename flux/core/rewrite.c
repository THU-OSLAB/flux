#define FLUX_FMT "rewrite: "

#define _GNU_SOURCE
#include <cpuid.h>
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <ucontext.h>

#ifndef PACKAGE
#define PACKAGE "flux"
#endif
#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "1"
#endif
#include <dis-asm.h>

#include <flux.h>
#include <flux/mpk.h>
#include <flux/rewrite.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define FLUX_REWRITE_MIN_PAGE_SIZE 4096
#define FLUX_REWRITE_SYSCALL_SLOTS 512
#define FLUX_REWRITE_SIGRETURN_NOSTACK 0x220
#define FLUX_REWRITE_MAX_INSN_SIZE 15
#define FLUX_CPUID_7_1_APX_F (1U << 21)

/*
 * The syscall-to-call rewrite and RAX-indexed zero-page slide follow zpoline:
 * https://github.com/yasukata/zpoline/tree/56aec879727af984ed7ebc28067f4f9c7cb60bdf
 * (Apache-2.0). Flux replaces zpoline's hook with its native Linux-ABI entry
 * and enters Flux through its Linux syscall-ABI gate.  The high return-address
 * bit tells the restart path that the rewritten instruction is two bytes long.
 * The trampoline does not clobber any registers.
 */
static const unsigned char flux_zero_trampoline[] = {
	0x48, 0x0f, 0xba, 0x2c, 0x24, 0x3f, /* btsq $63, (%rsp) */
	0xff, 0x24, 0x25, 0x10, 0x00, 0x10, 0x00, /* jmp *0x100010 */
};

/*
 * A signal restorer executes after RET has advanced RSP past pretcode.  It
 * cannot use the normal CALL rewrite when the signal frame is read-only,
 * because CALL would first write the synthetic return address at RSP-8.
 * Restore __NR_rt_sigreturn here and enter the gate that reconstructs that
 * logical stack address without touching application memory.
 */
static const unsigned char flux_zero_sigreturn_nostack[] = {
	0xb8, 0x0f, 0x00, 0x00, 0x00, /* mov $__NR_rt_sigreturn, %eax */
	0xff, 0x24, 0x25, 0x18, 0x00, 0x10, 0x00, /* jmp *0x100018 */
};

_Static_assert(FLUX_REWRITE_SYSCALL_SLOTS + sizeof(flux_zero_trampoline) <=
		       FLUX_REWRITE_MIN_PAGE_SIZE,
	       "zero-page trampoline must fit in one page");
_Static_assert(FLUX_REWRITE_SIGRETURN_NOSTACK >=
		       FLUX_REWRITE_SYSCALL_SLOTS + sizeof(flux_zero_trampoline) &&
		       FLUX_REWRITE_SIGRETURN_NOSTACK +
			       sizeof(flux_zero_sigreturn_nostack) <=
		       FLUX_REWRITE_MIN_PAGE_SIZE,
	       "zero-page sigreturn trampoline must not overlap");

struct flux_rewrite_site {
	unsigned char *addr;
	unsigned char length;
	unsigned char kind;
	unsigned char original[FLUX_REWRITE_MAX_INSN_SIZE];
	void *trampoline;
};

/*
 * This header is created before the multiprocess base-map snapshot, so every
 * fork inherits it with private COW state. Exec copies the empty runtime
 * template; discarding that host mm also discards its rewrite records.
 */
struct flux_rewrite_state {
	struct flux_rewrite_site *sites;
	size_t count;
	size_t capacity;
	size_t mapping_size;
};

static struct flux_rewrite_state *flux_rewrite_state;
static size_t flux_rewrite_page_size;

#define flux_rewrite_count (flux_rewrite_state->count)

enum flux_rewrite_patch_kind {
	FLUX_REWRITE_SYSCALL,
	FLUX_REWRITE_REDZONE,
	FLUX_REWRITE_MPK_WRPKRU,
	FLUX_REWRITE_MPK_XRSTOR,
	FLUX_REWRITE_MPK_MOV_IMM32,
	FLUX_REWRITE_MPK_CMP64_RIP,
};

struct flux_rewrite_patch {
	size_t offset;
	unsigned char length;
	unsigned char replacement[FLUX_REWRITE_MAX_INSN_SIZE];
	enum flux_rewrite_patch_kind kind;
};

struct flux_rewrite_plan {
	struct flux_rewrite_patch *patches;
	size_t count;
	size_t capacity;
	size_t mapping_size;
};

static void flux_rewrite_low_memset(uintptr_t dst, unsigned char value,
				    size_t len)
{
	__asm__ __volatile__("rep stosb"
			     : "+D"(dst), "+c"(len)
			     : "a"(value)
			     : "memory");
}

static void flux_rewrite_low_memcpy(uintptr_t dst, const void *src, size_t len)
{
	__asm__ __volatile__("rep movsb"
			     : "+D"(dst), "+S"(src), "+c"(len)
			     :
			     : "memory");
}

static int flux_rewrite_reject(size_t *offset, size_t bad_offset)
{
	if (offset)
		*offset = bad_offset;
	return -FLUX_ENOEXEC;
}

static bool flux_cpu_has_apx_f(void)
{
	unsigned int eax;
	unsigned int ebx;
	unsigned int ecx;
	unsigned int edx;

	return __get_cpuid_count(7, 1, &eax, &ebx, &ecx, &edx) &&
	       (edx & FLUX_CPUID_7_1_APX_F);
}

static bool flux_rewrite_crosses_page(const unsigned char *addr, size_t len)
{
	size_t page_offset = (uintptr_t)addr & (FLUX_REWRITE_MIN_PAGE_SIZE - 1);

	return len > FLUX_REWRITE_MIN_PAGE_SIZE - page_offset;
}

static int flux_rewrite_reserve_patches(struct flux_rewrite_plan *plan,
					size_t needed)
{
	void *new_patches;
	size_t needed_size;
	size_t new_size;

	if (needed <= plan->capacity)
		return 0;
	if (!flux_rewrite_page_size ||
	    needed > SIZE_MAX / sizeof(*plan->patches))
		return -FLUX_ENOMEM;
	needed_size = needed * sizeof(*plan->patches);
	if (needed_size > SIZE_MAX - (flux_rewrite_page_size - 1))
		return -FLUX_ENOMEM;
	new_size = (needed_size + flux_rewrite_page_size - 1) &
		   ~(flux_rewrite_page_size - 1);

	if (!plan->patches) {
		new_patches = mmap(NULL, new_size, PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	} else {
		new_patches = mremap(plan->patches, plan->mapping_size,
				     new_size, MREMAP_MAYMOVE);
	}
	if (new_patches == MAP_FAILED)
		return -FLUX_ENOMEM;
	plan->patches = new_patches;
	plan->mapping_size = new_size;
	plan->capacity = new_size / sizeof(*plan->patches);
	return 0;
}

static int flux_rewrite_push_patch(struct flux_rewrite_plan *plan,
				   size_t offset, size_t length,
				   const void *replacement,
				   enum flux_rewrite_patch_kind kind)
{
	int ret = flux_rewrite_reserve_patches(plan, plan->count + 1);
	struct flux_rewrite_patch *patch;

	if (ret < 0)
		return ret;
	patch = &plan->patches[plan->count++];
	patch->offset = offset;
	patch->length = length;
	patch->kind = kind;
	memcpy(patch->replacement, replacement, length);
	return 0;
}

static void flux_rewrite_plan_destroy(struct flux_rewrite_plan *plan)
{
	if (plan->patches)
		munmap(plan->patches, plan->mapping_size);
}

#define FLUX_REWRITE_CACHE_SLOTS 8
#define FLUX_REWRITE_CACHE_CODE_SIZE (2UL * 1024 * 1024)
#define FLUX_REWRITE_CACHE_PATCHES 1024

struct flux_rewrite_cache_entry {
	bool valid;
	uint64_t hash;
	uint64_t generation;
	size_t len;
	size_t patch_count;
	unsigned char code[FLUX_REWRITE_CACHE_CODE_SIZE];
	struct flux_rewrite_patch patches[FLUX_REWRITE_CACHE_PATCHES];
};

struct flux_rewrite_cache {
	uint64_t generation;
	struct flux_rewrite_cache_entry entries[FLUX_REWRITE_CACHE_SLOTS];
};

static struct flux_rewrite_cache *flux_rewrite_cache;

/*
 * This MAP_SHARED cache is created before the multiprocess base-map snapshot,
 * so dup_mm() descendants see plans inserted by sibling address spaces. The
 * Flux kernel's decoder mutex serializes lookup, insertion, and application.
 * The hash is only a screening key: the complete original byte range must
 * match before an offset-only plan can be reused.
 */
static uint64_t flux_rewrite_hash(const unsigned char *code, size_t len)
{
	uint64_t hash = UINT64_C(1469598103934665603);
	size_t i;

	for (i = 0; i < len; i++) {
		hash ^= code[i];
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static void flux_rewrite_cache_plan(
	struct flux_rewrite_cache_entry *entry,
	struct flux_rewrite_plan *plan)
{
	plan->patches = entry->patches;
	plan->count = entry->patch_count;
	plan->capacity = entry->patch_count;
	plan->mapping_size = 0;
}

static bool flux_rewrite_cache_lookup(const unsigned char *code, size_t len,
				      uint64_t hash,
				      struct flux_rewrite_plan *plan)
{
	size_t i;

	if (!flux_rewrite_cache || len > FLUX_REWRITE_CACHE_CODE_SIZE)
		return false;
	for (i = 0; i < FLUX_REWRITE_CACHE_SLOTS; i++) {
		struct flux_rewrite_cache_entry *entry =
			&flux_rewrite_cache->entries[i];

		if (!entry->valid ||
		    entry->hash != hash || entry->len != len ||
		    memcmp(entry->code, code, len))
			continue;
		entry->generation = ++flux_rewrite_cache->generation;
		flux_rewrite_cache_plan(entry, plan);
		return true;
	}
	return false;
}

static bool flux_rewrite_cache_store(const unsigned char *code, size_t len,
				     uint64_t hash,
				     const struct flux_rewrite_plan *plan,
				     struct flux_rewrite_plan *cached_plan)
{
	struct flux_rewrite_cache_entry *victim;
	size_t i;

	if (!flux_rewrite_cache || len > FLUX_REWRITE_CACHE_CODE_SIZE ||
	    plan->count > FLUX_REWRITE_CACHE_PATCHES)
		return false;
	victim = &flux_rewrite_cache->entries[0];
	for (i = 0; i < FLUX_REWRITE_CACHE_SLOTS; i++) {
		struct flux_rewrite_cache_entry *entry =
			&flux_rewrite_cache->entries[i];

		if (!entry->valid) {
			victim = entry;
			break;
		}
		if (entry->generation < victim->generation)
			victim = entry;
	}

	victim->valid = false;
	memcpy(victim->code, code, len);
	if (plan->count)
		memcpy(victim->patches, plan->patches,
		       plan->count * sizeof(*plan->patches));
	victim->hash = hash;
	victim->len = len;
	victim->patch_count = plan->count;
	victim->generation = ++flux_rewrite_cache->generation;
	victim->valid = true;
	flux_rewrite_cache_plan(victim, cached_plan);
	return true;
}

static void flux_rewrite_cache_init(void)
{
	struct flux_rewrite_cache *cache;
	int saved_errno;

	cache = mmap(NULL, sizeof(*cache), PROT_READ | PROT_WRITE,
		     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (cache == MAP_FAILED)
		goto out_warn;
	if (flux_mpk_protect_kernel(cache, sizeof(*cache),
				    PROT_READ | PROT_WRITE) < 0) {
		saved_errno = errno;
		munmap(cache, sizeof(*cache));
		errno = saved_errno;
		goto out_warn;
	}
	flux_rewrite_cache = cache;
	return;

out_warn:
	FLUX_LOG(FLUX_LOG_WARN, "shared rewrite cache disabled: %s\n",
		 strerror(errno));
}

#ifdef CONFIG_FLUX_MPK
static int flux_disassembly_null_printf(void *data __attribute__((unused)),
					const char *fmt,
					...)
{
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = vsnprintf(NULL, 0, fmt, args);
	va_end(args);
	return ret;
}

static int flux_disassembly_null_styled_printf(
	void *data __attribute__((unused)),
	enum disassembler_style style __attribute__((unused)),
	const char *fmt, ...)
{
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = vsnprintf(NULL, 0, fmt, args);
	va_end(args);
	return ret;
}

static bool flux_rewrite_is_wrpkru(const unsigned char *p)
{
	return p[0] == 0x0f && p[1] == 0x01 && p[2] == 0xef;
}

static bool flux_rewrite_is_xrstor(const unsigned char *p)
{
	return p[0] == 0x0f && p[1] == 0xae && (p[2] & 0xc0) != 0xc0 &&
	       (p[2] & 0x38) == 0x28;
}

static bool flux_rewrite_is_xrstors(const unsigned char *p)
{
	return p[0] == 0x0f && p[1] == 0xc7 && (p[2] & 0xc0) != 0xc0 &&
	       (p[2] & 0x38) == 0x18;
}

/*
 * This checked-switch recognition is adapted from isBenignWRPKRU() in ERIM:
 * https://github.com/vahldiek/erim/tree/7232f4762c5ff51035116f9664ec6e7af3b236af/src/erim
 * (CC BY 4.0). Flux narrows the accepted form to a switch that writes exactly
 * FLUX_MPK_APP_PKRU and verifies the value before continuing.
 */
static bool flux_rewrite_is_checked_app_switch(const unsigned char *code,
					       size_t len, size_t wrpkru_offset)
{
	uint32_t mov_pkru, cmp_pkru;
	size_t start;

	if (wrpkru_offset < 9)
		return false;
	start = wrpkru_offset - 9;
	if (len - start < 19)
		return false;

	if (code[start] != 0x31 || code[start + 1] != 0xc9 ||
	    code[start + 2] != 0x31 || code[start + 3] != 0xd2 ||
	    code[start + 4] != 0xb8 ||
	    !flux_rewrite_is_wrpkru(code + start + 9) ||
	    code[start + 12] != 0x3d)
		return false;

	memcpy(&mov_pkru, code + start + 5, sizeof(mov_pkru));
	memcpy(&cmp_pkru, code + start + 13, sizeof(cmp_pkru));
	if (mov_pkru != FLUX_MPK_APP_PKRU || cmp_pkru != mov_pkru)
		return false;

	/* jne rel8, with the target equal to the first xor. */
	if (code[start + 17] == 0x75 && (int8_t)code[start + 18] == -19)
		return true;

	/* jne rel32, with the target equal to the first xor. */
	if (len - start >= 23 && code[start + 17] == 0x0f &&
	    code[start + 18] == 0x85) {
		int32_t displacement;

		memcpy(&displacement, code + start + 19, sizeof(displacement));
		return displacement == -23;
	}

	return false;
}

static bool flux_mpk_is_checked_instruction(const unsigned char *code,
					    size_t len, size_t target)
{
	disassembler_ftype disassemble;
	disassemble_info info = { 0 };
	size_t pos = 0;
	bool checked = false;

	if (target > len || len - target < 3)
		return false;

	init_disassemble_info(&info, NULL, flux_disassembly_null_printf,
			      flux_disassembly_null_styled_printf);
	info.arch = bfd_arch_i386;
	info.mach = bfd_mach_x86_64;
	info.buffer = (unsigned char *)code;
	info.buffer_length = len;
	disassemble_init_for_target(&info);
	disassemble = disassembler(bfd_arch_i386, false, bfd_mach_x86_64, NULL);
	if (!disassemble)
		goto out_free;

	while (pos <= target) {
		int ret = disassemble(pos, &info);

		if (ret <= 0 || (size_t)ret > len - pos ||
		    (size_t)ret > FLUX_REWRITE_MAX_INSN_SIZE)
			goto out_free;
		if (pos == target) {
			checked = ret == 3 &&
				  flux_rewrite_is_wrpkru(code + pos) &&
				  flux_rewrite_is_checked_app_switch(code, len,
							     pos);
			goto out_free;
		}
		pos += (size_t)ret;
	}

out_free:
	disassemble_free_target(&info);
	return checked;
}

static bool flux_rewrite_plan_overlaps(const struct flux_rewrite_plan *plan,
				       size_t offset, size_t len)
{
	size_t i;

	for (i = 0; i < plan->count; i++) {
		const struct flux_rewrite_patch *patch = &plan->patches[i];

		if (offset < patch->offset + patch->length &&
		    patch->offset < offset + len)
			return true;
	}
	return false;
}

static int flux_mpk_push_trap(const unsigned char *code, size_t len,
			      size_t offset, size_t insn_len,
			      enum flux_rewrite_patch_kind kind,
			      struct flux_rewrite_plan *plan,
			      size_t *bad_offset)
{
	unsigned char replacement[FLUX_REWRITE_MAX_INSN_SIZE];
	size_t i;

	if (!insn_len || insn_len > sizeof(replacement) || offset > len ||
	    insn_len > len - offset ||
	    flux_rewrite_crosses_page(code + offset, insn_len) ||
	    flux_rewrite_plan_overlaps(plan, offset, insn_len))
		return flux_rewrite_reject(bad_offset, offset);

	replacement[0] = 0x0f;
	replacement[1] = 0x0b;
	for (i = 2; i < insn_len; i++)
		replacement[i] = 0x90;
	return flux_rewrite_push_patch(plan, offset, insn_len, replacement,
				       kind);
}

static bool flux_mpk_xrstor_is_rip_relative(const unsigned char *code,
					    size_t len)
{
	size_t pos = 0;
	bool address_size = false;
	bool rex_b = false;
	unsigned char modrm;

	/* This parser only answers the relocation question. Boundaries and
	 * instruction validity still come from libopcodes above. */
	while (pos < len) {
		unsigned char byte = code[pos];

		if (byte == 0x67) {
			address_size = true;
			pos++;
			continue;
		}
		if (byte >= 0x40 && byte <= 0x4f) {
			rex_b = (byte & 1) != 0;
			pos++;
			continue;
		}
		if (byte == 0x66 || byte == 0xf2 || byte == 0xf3 ||
		    byte == 0x64 || byte == 0x65 || byte == 0x2e ||
		    byte == 0x36 || byte == 0x3e || byte == 0x26 ||
		    byte == 0xf0) {
			pos++;
			continue;
		}
		break;
	}
	if (address_size || rex_b || pos + 3 > len || code[pos] != 0x0f ||
	    code[pos + 1] != 0xae)
		return false;
	modrm = code[pos + 2];
	return (modrm >> 6) == 0 && (modrm & 7) == 5;
}

/*
 * ERIM rewrites accidental patterns in operands. Flux keeps that operation
 * conservative: real three-byte instructions become residual sites, while
 * embedded patterns need an instruction-specific equivalent rewrite.
 * MOV r32, imm32 is completed in the saved register frame. Cerberus uses a
 * monitor/emulator for those residual sites; Flux's trampoline below handles
 * the safe XRSTOR subset without adding a hot-path check.
 */
static bool flux_mpk_is_cmp64_rip(const unsigned char *code, size_t len)
{
	/* REX.W CMP r64, [RIP + disp32]; REX.X/B have no addressing role. */
	return len == 7 && (code[0] & 0xf8) == 0x48 && code[1] == 0x3b &&
	       (code[2] & 0xc7) == 0x05;
}

static int flux_mpk_analyze_instruction(const unsigned char *code, size_t len,
					size_t pos, size_t insn_len,
					struct flux_rewrite_plan *plan,
					size_t *offset)
{
	size_t i;
	bool embedded = false;

	/* Garmr and Cerberus use best-effort rewriting and defer residual sites
	 * to an exceptional monitor. Keep that monitor boundary at decoded
	 * instructions: only explicitly supported operand forms are emulated;
	 * other operands are rejected without altering the instruction stream. */
	for (i = pos; i + 3 <= pos + insn_len; i++) {
		if (flux_rewrite_is_xrstors(code + i))
			continue;
		if (flux_rewrite_is_wrpkru(code + i) && i != pos)
			embedded = true;
		if (flux_rewrite_is_xrstor(code + i) && i != pos)
			embedded = true;
		if (embedded && !(insn_len == 5 && code[pos] >= 0xb8 &&
				  code[pos] <= 0xbf) &&
		    !flux_mpk_is_cmp64_rip(code + pos, insn_len))
			return flux_rewrite_reject(offset, i);
	}
	/* A raw gadget that straddles this instruction's end cannot be patched
	 * without changing either instruction. Keep the fail-closed behavior used
	 * by ERIM for such ambiguous byte streams. */
	for (i = pos; i < pos + insn_len && i + 3 <= len; i++) {
		if (i + 3 <= pos + insn_len)
			continue;
		if (flux_rewrite_is_xrstors(code + i))
			continue;
		if (flux_rewrite_is_wrpkru(code + i) ||
		    flux_rewrite_is_xrstor(code + i))
			return flux_rewrite_reject(offset, i);
	}
	/* MOV r32, imm32 can be completed directly in the saved register frame.
	 * Replace the entire decoded instruction, including the operand bytes,
	 * and retain the original only as non-executable rewrite metadata. */
	if (embedded && flux_mpk_is_cmp64_rip(code + pos, insn_len))
		return flux_mpk_push_trap(code, len, pos, insn_len,
					 FLUX_REWRITE_MPK_CMP64_RIP, plan, offset);
	if (embedded)
		return flux_mpk_push_trap(code, len, pos, insn_len,
					 FLUX_REWRITE_MPK_MOV_IMM32, plan, offset);
	if (flux_rewrite_is_wrpkru(code + pos)) {
		if (insn_len != 3)
			return flux_rewrite_reject(offset, pos);
		if (!flux_mpk_is_checked_instruction(code, len, pos) &&
		    flux_mpk_push_trap(code, len, pos, insn_len,
				       FLUX_REWRITE_MPK_WRPKRU, plan, offset) < 0)
			return -FLUX_ENOEXEC;
	} else if (flux_rewrite_is_xrstor(code + pos)) {
		if (flux_mpk_xrstor_is_rip_relative(code + pos, insn_len) ||
		    flux_mpk_push_trap(code, len, pos, insn_len,
				       FLUX_REWRITE_MPK_XRSTOR, plan, offset) < 0)
			return -FLUX_ENOEXEC;
	}

	return 0;
}

int flux_mpk_scan_exec(const void *addr, size_t len, size_t *offset)
{
	const unsigned char *code = addr;
	size_t i;

	if (!len)
		return 0;
	if (!code)
		return flux_rewrite_reject(offset, 0);
	if (len < 3)
		return 0;

	for (i = 0; i <= len - 3; i++) {
		if (flux_rewrite_is_wrpkru(code + i) &&
		    !flux_mpk_is_checked_instruction(code, len, i))
			return flux_rewrite_reject(offset, i);
		/* XRSTORS is privileged at CPL3 and is not a user MPK gadget. */
		if (flux_rewrite_is_xrstor(code + i))
			return flux_rewrite_reject(offset, i);
	}

	return 0;
}

static void *flux_mpk_make_xrstor_trampoline(const unsigned char *addr,
					     const unsigned char *original,
					     size_t length)
{
	static const unsigned char indirect_jump[] = {
		0xff, 0x25, 0x00, 0x00, 0x00, 0x00,
	};
	unsigned char *trampoline;
	uintptr_t continuation;

	trampoline = mmap(NULL, flux_rewrite_page_size, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (trampoline == MAP_FAILED)
		return NULL;
	memcpy(trampoline, original, length);
	memcpy(trampoline + length, indirect_jump, sizeof(indirect_jump));
	continuation = (uintptr_t)addr + length;
	memcpy(trampoline + length + sizeof(indirect_jump), &continuation,
	       sizeof(continuation));
	if (flux_mpk_protect_kernel(trampoline, flux_rewrite_page_size,
				    PROT_READ | PROT_EXEC) < 0) {
		munmap(trampoline, flux_rewrite_page_size);
		return NULL;
	}
	return trampoline;
}

static void flux_mpk_release_site(struct flux_rewrite_site *site)
{
	if (site->trampoline)
		munmap(site->trampoline, flux_rewrite_page_size);
}

int flux_mpk_handle_fault(int signum, void *ucontext,
			  struct flux_mpk_cmp64 *cmp)
{
	ucontext_t *uc = ucontext;
	uintptr_t ip;
	size_t i;

	if (signum != SIGILL || !uc || !flux_rewrite_state)
		return 0;
	ip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
	for (i = 0; i < flux_rewrite_count; i++) {
		struct flux_rewrite_site *site = &flux_rewrite_state->sites[i];

		if ((uintptr_t)site->addr != ip)
			continue;
		if (site->kind == FLUX_REWRITE_MPK_CMP64_RIP) {
			static const int regs[] = {
				REG_RAX, REG_RCX, REG_RDX, REG_RBX,
				REG_RSP, REG_RBP, REG_RSI, REG_RDI,
				REG_R8, REG_R9, REG_R10, REG_R11,
				REG_R12, REG_R13, REG_R14, REG_R15,
			};
			unsigned int reg;
			int32_t displacement;

			if (!cmp || !flux_mpk_is_cmp64_rip(site->original,
							     site->length))
				return -1;
			reg = ((site->original[2] >> 3) & 7) |
			      ((site->original[0] & 4) << 1);
			memcpy(&displacement, site->original + 3,
			       sizeof(displacement));
			cmp->address = ip + site->length + (intptr_t)displacement;
			cmp->lhs = uc->uc_mcontext.gregs[regs[reg]];
			cmp->length = site->length;
			return FLUX_MPK_FAULT_CMP64;
		}
		if (site->kind == FLUX_REWRITE_MPK_MOV_IMM32) {
			static const int regs[] = {
				REG_RAX, REG_RCX, REG_RDX, REG_RBX,
				REG_RSP, REG_RBP, REG_RSI, REG_RDI,
			};
			uint32_t value;
			unsigned char opcode = site->original[0];

			if (site->length != 5 || opcode < 0xb8 || opcode > 0xbf)
				return -1;
			memcpy(&value, site->original + 1, sizeof(value));
			/* A 32-bit destination clears the upper register half. MOV
			 * preserves flags and does not access application memory. */
			uc->uc_mcontext.gregs[regs[opcode - 0xb8]] = value;
			uc->uc_mcontext.gregs[REG_RIP] = ip + site->length;
			return 1;
		}
		if (site->kind == FLUX_REWRITE_MPK_XRSTOR) {
			FLUX_LOG(FLUX_LOG_INFO,
				 "SIGILL matched XRSTOR residual at %p (trampoline %p)\n",
				 site->addr, site->trampoline);
			if ((uc->uc_mcontext.gregs[REG_RAX] & (1UL << 9)) ||
			    !site->trampoline) {
				FLUX_LOG(
					FLUX_LOG_ERR,
					"reject XRSTOR residual at %p rax=%#llx trampoline=%p\n",
					site->addr,
					(unsigned long long)
						uc->uc_mcontext.gregs[REG_RAX],
					site->trampoline);
				return -1;
			}
			FLUX_LOG(FLUX_LOG_INFO,
				 "resume XRSTOR residual at %p via %p\n",
				 site->addr, site->trampoline);
			uc->uc_mcontext.gregs[REG_RIP] =
				(greg_t)(uintptr_t)site->trampoline;
			return 1;
		}
		if (site->kind == FLUX_REWRITE_MPK_WRPKRU) {
			FLUX_LOG(FLUX_LOG_ERR, "reject WRPKRU residual at %p\n",
				 site->addr);
			return -1;
		}
	}
	return 0;
}

#endif

struct flux_disassembly_state {
	unsigned char *code;
	size_t offset;
	char text[256];
	size_t text_len;
	int64_t rsp_disp;
	bool has_rsp_disp;
	bool conflicting_rsp_disp;
	bool output_truncated;
};

static int flux_disassembly_vprintf(struct flux_disassembly_state *state,
				    const char *fmt, va_list args)
{
	char fragment[128];
	int ret;

	if (!strcmp(fmt, "%.*s")) {
		va_list copy;
		const char *string;
		size_t length;
		int precision;

		va_copy(copy, args);
		precision = va_arg(copy, int);
		string = va_arg(copy, const char *);
		va_end(copy);
		if (string && precision >= 0 &&
		    (size_t)precision < sizeof(fragment)) {
			length = strnlen(string, (size_t)precision);
			if (state->text_len + length >= sizeof(state->text)) {
				state->output_truncated = true;
			} else {
				memcpy(state->text + state->text_len, string,
				       length);
				state->text_len += length;
				state->text[state->text_len] = '\0';
			}
			return (int)length;
		}
	}

	ret = vsnprintf(fragment, sizeof(fragment), fmt, args);
	if (ret < 0 || (size_t)ret >= sizeof(fragment)) {
		state->output_truncated = true;
		return ret;
	}
	if (state->text_len + (size_t)ret >= sizeof(state->text)) {
		state->output_truncated = true;
	} else {
		memcpy(state->text + state->text_len, fragment,
		       (size_t)ret + 1);
		state->text_len += (size_t)ret;
	}
	return ret;
}

static int flux_disassembly_printf(void *data, const char *fmt, ...)
{
	struct flux_disassembly_state *state = data;
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = flux_disassembly_vprintf(state, fmt, args);
	va_end(args);
	return ret;
}

static int flux_disassembly_styled_printf(void *data,
					  enum disassembler_style style
					  __attribute__((unused)),
					  const char *fmt, ...)
{
	struct flux_disassembly_state *state = data;
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = flux_disassembly_vprintf(state, fmt, args);
	va_end(args);
	return ret;
}

static void flux_disassembly_parse_rsp(struct flux_disassembly_state *state)
{
	const char *cursor = state->text;

	while ((cursor = strstr(cursor, "(%rsp)"))) {
		const char *start = cursor;
		int64_t disp;

		while (start > state->text && start[-1] != ' ' &&
		       start[-1] != '\t' && start[-1] != ',')
			start--;
		if (*start != '-') {
			cursor += sizeof("(%rsp)") - 1;
			continue;
		}

		if (flux_parse_i64_ascii(start, cursor, 0, &disp) && disp < 0) {
			if (state->has_rsp_disp && state->rsp_disp != disp)
				state->conflicting_rsp_disp = true;
			state->has_rsp_disp = true;
			state->rsp_disp = disp;
		}
		cursor += sizeof("(%rsp)") - 1;
	}
}

static bool flux_disassembly_has_token(const char *text, const char *token)
{
	size_t len = strlen(token);
	const char *pos = text;

	while ((pos = strstr(pos, token))) {
		bool starts_token = pos == text || pos[-1] == 32 ||
				    pos[-1] == 9;
		bool ends_token = pos[len] == 0 || pos[len] == 32 ||
				  pos[len] == 9;

		if (starts_token && ends_token)
			return true;
		pos += len;
	}
	return false;
}

static bool flux_rewrite_ends_region(const char *text)
{
	static const char *const terminators[] = {
		"ret",	"retq", "retf", "lret", "lretq", "jmp",
		"jmpq", "ljmp", "ud2",	"int3", "hlt",
	};
	size_t i;

	for (i = 0; i < sizeof(terminators) / sizeof(terminators[0]); i++)
		if (flux_disassembly_has_token(text, terminators[i]))
			return true;
	return false;
}

static int flux_rewrite_finish_region(struct flux_rewrite_plan *plan,
				      size_t patch_start, bool has_syscall,
				      bool has_unadjustable_redzone,
				      size_t unadjustable_redzone_offset,
				      size_t *offset)
{
	size_t read_idx;
	size_t write_idx;

	if (has_syscall && has_unadjustable_redzone)
		return flux_rewrite_reject(offset, unadjustable_redzone_offset);
	if (has_syscall)
		return 0;

	for (read_idx = patch_start, write_idx = patch_start;
	     read_idx < plan->count; read_idx++) {
		if (plan->patches[read_idx].kind == FLUX_REWRITE_REDZONE)
			continue;
		plan->patches[write_idx++] = plan->patches[read_idx];
	}
	plan->count = write_idx;
	return 0;
}

static int flux_rewrite_redzone(struct flux_disassembly_state *state,
				size_t insn_len, struct flux_rewrite_plan *plan,
				bool *unadjustable)
{
	unsigned char old_disp;
	unsigned char new_disp;
	size_t match = 0;
	size_t i;

	*unadjustable = false;
	/* An MPK residual instruction is copied to its exceptional trampoline.
	 * Adjusting its stack displacement in the original stream would overlap
	 * the trap patch and would also change the instruction being emulated. */
#ifdef CONFIG_FLUX_MPK
	if (flux_rewrite_is_wrpkru(state->code + state->offset) ||
	    flux_rewrite_is_xrstor(state->code + state->offset))
		return 0;
#endif
	if (!state->has_rsp_disp)
		return 0;
	if (state->rsp_disp < -128)
		return 0;
	if (state->conflicting_rsp_disp || state->rsp_disp < -120) {
		*unadjustable = true;
		return 0;
	}

	old_disp = (unsigned char)state->rsp_disp;
	for (i = 0; i + 1 < insn_len; i++) {
		if (state->code[state->offset + i] != 0x24 ||
		    state->code[state->offset + i + 1] != old_disp)
			continue;
		if (match) {
			*unadjustable = true;
			return 0;
		}
		match = state->offset + i + 1;
	}
	if (!match ||
	    flux_rewrite_crosses_page(state->code + state->offset, insn_len)) {
		*unadjustable = true;
		return 0;
	}
	new_disp = old_disp - 8;
	return flux_rewrite_push_patch(plan, match, 1, &new_disp,
				       FLUX_REWRITE_REDZONE);
}

static bool flux_rewrite_is_rt_sigreturn(const unsigned char *code,
					 size_t syscall_offset)
{
	static const unsigned char prefix[] = {
		0x48, 0xc7, 0xc0, 0x0f, 0x00, 0x00, 0x00,
	};

	return syscall_offset >= sizeof(prefix) &&
	       !memcmp(code + syscall_offset - sizeof(prefix), prefix,
		       sizeof(prefix));
}

static bool flux_rewrite_is_rt_sigreturn_gate_call(const unsigned char *code,
						    size_t offset,
						    size_t insn_len)
{
	static const unsigned char prefix[] = {
		0x48, 0xc7, 0xc0, 0x0f, 0x00, 0x00, 0x00,
	};
	static const unsigned char call_gate[] = {
		0xff, 0x14, 0x25, 0x08, 0x00, 0x10, 0x00,
	};

	return insn_len == sizeof(call_gate) && offset >= sizeof(prefix) &&
	       !memcmp(code + offset - sizeof(prefix), prefix, sizeof(prefix)) &&
	       !memcmp(code + offset, call_gate, sizeof(call_gate));
}

static int flux_rewrite_analyze(unsigned char *code, size_t len,
				struct flux_rewrite_plan *plan, size_t *offset)
{
	struct flux_disassembly_state state = { .code = code };
	disassembler_ftype disassemble;
	disassemble_info info = { 0 };
	size_t region_patch_start = 0;
	size_t unadjustable_redzone_offset = 0;
	bool region_has_syscall = false;
	bool region_has_unadjustable_redzone = false;
	bool apx_f = flux_cpu_has_apx_f();
	int ret;

	init_disassemble_info(&info, &state, flux_disassembly_printf,
			      flux_disassembly_styled_printf);
	info.arch = bfd_arch_i386;
	info.mach = bfd_mach_x86_64;
	info.buffer = code;
	info.buffer_length = len;
	disassemble_init_for_target(&info);
	disassemble = disassembler(bfd_arch_i386, false, bfd_mach_x86_64, NULL);
	if (!disassemble) {
		ret = flux_rewrite_reject(offset, 0);
		goto out_free;
	}

	while (state.offset < len) {
		bool unadjustable_redzone = false;
		size_t insn_len;

		state.text[0] = '\0';
		state.text_len = 0;
		state.has_rsp_disp = false;
		state.conflicting_rsp_disp = false;
		state.output_truncated = false;
		ret = disassemble(state.offset, &info);
		if (ret <= 0 || (size_t)ret > len - state.offset ||
		    (size_t)ret > FLUX_REWRITE_MAX_INSN_SIZE ||
		    state.output_truncated) {
			ret = flux_rewrite_reject(offset, state.offset);
			goto out_free;
		}
		insn_len = (size_t)ret;
		flux_disassembly_parse_rsp(&state);
		if (flux_rewrite_is_rt_sigreturn_gate_call(
				code, state.offset, insn_len)) {
			static const unsigned char jmp_gate[] = {
				0xff, 0x24, 0x25, 0x18, 0x00, 0x10, 0x00,
			};

			ret = flux_rewrite_push_patch(
				plan, state.offset, sizeof(jmp_gate), jmp_gate,
				FLUX_REWRITE_SYSCALL);
			if (ret < 0)
				goto out_free;
			region_has_syscall = true;
		} else if (flux_disassembly_has_token(state.text, "syscall")) {
			if (insn_len != 2 || code[state.offset] != 0x0f ||
			    code[state.offset + 1] != 0x05 ||
			    flux_rewrite_crosses_page(code + state.offset, 2)) {
				/*
				 * Binutils 2.42 decodes D5 as an APX REX2 prefix even
				 * when the running CPU has no APX_F support. In that
				 * case the instruction cannot execute as syscall and
				 * would #UD, so do not reject executable data that
				 * happens to contain the three-byte encoding. Keep the
				 * conservative rejection on APX-capable CPUs and for
				 * every other non-canonical syscall decoding.
				 */
				if (apx_f || code[state.offset] != 0xd5) {
					ret = flux_rewrite_reject(offset, state.offset);
					goto out_free;
				}
			} else {
				if (flux_rewrite_is_rt_sigreturn(code,
								 state.offset)) {
					uint32_t slot =
						FLUX_REWRITE_SIGRETURN_NOSTACK;

					ret = flux_rewrite_push_patch(
						plan, state.offset - sizeof(slot),
						sizeof(slot), &slot,
						FLUX_REWRITE_SYSCALL);
					if (ret < 0)
						goto out_free;
					ret = flux_rewrite_push_patch(
						plan, state.offset, 2,
						(const unsigned char[]){ 0xff,
									  0xe0 },
						FLUX_REWRITE_SYSCALL);
				} else {
					ret = flux_rewrite_push_patch(
						plan, state.offset, 2,
						(const unsigned char[]){ 0xff,
									  0xd0 },
						FLUX_REWRITE_SYSCALL);
				}
				if (ret < 0)
					goto out_free;
				region_has_syscall = true;
			}
		}
#ifdef CONFIG_FLUX_MPK
		ret = flux_mpk_analyze_instruction(
			code, len, state.offset, insn_len, plan, offset);
		if (ret < 0)
			goto out_free;
#endif
		ret = flux_rewrite_redzone(&state, insn_len, plan,
					   &unadjustable_redzone);
		if (ret < 0)
			goto out_free;
		if (unadjustable_redzone && !region_has_unadjustable_redzone) {
			region_has_unadjustable_redzone = true;
			unadjustable_redzone_offset = state.offset;
		}
		state.offset += insn_len;
		if (flux_rewrite_ends_region(state.text)) {
			ret = flux_rewrite_finish_region(
				plan, region_patch_start, region_has_syscall,
				region_has_unadjustable_redzone,
				unadjustable_redzone_offset, offset);
			if (ret < 0)
				goto out_free;
			region_patch_start = plan->count;
			region_has_syscall = false;
			region_has_unadjustable_redzone = false;
		}
	}

	ret = flux_rewrite_finish_region(plan, region_patch_start,
					 region_has_syscall,
					 region_has_unadjustable_redzone,
					 unadjustable_redzone_offset, offset);
out_free:
	disassemble_free_target(&info);
	return ret;
}

static int flux_rewrite_protect_state(struct flux_rewrite_state *state,
				      int prot)
{
#ifdef CONFIG_FLUX_MPK
	return flux_mpk_protect_shared(state, flux_rewrite_page_size, prot);
#else
	if (mprotect(state, flux_rewrite_page_size, prot) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "mprotect rewrite state failed: %s\n",
			 strerror(errno));
		return -1;
	}
	return 0;
#endif
}

/* The MM caller serializes writers. Only record storage excludes fault readers. */
static inline void flux_rewrite_records_lock(void)
{
#ifdef CONFIG_FLUX_MPK
	flux_rewrite_state_lock();
#endif
}

static inline void flux_rewrite_records_unlock(void)
{
#ifdef CONFIG_FLUX_MPK
	flux_rewrite_state_unlock();
#endif
}

static int flux_rewrite_reserve_sites(size_t additional)
{
	struct flux_rewrite_site *sites;
	size_t needed;
	size_t bytes;

	if (additional > SIZE_MAX - flux_rewrite_count)
		return -FLUX_ENOMEM;
	needed = flux_rewrite_count + additional;
	if (needed <= flux_rewrite_state->capacity)
		return 0;
	if (needed > SIZE_MAX / sizeof(*sites))
		return -FLUX_ENOMEM;
	bytes = needed * sizeof(*sites);
	if (bytes > SIZE_MAX - (flux_rewrite_page_size - 1))
		return -FLUX_ENOMEM;
	bytes = (bytes + flux_rewrite_page_size - 1) &
		~(flux_rewrite_page_size - 1);

	flux_rewrite_records_lock();
	if (!flux_rewrite_state->sites) {
		sites = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	} else {
		sites = mremap(flux_rewrite_state->sites,
			       flux_rewrite_state->mapping_size, bytes,
			       MREMAP_MAYMOVE);
	}
	if (sites == MAP_FAILED) {
		flux_rewrite_records_unlock();
		return -FLUX_ENOMEM;
	}

	flux_rewrite_state->sites = sites;
	flux_rewrite_state->capacity = bytes / sizeof(*sites);
	flux_rewrite_state->mapping_size = bytes;
	flux_rewrite_records_unlock();
	return 0;
}

static void flux_rewrite_apply(unsigned char *code, size_t len,
			       const struct flux_rewrite_plan *plan)
{
	size_t i;

	for (i = 0; i < plan->count; i++) {
		const struct flux_rewrite_patch *patch = &plan->patches[i];
		struct flux_rewrite_site site = {
			.addr = code + patch->offset,
			.length = patch->length,
			.kind = patch->kind,
		};

		/* Either access can fault; do not exclude a userfault manager. */
		memcpy(site.original, site.addr, site.length);
#ifdef CONFIG_FLUX_MPK
		if (patch->kind == FLUX_REWRITE_MPK_XRSTOR)
			site.trampoline = flux_mpk_make_xrstor_trampoline(
				site.addr, site.original, site.length);
#endif
		flux_rewrite_records_lock();
		flux_rewrite_state->sites[flux_rewrite_count++] = site;
		flux_rewrite_records_unlock();
		memcpy(site.addr, patch->replacement, site.length);
		if (patch->kind == FLUX_REWRITE_MPK_XRSTOR)
			FLUX_LOG(FLUX_LOG_INFO,
				 "recorded XRSTOR residual at %p (trampoline %p)\n",
				 site.addr, site.trampoline);
	}

	__builtin___clear_cache((char *)code, (char *)code + len);
}

static bool flux_rewrite_range_contains(uintptr_t start, uintptr_t end,
					uintptr_t value, size_t width)
{
	return value >= start && value < end && width <= end - value;
}

int flux_rewrite_invalidate(void *addr, size_t len, bool restore)
{
	uintptr_t start = (uintptr_t)addr;
	uintptr_t end;
	size_t read_idx, write_idx;

	if (!len)
		return 0;
	if (!addr || start > UINTPTR_MAX - len)
		return -FLUX_EINVAL;
	if (!flux_rewrite_state)
		return -FLUX_EINVAL;
	end = start + len;

	/* Writers are serialized by the MM transaction mutex. Restore bytes
	 * before taking the record lock, because a restore can fault as well. */
	if (restore) {
		for (read_idx = 0; read_idx < flux_rewrite_count; read_idx++) {
			const struct flux_rewrite_site *site =
				&flux_rewrite_state->sites[read_idx];

			if (flux_rewrite_range_contains(
				    start, end, (uintptr_t)site->addr, site->length))
				memcpy(site->addr, site->original, site->length);
		}
	}
	flux_rewrite_records_lock();

	for (read_idx = 0, write_idx = 0; read_idx < flux_rewrite_count;
	     read_idx++) {
		struct flux_rewrite_site site =
			flux_rewrite_state->sites[read_idx];

		if (!flux_rewrite_range_contains(
			    start, end, (uintptr_t)site.addr, site.length)) {
			flux_rewrite_state->sites[write_idx++] = site;
			continue;
		}
#ifdef CONFIG_FLUX_MPK
		flux_mpk_release_site(&site);
#endif
	}
	flux_rewrite_count = write_idx;
	flux_rewrite_records_unlock();
	if (restore)
		__builtin___clear_cache((char *)addr, (char *)addr + len);
	return 0;
}

int flux_rewrite_init(void)
{
	unsigned char *zero_page = MAP_FAILED;
	struct flux_rewrite_state *state = MAP_FAILED;
	long page_size;

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size < FLUX_REWRITE_MIN_PAGE_SIZE ||
	    (page_size & (page_size - 1))) {
		errno = EINVAL;
		FLUX_LOG(FLUX_LOG_ERR, "invalid host page size %ld\n",
			 page_size);
		return -1;
	}
	flux_rewrite_page_size = (size_t)page_size;

	state = mmap(NULL, (size_t)page_size, PROT_READ,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (state == MAP_FAILED) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to map rewrite state: %s\n",
			 strerror(errno));
		return -1;
	}
	if (flux_rewrite_protect_state(state, PROT_READ | PROT_WRITE) < 0)
		goto out_unmap_state;

	zero_page = mmap((void *)0, (size_t)page_size, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
			 0);
	if (zero_page == MAP_FAILED) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to reserve zero-page trampoline: %s\n",
			 strerror(errno));
		goto out_unmap_state;
	}
	if (zero_page != (unsigned char *)0) {
		errno = EEXIST;
		FLUX_LOG(
			FLUX_LOG_ERR,
			"zero-page trampoline mapped at unexpected address %p\n",
			zero_page);
		goto out_unmap_zero;
	}

	flux_rewrite_low_memset(0, 0x90, (size_t)page_size);
	flux_rewrite_low_memcpy(FLUX_REWRITE_SYSCALL_SLOTS,
				flux_zero_trampoline,
				sizeof(flux_zero_trampoline));
	flux_rewrite_low_memcpy(FLUX_REWRITE_SIGRETURN_NOSTACK,
				flux_zero_sigreturn_nostack,
				sizeof(flux_zero_sigreturn_nostack));

#ifdef CONFIG_FLUX_MPK
	if (syscall(SYS_pkey_mprotect, zero_page, (size_t)page_size,
		    PROT_READ | PROT_EXEC, FLUX_MPK_KERNEL_PKEY) < 0) {
#else
	if (mprotect(zero_page, (size_t)page_size, PROT_READ | PROT_EXEC) < 0) {
#endif
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to protect zero-page trampoline: %s\n",
			 strerror(errno));
		goto out_unmap_zero;
	}

	flux_rewrite_cache_init();
	flux_rewrite_state = state;
	FLUX_LOG(FLUX_LOG_INFO,
		 "zero-page trampoline ready (%d syscall slots)\n",
		 FLUX_REWRITE_SYSCALL_SLOTS);
	return 0;

out_unmap_zero:
	munmap(zero_page, (size_t)page_size);
out_unmap_state:
	munmap(state, (size_t)page_size);
	return -1;
}

int flux_rewrite_exec(void *addr, size_t len, size_t *offset)
{
	unsigned char *code = addr;
	struct flux_rewrite_plan plan = { 0 };
	struct flux_rewrite_plan cached_plan = { 0 };
	const struct flux_rewrite_plan *active_plan = &plan;
	uint64_t hash = 0;
	bool cacheable;
	bool cache_hit;
	int ret;

	if (!len)
		return 0;
	if (!code || (uintptr_t)code > UINTPTR_MAX - len)
		return flux_rewrite_reject(offset, 0);

	/* Restore an earlier rewrite before matching or analyzing JIT pages. */
	ret = flux_rewrite_invalidate(addr, len, true);
	if (ret < 0)
		goto out;
	cacheable = flux_rewrite_cache &&
		    len <= FLUX_REWRITE_CACHE_CODE_SIZE;
	if (cacheable)
		hash = flux_rewrite_hash(code, len);
	cache_hit = cacheable && flux_rewrite_cache_lookup(
		code, len, hash, &cached_plan);
	if (cache_hit) {
		active_plan = &cached_plan;
	} else {
		ret = flux_rewrite_analyze(code, len, &plan, offset);
		if (ret < 0)
			goto out;
	}
	if (active_plan->count) {
		ret = flux_rewrite_reserve_sites(active_plan->count);
		if (ret < 0) {
			if (ret == -FLUX_ENOMEM)
				ret = flux_rewrite_reject(offset, 0);
			goto out;
		}
	}

	if (!cache_hit && cacheable && flux_rewrite_cache_store(
		    code, len, hash, &plan, &cached_plan))
		active_plan = &cached_plan;
	if (active_plan->count)
		flux_rewrite_apply(code, len, active_plan);
	ret = 0;

out:
	flux_rewrite_plan_destroy(&plan);
	return ret;
}
