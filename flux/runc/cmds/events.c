#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../runc.h"

int flux_runc_cmd_events(int argc, char **argv)
{
	static struct option options[] = {
		{ "interval", required_argument, NULL, 'i' },
		{ "stats", no_argument, NULL, 's' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct timespec interval = { .tv_sec = 5 };
	bool one_shot = false;
	int opt;

	optind = 2;
	while ((opt = getopt_long(argc, argv, "+i:sh", options, NULL)) != -1) {
		switch (opt) {
		case 'i': {
			char *end;
			double seconds;

			errno = 0;
			seconds = strtod(optarg, &end);
			if (errno || end == optarg || *end || seconds <= 0)
				return EXIT_FAILURE;
			interval.tv_sec = (time_t)seconds;
			interval.tv_nsec = (long)((seconds - interval.tv_sec) *
						  1000000000.0);
			break;
		}
		case 's':
			one_shot = true;
			break;
		case 'h':
		default:
			return EXIT_FAILURE;
		}
	}
	if (argc - optind != 1)
		return EXIT_FAILURE;

	for (;;) {
		int ret = flux_runc_stats_emit(argv[optind], stdout);

		if (ret < 0) {
			flux_runc_log_errno("failed to collect container events", ret);
			return EXIT_FAILURE;
		}
		fflush(stdout);
		if (one_shot)
			return EXIT_SUCCESS;
		while (nanosleep(&interval, &interval) < 0) {
			if (errno != EINTR)
				return EXIT_FAILURE;
		}
	}
}
