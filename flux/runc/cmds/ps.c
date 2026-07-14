#include <errno.h>
#include <ctype.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <flux.h>

#include "../runc.h"

enum flux_runc_output_format {
	FLUX_RUNC_OUTPUT_TABLE = 0,
	FLUX_RUNC_OUTPUT_JSON,
};

struct flux_runc_ps_cmd {
	const char *container_id;
	enum flux_runc_output_format format;
};

static int flux_runc_ps_parse(int argc, char **argv,
			      struct flux_runc_ps_cmd *cmd)
{
	static struct option options[] = {
		{ "format", required_argument, 0, 'f' },
		{ "help", no_argument, 0, 'h' },
		{ NULL, 0, 0, 0 },
	};
	int opt;

	memset(cmd, 0, sizeof(*cmd));
	cmd->format = FLUX_RUNC_OUTPUT_TABLE;
	optind = 2;

	while ((opt = getopt_long(argc, argv, "+f:h", options, NULL)) != -1) {
		switch (opt) {
		case 'f':
			if (!strcmp(optarg, "json"))
				cmd->format = FLUX_RUNC_OUTPUT_JSON;
			else if (!strcmp(optarg, "table"))
				cmd->format = FLUX_RUNC_OUTPUT_TABLE;
			else
				return -EINVAL;
			break;
		case 'h':
		default:
			return -EINVAL;
		}
	}

	if (argc - optind != 1)
		return -EINVAL;

	cmd->container_id = argv[optind];
	return 0;
}

static int flux_runc_ps_capture_exec_output(const char *container_id,
					    char **buf_out, size_t *len_out,
					    int *exit_code_out)
{
	char *buf = NULL;
	size_t len = 0;
	size_t cap = 0;
	int pipefd[2];
	pid_t child;
	int ret = 0;
	int status;

	if (pipe(pipefd) < 0)
		return -errno;

	child = fork();
	if (child < 0) {
		ret = -errno;
		goto out;
	}

	if (child == 0) {
		char *exec_argv[] = {
			"flux-runc", "exec", (char *)container_id,
			"/bin/ps",   NULL,
		};

		close(pipefd[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) < 0)
			_exit(127);
		close(pipefd[1]);
		_exit(flux_runc_cmd_exec(4, exec_argv));
	}

	close(pipefd[1]);
	for (;;) {
		char tmp[256];
		ssize_t nr = read(pipefd[0], tmp, sizeof(tmp));

		if (nr < 0) {
			if (errno == EINTR)
				continue;
			ret = -errno;
			break;
		}
		if (nr == 0)
			break;

		if (len + (size_t)nr + 1 > cap) {
			size_t new_cap = cap ? cap * 2 : 1024;

			while (new_cap < len + (size_t)nr + 1)
				new_cap *= 2;

			buf = realloc(buf, new_cap);
			if (!buf) {
				ret = -ENOMEM;
				break;
			}
			cap = new_cap;
		}

		memcpy(buf + len, tmp, (size_t)nr);
		len += (size_t)nr;
	}

	if (!buf) {
		buf = calloc(1, 1);
		if (!buf)
			ret = -ENOMEM;
	} else {
		buf[len] = '\0';
	}

	if (waitpid(child, &status, 0) < 0) {
		ret = -errno;
		goto out;
	}

	if (exit_code_out) {
		if (WIFEXITED(status))
			*exit_code_out = WEXITSTATUS(status);
		else if (WIFSIGNALED(status))
			*exit_code_out = 128 + WTERMSIG(status);
		else
			*exit_code_out = EXIT_FAILURE;
	}

	if (buf_out)
		*buf_out = buf;
	else
		free(buf);
	if (len_out)
		*len_out = len;
	close(pipefd[0]);
	return ret;

out:
	close(pipefd[0]);
	if (child > 0)
		(void)waitpid(child, &status, 0);
	free(buf);
	return ret;
}

int flux_runc_cmd_ps(int argc, char **argv)
{
	struct flux_runc_ps_cmd cmd;
	char *output = NULL;
	size_t output_len = 0;
	int exec_rc = EXIT_FAILURE;
	int ret;

	ret = flux_runc_ps_parse(argc, argv, &cmd);
	if (ret < 0)
		return EXIT_FAILURE;

	ret = flux_runc_ps_capture_exec_output(cmd.container_id, &output,
					       &output_len, &exec_rc);
	if (ret < 0) {
		flux_runc_log_errno("failed to run container ps", ret);
		return EXIT_FAILURE;
	}

	if (cmd.format == FLUX_RUNC_OUTPUT_JSON) {
		const char *cursor = output;
		bool first = true;

		if (fputs("[\n", stdout) == EOF) {
			free(output);
			return EXIT_FAILURE;
		}

		while (cursor && *cursor) {
			char *end;
			long pid;

			while (*cursor == ' ' || *cursor == '\t' ||
			       *cursor == '\n' || *cursor == '\r')
				cursor++;
			if (!*cursor)
				break;
			if (!isdigit((unsigned char)*cursor)) {
				while (*cursor && *cursor != '\n')
					cursor++;
				continue;
			}

			errno = 0;
			pid = strtol(cursor, &end, 10);
			if (errno || end == cursor || pid <= 0) {
				while (*cursor && *cursor != '\n')
					cursor++;
				continue;
			}

			if (!first && fputs(",\n", stdout) == EOF) {
				free(output);
				return EXIT_FAILURE;
			}
			if (fprintf(stdout, "  %ld", pid) < 0) {
				free(output);
				return EXIT_FAILURE;
			}
			first = false;
			cursor = end;
		}

		if (fputs("\n]\n", stdout) == EOF) {
			free(output);
			return EXIT_FAILURE;
		}
	} else if (output_len > 0 &&
		   fwrite(output, 1, output_len, stdout) != output_len) {
		free(output);
		return EXIT_FAILURE;
	}

	free(output);
	return exec_rc == 0 ? 0 : EXIT_FAILURE;
}
