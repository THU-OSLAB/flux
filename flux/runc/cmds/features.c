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
		  "  \"ociVersionMax\": \"1.2.1\",\n"
		  "  \"process\": {\n"
		  "    \"terminal\": true,\n"
		  "    \"cwd\": true,\n"
		  "    \"user\": true,\n"
		  "    \"capabilities\": true,\n"
		  "    \"rlimits\": true,\n"
		  "    \"noNewPrivileges\": true\n"
		  "  },\n"
		  "  \"linux\": {\n"
		  "    \"hostname\": true,\n"
		  "    \"mounts\": true,\n"
		  "    \"maskedPaths\": true,\n"
		  "    \"readonlyPaths\": true,\n"
		  "    \"namespaces\": false,\n"
		  "    \"resources\": false,\n"
		  "    \"seccomp\": false,\n"
		  "    \"apparmor\": false,\n"
		  "    \"selinux\": false\n"
		  "  }\n"
		  "}\n",
		  stdout) == EOF)
		return EXIT_FAILURE;

	return 0;
}
