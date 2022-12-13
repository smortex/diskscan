/*
 *  Copyright 2013 Baruch Even <baruch@ev-en.org>
 *
 *  This file is part of DiskScan.
 *
 *  DiskScan is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *  DiskScan is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with DiskScan.  If not, see <http://www.gnu.org/licenses/>.
 * 
 */

#include "verbose.h"
#include "diskscan.h"
#include "compiler.h"
#include "cli.h"

#include "progressbar/include/progressbar.h"
#include "hdrhistogram/src/hdr_histogram.h"

#include <stdio.h>
#include <signal.h>
#include <inttypes.h>
#include <unistd.h>
#include <getopt.h>
#include <memory.h>
#include <stdlib.h>
#include <errno.h>

static disk_t disk;
static progressbar *bar;

typedef struct options_t options_t;
struct options_t {
	char *disk_path;
	int verbose;
	int fix;
	enum scan_mode mode;
	unsigned scan_size;
	char *data_log_name;
	char *data_log_raw_name;
	disk_mount_e allowed_mount;
	uint64_t start_sector;
	uint64_t end_sector;
};

static void print_header(void)
{
	printf("diskscan version %s\n\n", VERSION);
	VERBOSE("Verbosity set");
	VVERBOSE("High verbosity set");
	VVVERBOSE("Very high verbosity set");
}

static int usage(void) {
	printf("diskscan version %s\n\n", VERSION);
	printf("diskscan [options] /dev/sd\n");
	printf("Options:\n");
	printf("    -v, --verbose        - Increase verbosity, multiple uses for higher levels\n");
	printf("    -f, --fix            - Attempt to fix near failures, nothing can be done for unreadable sectors\n");
	printf("    -s, --scan <mode>    - Scan in order (seq, random)\n");
	printf("    -e, --size <size>    - Scan size (default to 64K, must be multiple of 512)\n");
	printf("    -o, --output <file>  - Output file (json)\n");
	printf("    -r, --raw-log <file> - Raw log of all scan results (json)\n");
	printf("    -S, --start-sector <sector> - Start scan at sector\n");
	printf("    -E, --end-sector <sector>   - Stop scan at sector\n");
	printf("    --force-mounted      - Allow checking a read-only mounted disk\n");
	printf("    --force-mounted-rw   - Allow checking a read-write mounted disk\n");
	printf("\n");
	return 1;
}

void report_progress(disk_t * UNUSED(disk), int progress_part, int progress_full)
{
	if (bar == NULL)
		bar = progressbar_new("Disk scan", progress_full);
	progressbar_update(bar, progress_part);
}

void report_scan_success(disk_t *UNUSED(disk), uint64_t UNUSED(offset_bytes), uint64_t UNUSED(data_size), uint64_t UNUSED(time))
{
}

void report_scan_error(disk_t *UNUSED(disk), uint64_t UNUSED(offset_bytes), uint64_t UNUSED(data_size), uint64_t UNUSED(time))
{
}

static void print_latency(latency_t *latency_graph, unsigned latency_graph_len)
{
	unsigned i;

	const uint32_t height = 30; // number of lines to fill
	const uint32_t min_val = 0;
	uint32_t max_val = 1;

	for (i = 0; i < latency_graph_len; i++) {
		if (max_val < latency_graph[i].latency_max_msec)
			max_val = latency_graph[i].latency_max_msec;
	}

	uint32_t height_interval = (max_val - min_val + 1) / (height - 3);
	if (height_interval == 0)
		height_interval = 1;
	else if (height_interval > 10000)
		height_interval = 10000;

	uint32_t j;
	for (j = height; j > 0; j--) {
		if (j % 5 == 0)
			printf("%5u | ", j * height_interval);
		else
			printf("      | ");

		for (i = 0; i < latency_graph_len; i++) {
			uint32_t max_height = latency_graph[i].latency_max_msec / height_interval + 1;
			uint32_t med_height = latency_graph[i].latency_median_msec / height_interval + 1;
			uint32_t min_height = latency_graph[i].latency_min_msec / height_interval + 1;

			if (max_height == med_height) {
				max_height++;
			}
			if (med_height == min_height) {
				med_height++;
				if (max_height == med_height)
					max_height++;
			}

			if (max_height != j && med_height != j && min_height != j) {
				printf(" ");
				continue;
			}

			if (max_height == j)
				printf("^");
			else if (med_height == j)
				printf("*");
			else
				printf("_");
		}
		printf("\n");
	}
	printf("      +-");
	for (i = 0; i < latency_graph_len; i++) {
		printf("-");
	}
	printf("\n");

}

void report_scan_done(disk_t *pdisk)
{
	progressbar_finish(bar);

	printf("\nAccess time histogram:\n");
	hdr_percentiles_print(pdisk->histogram, stdout, 5, 1000.0, CLASSIC); // Print msecs

	printf("\nLatency graph:\n");
	print_latency(pdisk->latency_graph, pdisk->latency_graph_len);

	printf("\nConclusion: %s\n", conclusion_to_str(pdisk->conclusion));
}

static unsigned str_to_scan_size(const char *str)
{
	char *endptr;
	long int val;

	errno = 0;
	val = strtol(str, &endptr, 0);
	if (errno != 0 || val <= 0) {
		ERROR("Failed to parse the value (%s) to a number", str);
		return 0;
	}

	if (*endptr != 0) {
		unsigned factor = 1;

		if (strcmp(endptr, "b") == 0 || strcmp(endptr, "B") == 0)
			factor = 1;
		else if (strcmp(endptr, "k") == 0 || strcmp(endptr, "K") == 0)
			factor = 1024;
		else if (strcmp(endptr, "m") == 0 || strcmp(endptr, "M") == 0)
			factor = 1024*1024;
		else {
			ERROR("Unknown suffix '%s': B, K, and M are accepted", endptr);
			return 0;
		}

		val *= factor;
	}

	unsigned retval = (unsigned)val;
	if (retval > 32*1024*1024) {
		ERROR("Maximum transfer size is 32MB, cannot handle more than that for now.");
		return 0;
	}

	return (unsigned)val;
}

static int parse_args(int argc, char **argv, options_t *opts)
{
	int c;
	int unknown = 0;
	static int allowed_mount = DISK_NOT_MOUNTED;

	opts->scan_size = 64*1024;

	while (1) {
		int option_index = 0;
		static struct option long_options[] = {
			{"verbose", no_argument,       0,  'v'},
			{"fix",     no_argument,       0,  'f'},
			{"scan",    required_argument, 0,  's'},
			{"size",    required_argument, 0,  'e'},
			{"raw-log", required_argument, 0,  'r'},
			{"output",  required_argument, 0,  'o'},
			{"start-sector", required_argument, 0, 'S'},
			{"end-sector",   required_argument, 0, 'E'},
			{"force-mounted", no_argument, &allowed_mount, DISK_MOUNTED_RO},
			{"force-mounted-rw", no_argument, &allowed_mount, DISK_MOUNTED_RW},
			{0,         0,                 0,  0}
		};

		c = getopt_long(argc, argv, "vfs:e:o:r:S:E:", long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
			case 0:
				break;

			case 'v':
				opts->verbose++;
				break;
			case 'f':
				opts->fix = 1;
				break;
			case 's':
				opts->mode = str_to_scan_mode(optarg);
				if (opts->mode == SCAN_MODE_UNKNOWN) {
					opts->mode = SCAN_MODE_SEQ;
					printf("Unknown scan mode %s given, using sequential\n", optarg);
				}
				break;
			case 'e':
				opts->scan_size = str_to_scan_size(optarg);
				break;

			case 'o':
				opts->data_log_name = optarg;
				break;
			case 'r':
				opts->data_log_raw_name = optarg;
				break;
			case 'S':
				opts->start_sector = atoll(optarg);
				break;
			case 'E':
				opts->end_sector = atoll(optarg);
				break;

			default:
				unknown = 1;
				break;
		}
	}

	if (optind == argc) {
		printf("No disk path provided to scan!\n");
		return usage();
	}
	if (optind < argc - 1) {
		printf("Too many disk paths provided to scan, can only scan one disk!\n");
		return usage();
	}

	if (unknown) {
		printf("Unknown option provided\n");
		return usage();
	}

	if (opts->scan_size == 0) {
		printf("Scan size is invalid, must be a positive number\n");
		return usage();
	}

	opts->disk_path = argv[optind];
	opts->allowed_mount = allowed_mount;
	return 0;
}

/*
static int print_disk_info(disk_t *UNUSED(disk))
{
	return 0;
}
*/

static void diskscan_cli_signal(int UNUSED(signal))
{
	disk_scan_stop(&disk);
}

static void setup_signals(void)
{
	struct sigaction act = {
		.sa_handler = diskscan_cli_signal,
		.sa_flags = SA_RESTART,
	};

	sigaction(SIGINT, &act, NULL);
	sigaction(SIGTERM, &act, NULL);
}

int diskscan_cli(int argc, char **argv)
{
	int ret;
	options_t opts;

	memset(&opts, 0, sizeof(opts));
	opts.mode = SCAN_MODE_SEQ;
	opts.allowed_mount = DISK_NOT_MOUNTED;

	if (parse_args(argc, argv, &opts))
		return 1;
	verbose = opts.verbose;

	print_header();

	setup_signals();

	if (disk_open(&disk, opts.disk_path, opts.fix, 70, opts.allowed_mount))
		return 1;

	/*
	if (print_disk_info(&disk))
		return 1;
	*/

	if (opts.data_log_raw_name)
		data_log_raw_start(&disk.data_raw, opts.data_log_raw_name, &disk);
	if (opts.data_log_name)
		data_log_start(&disk.data_log, opts.data_log_name, &disk);
	ret = 0;
	if (disk_scan(&disk, opts.mode, opts.scan_size, opts.start_sector, opts.end_sector))
		ret = 1;
	if (opts.data_log_raw_name)
		data_log_raw_end(&disk.data_raw);
	if (opts.data_log_name)
		data_log_end(&disk.data_log, &disk);

	disk_close(&disk);
	return ret;
}
