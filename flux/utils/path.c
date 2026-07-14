#define _GNU_SOURCE

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <utils/path.h>

static int flux_lookup_username_by_uid(uid_t uid, char *buf, size_t len)
{
	struct passwd pwd;
	struct passwd *result = NULL;
	long pwbuf_len;
	char *pwbuf;
	int ret;

	pwbuf_len = sysconf(_SC_GETPW_R_SIZE_MAX);
	if (pwbuf_len < 0)
		pwbuf_len = 1024;

	pwbuf = malloc((size_t)pwbuf_len);
	if (!pwbuf)
		return -1;

	ret = getpwuid_r(uid, &pwd, pwbuf, (size_t)pwbuf_len, &result);
	if (ret == 0 && result && result->pw_name && result->pw_name[0]) {
		if (snprintf(buf, len, "%s", result->pw_name) >= (int)len) {
			free(pwbuf);
			return -1;
		}
		free(pwbuf);
		return 0;
	}

	free(pwbuf);
	if (snprintf(buf, len, "%lu", (unsigned long)uid) >= (int)len)
		return -1;
	return 0;
}

static uid_t flux_path_owner_uid(void)
{
	const char *sudo_uid;
	char *end;
	unsigned long uid;

	sudo_uid = getenv("SUDO_UID");
	if (!sudo_uid || !sudo_uid[0])
		return getuid();

	errno = 0;
	uid = strtoul(sudo_uid, &end, 10);
	if (errno || *end)
		return getuid();

	return (uid_t)uid;
}

char *flux_user_scoped_path_strdup(const char *base)
{
	char user[64];
	uid_t uid;
	size_t len;
	char *path;

	if (!base || !base[0])
		return NULL;

	uid = flux_path_owner_uid();
	if (flux_lookup_username_by_uid(uid, user, sizeof(user)) < 0)
		return NULL;

	len = strlen(base) + 1 + strlen(user) + 1;
	path = malloc(len);
	if (!path)
		return NULL;

	if (snprintf(path, len, "%s.%s", base, user) >= (int)len) {
		free(path);
		return NULL;
	}

	return path;
}
