#include <stdlib.h>
#include <string.h>

#include <flux.h>

#include "../runc.h"

int flux_runc_cmd_features(int argc, char **argv)
{
	if (argc != 2)
		return EXIT_FAILURE;

	if (fputs("{\n"
		  "  \"ociVersionMin\": \"1.0.0\",\n"
		  "  \"ociVersionMax\": \"1.3.0\",\n"
		  "  \"mountOptions\": [\n"
		  "    \"bind\", \"rbind\", \"ro\", \"rw\",\n"
		  "    \"nosuid\", \"suid\", \"nodev\", \"dev\",\n"
		  "    \"noexec\", \"exec\", \"noatime\",\n"
		  "    \"nodiratime\", \"relatime\", \"strictatime\",\n"
		  "    \"dirsync\", \"sync\", \"defaults\"\n"
		  "  ],\n"
		  "  \"linux\": {\n"
		  "    \"capabilities\": [\n"
		  "      \"CAP_CHOWN\", \"CAP_DAC_OVERRIDE\",\n"
		  "      \"CAP_DAC_READ_SEARCH\", \"CAP_FOWNER\",\n"
		  "      \"CAP_FSETID\", \"CAP_KILL\", \"CAP_SETGID\",\n"
		  "      \"CAP_SETUID\", \"CAP_SETPCAP\",\n"
		  "      \"CAP_LINUX_IMMUTABLE\",\n"
		  "      \"CAP_NET_BIND_SERVICE\", \"CAP_NET_BROADCAST\",\n"
		  "      \"CAP_NET_ADMIN\", \"CAP_NET_RAW\",\n"
		  "      \"CAP_IPC_LOCK\", \"CAP_IPC_OWNER\",\n"
		  "      \"CAP_SYS_MODULE\", \"CAP_SYS_RAWIO\",\n"
		  "      \"CAP_SYS_CHROOT\", \"CAP_SYS_PTRACE\",\n"
		  "      \"CAP_SYS_PACCT\", \"CAP_SYS_ADMIN\",\n"
		  "      \"CAP_SYS_BOOT\", \"CAP_SYS_NICE\",\n"
		  "      \"CAP_SYS_RESOURCE\", \"CAP_SYS_TIME\",\n"
		  "      \"CAP_SYS_TTY_CONFIG\", \"CAP_MKNOD\",\n"
		  "      \"CAP_LEASE\", \"CAP_AUDIT_WRITE\",\n"
		  "      \"CAP_AUDIT_CONTROL\", \"CAP_SETFCAP\",\n"
		  "      \"CAP_MAC_OVERRIDE\", \"CAP_MAC_ADMIN\",\n"
		  "      \"CAP_SYSLOG\", \"CAP_WAKE_ALARM\",\n"
		  "      \"CAP_BLOCK_SUSPEND\", \"CAP_AUDIT_READ\",\n"
		  "      \"CAP_PERFMON\", \"CAP_BPF\",\n"
		  "      \"CAP_CHECKPOINT_RESTORE\"\n"
		  "    ],\n"
		  "    \"cgroup\": {\n"
		  "      \"v1\": true, \"v2\": true,\n"
		  "      \"systemd\": false, \"systemdUser\": false,\n"
		  "      \"rdma\": false\n"
		  "    },\n"
		  "    \"seccomp\": { \"enabled\": false },\n"
		  "    \"apparmor\": { \"enabled\": false },\n"
		  "    \"selinux\": { \"enabled\": false },\n"
		  "    \"intelRdt\": { \"enabled\": false }\n"
		  "  }\n"
		  "}\n",
		  stdout) == EOF)
		return EXIT_FAILURE;

	return 0;
}
