#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/sched/clock.h>
#include <asm/sparsemem.h>
#include <asm/unistd.h>
#include <asm/setup.h>
#include <asm/smp.h>
#include <asm/processor.h>
#include <asm/host_ops.h>
#include <asm/x86/cpufeature.h>
#include <asm/x86/cpuid.h>
#include <asm/x86/tsc.h>
#include <asm/x86/fpu.h>

union fpregs_state init_task_xstate __aligned(PAGE_SIZE) = {
	.xsave.i387.cwd = 0x37f,
	.xsave.i387.mxcsr = MXCSR_DEFAULT,
};
unsigned int flux_xstate_copy_size __ro_after_init = sizeof(struct xregs_state);

/*
 * Although we spell it out in here, the Processor Trace
 * xfeature is completely unused.  We use other mechanisms
 * to save/restore PT state in Linux.
 */
static const char *xfeature_names[] = {
	"x87 floating point registers",
	"SSE registers",
	"AVX registers",
	"MPX bounds registers",
	"MPX CSR",
	"AVX-512 opmask",
	"AVX-512 Hi256",
	"AVX-512 ZMM_Hi256",
	"Processor Trace (unused)",
	"Protection Keys User registers",
	"PASID state",
	"Control-flow User registers",
	"Control-flow Kernel registers (unused)",
	"unknown xstate feature",
	"unknown xstate feature",
	"unknown xstate feature",
	"unknown xstate feature",
	"AMX Tile config",
	"AMX Tile data",
	"unknown xstate feature",
};

/*
 * Return whether the system supports a given xfeature.
 *
 * Also return the name of the (most advanced) feature that the caller requested:
 */
int cpu_has_xfeatures(u64 xfeatures_needed, const char **feature_name)
{
	u64 xfeatures_missing = xfeatures_needed &
				~boot_cpu_data.xstate_features;

	if (unlikely(feature_name)) {
		long xfeature_idx, max_idx;
		u64 xfeatures_print;
		/*
		 * So we use FLS here to be able to print the most advanced
		 * feature that was requested but is missing. So if a driver
		 * asks about "XFEATURE_MASK_SSE | XFEATURE_MASK_YMM" we'll print the
		 * missing AVX feature - this is the most informative message
		 * to users:
		 */
		if (xfeatures_missing)
			xfeatures_print = xfeatures_missing;
		else
			xfeatures_print = xfeatures_needed;

		xfeature_idx = fls64(xfeatures_print) - 1;
		max_idx = ARRAY_SIZE(xfeature_names) - 1;
		xfeature_idx = min(xfeature_idx, max_idx);

		*feature_name = xfeature_names[xfeature_idx];
	}

	if (xfeatures_missing)
		return 0;

	return 1;
}
EXPORT_SYMBOL_GPL(cpu_has_xfeatures);

struct cpuinfo_x86 boot_cpu_data __read_mostly;
EXPORT_SYMBOL(boot_cpu_data);

static void get_cpu_cap(struct cpuinfo_x86 *c)
{
	u32 eax, ebx, ecx, edx;

	/* Disabling the serial number may affect the cpuid level */
	c->cpuid_level = cpuid_eax(0);

	/* Intel-defined flags: level 0x00000001 */
	if (c->cpuid_level >= 0x00000001) {
		cpuid(0x00000001, &eax, &ebx, &ecx, &edx);

		c->x86_capability[CPUID_1_ECX] = ecx;
		c->x86_capability[CPUID_1_EDX] = edx;
	}

	/* Thermal and Power Management Leaf: level 0x00000006 (eax) */
	if (c->cpuid_level >= 0x00000006)
		c->x86_capability[CPUID_6_EAX] = cpuid_eax(0x00000006);

	/* Additional Intel-defined flags: level 0x00000007 */
	if (c->cpuid_level >= 0x00000007) {
		cpuid_count(0x00000007, 0, &eax, &ebx, &ecx, &edx);
		c->x86_capability[CPUID_7_0_EBX] = ebx;
		c->x86_capability[CPUID_7_ECX] = ecx;
		c->x86_capability[CPUID_7_EDX] = edx;

		/* Check valid sub-leaf index before accessing it */
		if (eax >= 1) {
			cpuid_count(0x00000007, 1, &eax, &ebx, &ecx, &edx);
			c->x86_capability[CPUID_7_1_EAX] = eax;
		}
	}

	/* Extended state features: level 0x0000000d */
	if (c->cpuid_level >= 0x0000000d) {
		cpuid_count(0x0000000d, 1, &eax, &ebx, &ecx, &edx);

		c->x86_capability[CPUID_D_1_EAX] = eax;
	}

	/* AMD-defined flags: level 0x80000001 */
	eax = cpuid_eax(0x80000000);
	c->extended_cpuid_level = eax;

	if ((eax & 0xffff0000) == 0x80000000) {
		if (eax >= 0x80000001) {
			cpuid(0x80000001, &eax, &ebx, &ecx, &edx);

			c->x86_capability[CPUID_8000_0001_ECX] = ecx;
			c->x86_capability[CPUID_8000_0001_EDX] = edx;
		}
	}

	if (c->extended_cpuid_level >= 0x80000007) {
		cpuid(0x80000007, &eax, &ebx, &ecx, &edx);

		c->x86_capability[CPUID_8000_0007_EBX] = ebx;
		c->x86_power = edx;
	}

	if (c->extended_cpuid_level >= 0x80000008) {
		cpuid(0x80000008, &eax, &ebx, &ecx, &edx);
		c->x86_capability[CPUID_8000_0008_EBX] = ebx;
	}

	if (c->extended_cpuid_level >= 0x8000000a)
		c->x86_capability[CPUID_8000_000A_EDX] = cpuid_edx(0x8000000a);

	if (c->extended_cpuid_level >= 0x8000001f)
		c->x86_capability[CPUID_8000_001F_EAX] = cpuid_eax(0x8000001f);

	if (c->extended_cpuid_level >= 0x80000021)
		c->x86_capability[CPUID_8000_0021_EAX] = cpuid_eax(0x80000021);
}

static void get_cpu_vendor(struct cpuinfo_x86 *c)
{
	unsigned int eax, ebx, ecx, edx;

	/* Get vendor_id */
	cpuid(0, &eax, &ebx, &ecx, &edx);
	memcpy(c->x86_vendor_id + 0, &ebx, 4);
	memcpy(c->x86_vendor_id + 4, &edx, 4);
	memcpy(c->x86_vendor_id + 8, &ecx, 4);
	c->x86_vendor_id[12] = '\0';

	if (strncmp(c->x86_vendor_id, "GenuineIntel", 12) == 0)
		c->x86_vendor = X86_VENDOR_INTEL;
	else if (strncmp(c->x86_vendor_id, "AuthenticAMD", 12) == 0)
		c->x86_vendor = X86_VENDOR_AMD;
	else
		c->x86_vendor = X86_VENDOR_UNKNOWN;
}

static void get_cpu_model(struct cpuinfo_x86 *c)
{
	unsigned int eax, ebx, ecx, edx;

	/* Get family/model/stepping from CPUID.1 */
	cpuid(1, &eax, &ebx, &ecx, &edx);
	int family = (eax >> 8) & 0xf;
	int model = (eax >> 4) & 0xf;
	int ext_model = (eax >> 16) & 0xf;
	if (family == 6 || family == 15)
		model |= (ext_model << 4);
	c->x86 = family;
	c->x86_model = model;
	c->x86_stepping = eax & 0xf;

	/* x86_model_id: from CPUID 0x80000002–4 */
	char *p = c->x86_model_id;
	for (int i = 0; i < 3; i++) {
		cpuid(0x80000002 + i, &eax, &ebx, &ecx, &edx);
		memcpy(p, &eax, 4);
		p += 4;
		memcpy(p, &ebx, 4);
		p += 4;
		memcpy(p, &ecx, 4);
		p += 4;
		memcpy(p, &edx, 4);
		p += 4;
	}
	c->x86_model_id[48] = '\0';
}

static void get_cpu_fpu_features(struct cpuinfo_x86 *c)
{
	unsigned int eax, ebx, ecx, edx;

	/*
	 * Find user xstates supported by the processor.
	 */
	cpuid_count(XSTATE_CPUID, 0, &eax, &ebx, &ecx, &edx);
	c->xstate_features = eax + ((u64)edx << 32);
}

static void get_flux_xstate_copy_size(void)
{
	unsigned int size = sizeof(struct xregs_state);
	u64 mask = FLUX_XSAVE_MASK;
	int feature;

	for (feature = FIRST_EXTENDED_XFEATURE; feature < XFEATURE_MAX;
	     feature++) {
		u32 eax, ebx, ecx, edx;
		u64 bit = BIT_ULL(feature);

		if (!(mask & bit))
			continue;

		cpuid_count(XSTATE_CPUID, feature, &eax, &ebx, &ecx, &edx);
		if (!eax)
			continue;

		size = max(size, ebx + eax);
	}

	flux_xstate_copy_size = min_t(unsigned int, size,
				       sizeof(union fpregs_state));
}

void __init fill_cpuinfo(struct cpuinfo_x86 *c)
{
	get_cpu_vendor(c);
	get_cpu_model(c);
	get_cpu_cap(c);
	get_cpu_fpu_features(c);
	get_flux_xstate_copy_size();
}
