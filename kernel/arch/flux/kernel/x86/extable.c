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

	default:
		pr_err("Unknown ex_table type %d at 0x%llx\n", type, regs->ip);
		BUG();
	}
}