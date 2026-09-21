#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Reuse Linux 6.6 task I/O permission rules; Flux uses faults, not a TSS."""
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text()
body = source[source.index('static atomic64_t io_bitmap_sequence;'):
              source.index('\n#else /* CONFIG_X86_IOPL_IOPERM */')]
start = body.index('static void task_update_io_bitmap(')
stop = body.index('\nvoid io_bitmap_exit(', start)
body = body[:start] + '''/* The Flux instruction fault path reads task state directly. */
static void task_update_io_bitmap(struct task_struct *tsk) { }
''' + body[stop:]
for text in ['\tset_tsk_thread_flag(tsk, TIF_IO_BITMAP);\n',
             '\tset_thread_flag(TIF_IO_BITMAP);\n']:
    assert body.count(text) == 1
    body = body.replace(text, '')
body = body.replace('io_bitmap_share', 'flux_io_bitmap_share')
body = body.replace('io_bitmap_exit', 'flux_io_bitmap_exit')
body = body.replace('long ksys_ioperm(', 'static long flux_ksys_ioperm(')
body = body.replace('return ksys_ioperm(', 'return flux_ksys_ioperm(')
body = body.replace('SYSCALL_DEFINE3(ioperm,', 'SYSCALL_DEFINE3(host_ioperm,')
body = body.replace('SYSCALL_DEFINE1(iopl,', 'SYSCALL_DEFINE1(host_iopl,')
needle = '\t/*\n\t * If it\'s the first ioperm() call'
assert body.count(needle) == 1
body = body.replace(needle, '''\tif (turn_on) {
		long ret = flux_io_backend_ready();

		if (ret)
			return ret;
	}

''' + needle)
needle = '''\t\tif (!capable(CAP_SYS_RAWIO) ||
		    security_locked_down(LOCKDOWN_IOPORT))
			return -EPERM;
'''
assert body.count(needle) == 1
body = body.replace(needle, needle + '''\t	{
			long ret = flux_io_backend_ready();

			if (ret)
				return ret;
		}
''')
pathlib.Path(sys.argv[2]).write_text(body)
