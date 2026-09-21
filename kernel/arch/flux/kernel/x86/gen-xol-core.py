#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Reuse the in-tree Linux 6.6 uprobes instruction algorithms for Flux.

The native x86 exception/debug-register entry is replaced by the Flux signal
adapter. The instruction analysis and relocation bodies retain their source.
"""
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text()
stop = source.index('/*\n * arch_uprobe_pre_xol - prepare to execute out of line.')
body = ''.join(line for line in source[:stop].splitlines(True)
               if not line.startswith('#include '))
assert body.count('current->utask') == 3
body = body.replace('struct uprobe_task', 'struct flux_xol_state')
body = body.replace('current->utask', 'current->thread.xol')
body = body.replace('int arch_uprobe_analyze_insn(',
                    'static int flux_xol_analyze_insn(')
# Flux names its register storage u64, whereas native x86 uses unsigned long.
# Keep typed register pointers consistent without changing field offsets.
assert body.count('static inline unsigned long *\nscratch_reg') == 1
assert body.count('unsigned long *sr = scratch_reg') == 2
body = body.replace('static inline unsigned long *\nscratch_reg',
                    'static inline u64 *\nscratch_reg')
body = body.replace('unsigned long *sr = scratch_reg', 'u64 *sr = scratch_reg')
body = body.replace('unsigned long *src_ptr = (void *)regs',
                    'u64 *src_ptr = (void *)regs')
pathlib.Path(sys.argv[2]).write_text(body)
