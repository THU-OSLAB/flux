#include <flux.h>

int main(int argc, char *argv[])
{
	struct flux_launch_spec spec;
	int ret;

	if (flux_launch_should_bootstrap_multiproc(argc, argv))
		return flux_launch_bootstrap_multiproc(argv);

	ret = flux_launch_prepare_flux_cli(&spec, argc, argv);
	if (ret > 0)
		return 0;
	if (ret < 0) {
		flux_launch_cleanup(&spec);
		return -1;
	}

	ret = flux_launch_run(&spec);
	flux_launch_cleanup(&spec);
	return ret;
}
