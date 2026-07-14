#include <stdio.h>
#include <string.h>

#include <flux.h>

int flux_sysctl(const char *path, const char *value)
{
	int ret;
	int fd;
	char *delim, *p;
	char full_path[256];

	flux_mount_fs("proc");

	snprintf(full_path, sizeof(full_path), "/proc/sys/%s", path);
	p = full_path;
	while ((delim = strstr(p, "."))) {
		*delim = '/';
		p = delim + 1;
	}

	fd = flux_sys_open(full_path, FLUX_O_WRONLY | FLUX_O_CREAT, 0);
	if (fd < 0) {
		printf("flux_sys_open %s: %s\n", full_path, flux_strerror(fd));
		return -1;
	}
	ret = flux_sys_write(fd, value, strlen(value));
	if (ret < 0) {
		printf("flux_sys_write %s: %s\n", full_path, flux_strerror(fd));
	}

	flux_sys_close(fd);

	return 0;
}

/* Configure sysctl parameters as the form of "key=value;key=value;..." */
void flux_sysctl_parse_write(const char *sysctls)
{
	char *saveptr = NULL, *token = NULL;
	char *key = NULL, *value = NULL;
	char strings[256];
	int ret = 0;

	strcpy(strings, sysctls);
	for (token = strtok_r(strings, ";", &saveptr); token;
	     token = strtok_r(NULL, ";", &saveptr)) {
		key = strtok(token, "=");
		value = strtok(NULL, "=");
		ret = flux_sysctl(key, value);
		if (ret) {
			printf("Failed to configure sysctl entries: %s\n",
			       flux_strerror(ret));
			return;
		}
	}
}
