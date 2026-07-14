#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Generate a syscall table header.
#
# Each line of the syscall table should have the following format:
#
# NR ABI NAME [NATIVE] [COMPAT]
#
# NR       syscall number
# ABI      ABI name
# NAME     syscall name
# NATIVE   native entry point (optional)
# COMPAT   compat entry point (optional)
#
# Unlike scripts/syscalltbl.sh, this variant allows duplicate syscall numbers.
# For duplicates, the later line overrides the earlier line.

set -e

usage() {
	echo >&2 "usage: $0 [--abis ABIS] INFILE OUTFILE" >&2
	echo >&2
	echo >&2 "  INFILE    input syscall table"
	echo >&2 "  OUTFILE   output header file"
	echo >&2
	echo >&2 "options:"
	echo >&2 "  --abis ABIS        ABI(s) to handle (By default, all lines are handled)"
	exit 1
}

# default unless specified by options
abis=

while [ $# -gt 0 ]
do
	case $1 in
	--abis)
		abis=$(echo "($2)" | tr ',' '|')
		shift 2;;
	-*)
		echo "$1: unknown option" >&2
		usage;;
	*)
		break;;
	esac
done

if [ $# -ne 2 ]; then
	usage
fi

infile="$1"
outfile="$2"

grep -E "^[0-9]+[[:space:]]+$abis" "$infile" | \
awk '
{
	nr = $1 + 0
	native = $4
	compat = $5

	if (nr > max)
		max = nr

	if (compat != "")
		line[nr] = "__SYSCALL_WITH_COMPAT(" nr ", " native ", " compat ")"
	else if (native != "")
		line[nr] = "__SYSCALL(" nr ", " native ")"
	else
		line[nr] = "__SYSCALL(" nr ", sys_ni_syscall)"
}
END {
	if (max < 0)
		exit 0

	for (i = 0; i <= max; i++) {
		if (i in line)
			print line[i]
		else
			print "__SYSCALL(" i ", sys_ni_syscall)"
	}
}
' > "$outfile"
