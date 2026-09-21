// SPDX-License-Identifier: GPL-2.0-only
#include <linux/extable.h>
#include <linux/uaccess.h>
#include <linux/sched/debug.h>
#include <linux/bitfield.h>

#include <asm/extable.h>

static inline unsigned long ex_fixup_addr(const struct exception_table_entry *x)
{
	return (unsigned long)&x->fixup + x->fixup;
}

static bool ex_handler_default(const struct exception_table_entry *e,
			       struct pt_regs *regs)
{
	if (e->data & EX_FLAG_CLEAR_AX)
		regs->ax = 0;
	if (e->data & EX_FLAG_CLEAR_DX)
		regs->dx = 0;

	regs->ip = ex_fixup_addr(e);
	return true;
}

static bool ex_handler_fault(const struct exception_table_entry *e,
			     struct pt_regs *regs, int trapnr)
{
	regs->ax = trapnr;
	return ex_handler_default(e, regs);
}

int fixup_exception(struct pt_regs *regs, int trapnr, unsigned long error_code,
		    unsigned long fault_addr)
{
	const struct exception_table_entry *e;
	int type, reg, imm;

	e = search_exception_tables(regs->ip);
	if (!e)
		return 0;

	type = FIELD_GET(EX_DATA_TYPE_MASK, e->data);
	reg = FIELD_GET(EX_DATA_REG_MASK, e->data);
	imm = FIELD_GET(EX_DATA_IMM_MASK, e->data);

	switch (type) {
	case EX_TYPE_DEFAULT:
	case EX_TYPE_UACCESS:
		return ex_handler_default(e, regs);

	case EX_TYPE_FAULT:
	case EX_TYPE_FAULT_MCE_SAFE:
		return ex_handler_fault(e, regs, trapnr);

	case EX_TYPE_FLUX_IOPORT:
		regs->r8 = fault_addr;
		regs->r9 = error_code;
		regs->r10 = trapnr | (1UL << 63);
		return ex_handler_default(e, regs);

	case EX_TYPE_FLUX_MPK_READ:
		regs->ax = 0;
		regs->dx = fault_addr;
		regs->cx = error_code | (1UL << 63);
		return ex_handler_default(e, regs);

	default:
		pr_err("Unknown ex_table type %d at 0x%llx\n", type, regs->ip);
		BUG();
	}
}
