// SPDX-License-Identifier: GPL-2.0
/* Deployment environment; application ELF and stack construction live in Linux. */
#include <stdlib.h>
#include <string.h>
#include <kernel/asm/flux_oci.h>
#include <flux.h>
#include <flux/runc.h>

void flux_free_envp(char **envp)
{
	size_t i = 0;

	if (!envp)
		return;

	while (envp[i])
		free(envp[i++]);
	free(envp);
}

static size_t flux_count_envp(char **envp)
{
	size_t count = 0;

	if (!envp)
		return 0;

	while (envp[count])
		count++;

	return count;
}

static inline size_t flux_env_key_len(const char *env)
{
	const char *eq = strchr(env, '=');

	return eq ? (size_t)(eq - env) : strlen(env);
}

static bool flux_env_same_key(const char *lhs, const char *rhs)
{
	size_t lhs_len = flux_env_key_len(lhs);
	size_t rhs_len = flux_env_key_len(rhs);

	return lhs_len == rhs_len && strncmp(lhs, rhs, lhs_len) == 0;
}

static const char *flux_find_env_entry(char **envp, int env_num,
				       const char *key)
{
	size_t key_len = strlen(key);
	int i;

	if (!envp)
		return NULL;

	for (i = 0; i < env_num; i++) {
		if (!envp[i])
			continue;
		if (strncmp(envp[i], key, key_len) == 0 &&
		    envp[i][key_len] == '=')
			return envp[i];
	}

	return NULL;
}

static int flux_env_set_or_append(char **envp, size_t *envc, size_t env_cap,
				  const char *entry)
{
	char *copy;
	size_t i;

	copy = strdup(entry);
	if (!copy)
		return -1;

	for (i = 0; i < *envc; i++) {
		if (!flux_env_same_key(envp[i], entry))
			continue;

		free(envp[i]);
		envp[i] = copy;
		return 0;
	}

	if (*envc >= env_cap) {
		free(copy);
		return -1;
	}

	envp[*envc] = copy;
	(*envc)++;
	envp[*envc] = NULL;

	return 0;
}

char **flux_build_envp(void)
{
	const struct flux_oci_cfg *oci = flux_oci_cfg_get();
	char **base = oci ? oci->env : (run_cfg ? run_cfg->env : NULL);
	const char *library_path = NULL;
	int base_count = oci ? oci->env_num : (run_cfg ? run_cfg->env_num : 0);
	size_t base_envc = base && base_count > 0 ? base_count : 0;
	size_t extra_envc = flux_count_envp(flux_extra_envp);
	size_t envc = 0, env_cap, i;
	char **envp;

	if (oci && run_cfg && run_cfg->ld_path && run_cfg->ld_path[0] &&
	    !flux_find_env_entry(base, base_envc, "LD_LIBRARY_PATH"))
		library_path = flux_find_env_entry(run_cfg->env, run_cfg->env_num,
						"LD_LIBRARY_PATH");
	env_cap = base_envc + extra_envc + !!library_path;
	envp = calloc(env_cap + 1, sizeof(*envp));
	if (!envp)
		return NULL;
	for (i = 0; i < base_envc; i++)
		if (flux_env_set_or_append(envp, &envc, env_cap, base[i]) < 0)
			goto out;
	if (library_path &&
	    flux_env_set_or_append(envp, &envc, env_cap, library_path) < 0)
		goto out;
	for (i = 0; i < extra_envc; i++)
		if (flux_env_set_or_append(envp, &envc, env_cap,
					flux_extra_envp[i]) < 0)
			goto out;
	return envp;
out:
	flux_free_envp(envp);
	return NULL;
}
