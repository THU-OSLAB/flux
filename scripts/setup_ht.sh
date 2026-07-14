#!/usr/bin/env bash

function write_sysfs_value() {
	local path="$1"
	local value="$2"
	printf '%s' "$value" | tee "$path" >/dev/null 2>&1
}

function set_cpu_governor() {
	local target="$1"
	local governor=""
	local ok=0
	local failed=0

	for governor in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
		[ -e "$governor" ] || continue
		if write_sysfs_value "$governor" "$target"; then
			((ok++))
		else
			((failed++))
		fi
	done

	if [ "$ok" -eq 0 ] && [ "$failed" -eq 0 ]; then
		echo "Skipped (cpufreq not available)"
	elif [ "$failed" -eq 0 ]; then
		echo "Done ($ok CPUs)"
	else
		echo "Done ($ok ok, $failed skipped)"
	fi
}

function configure_performance() {
	echo -n "Placing all CPUs in performance mode..."
	set_cpu_governor "performance"

	if [ -f "/sys/devices/system/cpu/intel_pstate/no_turbo" ]; then
		echo -n "Disabling Turbo Boost..."
		if write_sysfs_value "/sys/devices/system/cpu/intel_pstate/no_turbo" "1"; then
			echo "Done"
		else
			echo "Skipped (not permitted on this host)"
		fi
	fi

    echo -n "Disabling SMT..."
    echo off | tee /sys/devices/system/cpu/smt/control
    echo "Done"

    echo -n "Disabling NMI watchdog..."
    sysctl kernel.nmi_watchdog=0
    echo "Done"
}

function reset_performance() {
	echo -n "Placing all CPUs in ondemand mode..."
	set_cpu_governor "ondemand"

	if [ -f "/sys/devices/system/cpu/intel_pstate/no_turbo" ]; then
		echo -n "Enabling Turbo Boost..."
		if write_sysfs_value "/sys/devices/system/cpu/intel_pstate/no_turbo" "0"; then
			echo "Done"
		else
			echo "Skipped (not permitted on this host)"
		fi
	fi

    echo -n "Enabling SMT..."
    echo on | sudo tee /sys/devices/system/cpu/smt/control
    echo "Done"

    echo -n "Enabling NMI watchdog..."
    sysctl kernel.nmi_watchdog=1
    echo "Done"
}

if [ "$1" = "reset" ]; then
	reset_performance
else
	configure_performance
fi
