/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <rte_config.h>
#include <rte_mempool.h>

#include "spdk/nvme.h"
#include "spdk/queue.h"

struct dev_ctx {
	TAILQ_ENTRY(dev_ctx)	tailq;
	bool			is_new;
	bool			is_removed;
	bool			is_draining;
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns	*ns;
	struct spdk_nvme_qpair	*qpair;
	uint32_t		io_size_blocks;
	uint64_t		size_in_ios;
	uint64_t		io_completed;
	uint64_t		prev_io_completed;
	uint64_t		current_queue_depth;
	uint64_t		offset_in_ios;
	char			name[1024];
};

struct perf_task {
	struct dev_ctx		*dev;
	void			*buf;
};

static struct rte_mempool *task_pool;

static TAILQ_HEAD(, dev_ctx) g_devs = TAILQ_HEAD_INITIALIZER(g_devs);

static uint64_t g_tsc_rate;

static uint32_t g_io_size_bytes = 4096;
static int g_queue_depth = 4;
static int g_time_in_sec;

static void
task_complete(struct perf_task *task);

static void
register_dev(struct spdk_nvme_ctrlr *ctrlr)
{
	struct dev_ctx *dev;
	const struct spdk_nvme_ctrlr_data *cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		perror("dev_ctx malloc");
		exit(1);
	}

	snprintf(dev->name, sizeof(dev->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);

	dev->ctrlr = ctrlr;
	dev->is_new = true;
	dev->is_removed = false;
	dev->is_draining = false;

	dev->ns = spdk_nvme_ctrlr_get_ns(ctrlr, 1);

	if (!dev->ns || !spdk_nvme_ns_is_active(dev->ns)) {
		printf("Controller %s: No active namespace; skipping\n", dev->name);
		goto skip;
	}

	if (spdk_nvme_ns_get_size(dev->ns) < g_io_size_bytes ||
	    spdk_nvme_ns_get_sector_size(dev->ns) > g_io_size_bytes) {
		printf("Controller %s: Invalid "
		       "ns size %" PRIu64 " / block size %u for I/O size %u\n",
		       dev->name,
		       spdk_nvme_ns_get_size(dev->ns),
		       spdk_nvme_ns_get_sector_size(dev->ns),
		       g_io_size_bytes);
		goto skip;
	}

	dev->size_in_ios = spdk_nvme_ns_get_size(dev->ns) / g_io_size_bytes;
	dev->io_size_blocks = g_io_size_bytes / spdk_nvme_ns_get_sector_size(dev->ns);

	dev->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, 0);
	if (!dev->qpair) {
		printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
		goto skip;
	}

	TAILQ_INSERT_TAIL(&g_devs, dev, tailq);
	return;

skip:
	free(dev);
}

static void
unregister_dev(struct dev_ctx *dev)
{
	printf("unregister_dev: %s\n", dev->name);

	spdk_nvme_ctrlr_free_io_qpair(dev->qpair);
	spdk_nvme_detach(dev->ctrlr);

	TAILQ_REMOVE(&g_devs, dev, tailq);
	free(dev);
}

static void task_ctor(struct rte_mempool *mp, void *arg, void *__task, unsigned id)
{
	struct perf_task *task = __task;
	task->buf = spdk_zmalloc(g_io_size_bytes, 0x200, NULL);
	if (task->buf == NULL) {
		fprintf(stderr, "task->buf rte_malloc failed\n");
		exit(1);
	}
	memset(task->buf, id % 8, g_io_size_bytes);
}

static void io_complete(void *ctx, const struct spdk_nvme_cpl *completion);

static void
submit_single_io(struct dev_ctx *dev)
{
	struct perf_task	*task = NULL;
	uint64_t		offset_in_ios;
	int			rc;

	if (rte_mempool_get(task_pool, (void **)&task) != 0) {
		fprintf(stderr, "task_pool rte_mempool_get failed\n");
		exit(1);
	}

	task->dev = dev;

	offset_in_ios = dev->offset_in_ios++;
	if (dev->offset_in_ios == dev->size_in_ios) {
		dev->offset_in_ios = 0;
	}

	rc = spdk_nvme_ns_cmd_read(dev->ns, dev->qpair, task->buf,
				   offset_in_ios * dev->io_size_blocks,
				   dev->io_size_blocks, io_complete, task, 0);

	if (rc != 0) {
		fprintf(stderr, "starting I/O failed\n");
		rte_mempool_put(task_pool, task);
	} else {
		dev->current_queue_depth++;
	}
}

static void
task_complete(struct perf_task *task)
{
	struct dev_ctx *dev;

	dev = task->dev;
	dev->current_queue_depth--;
	dev->io_completed++;

	rte_mempool_put(task_pool, task);

	/*
	 * is_draining indicates when time has expired for the test run
	 * and we are just waiting for the previously submitted I/O
	 * to complete.  In this case, do not submit a new I/O to replace
	 * the one just completed.
	 */
	if (!dev->is_draining && !dev->is_removed) {
		submit_single_io(dev);
	}
}

static void
io_complete(void *ctx, const struct spdk_nvme_cpl *completion)
{
	task_complete((struct perf_task *)ctx);
}

static void
check_io(struct dev_ctx *dev)
{
	spdk_nvme_qpair_process_completions(dev->qpair, 0);
}

static void
submit_io(struct dev_ctx *dev, int queue_depth)
{
	while (queue_depth-- > 0) {
		submit_single_io(dev);
	}
}

static void
drain_io(struct dev_ctx *dev)
{
	dev->is_draining = true;
	while (dev->current_queue_depth > 0) {
		check_io(dev);
	}
}

static void
print_stats(void)
{
	struct dev_ctx *dev;

	TAILQ_FOREACH(dev, &g_devs, tailq) {
		printf("%-43.43s: %10" PRIu64 " I/Os completed (+%" PRIu64 ")\n",
		       dev->name,
		       dev->io_completed,
		       dev->io_completed - dev->prev_io_completed);
		dev->prev_io_completed = dev->io_completed;
	}

	printf("\n");
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_probe_info *probe_info,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attaching to %04x:%02x:%02x.%02x\n",
	       probe_info->pci_addr.domain,
	       probe_info->pci_addr.bus,
	       probe_info->pci_addr.dev,
	       probe_info->pci_addr.func);

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_probe_info *probe_info,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attached to %04x:%02x:%02x.%02x\n",
	       probe_info->pci_addr.domain,
	       probe_info->pci_addr.bus,
	       probe_info->pci_addr.dev,
	       probe_info->pci_addr.func);

	register_dev(ctrlr);
}

static void
remove_cb(void *cb_ctx, struct spdk_nvme_ctrlr *ctrlr)
{
	struct dev_ctx *dev;

	TAILQ_FOREACH(dev, &g_devs, tailq) {
		if (dev->ctrlr == ctrlr) {
			/*
			 * Mark the device as removed, but don't detach yet.
			 *
			 * The I/O handling code will detach once it sees that
			 * is_removed is true and all outstanding I/O have been completed.
			 */
			dev->is_removed = true;
			printf("Controller removed: %s\n", dev->name);
			return;
		}
	}

	/*
	 * If we get here, this remove_cb is for a controller that we are not tracking
	 * in g_devs (for example, because we skipped it during register_dev),
	 * so immediately detach it.
	 */
	spdk_nvme_detach(ctrlr);
}

static int
io_loop(void)
{
	struct dev_ctx *dev, *dev_tmp;
	uint64_t tsc_end;
	uint64_t next_stats_tsc;

	tsc_end = spdk_get_ticks() + g_time_in_sec * g_tsc_rate;
	next_stats_tsc = spdk_get_ticks();

	while (1) {
		uint64_t now;

		/*
		 * Check for completed I/O for each controller. A new
		 * I/O will be submitted in the io_complete callback
		 * to replace each I/O that is completed.
		 */
		TAILQ_FOREACH(dev, &g_devs, tailq) {
			if (dev->is_new) {
				/* Submit initial I/O for this controller. */
				submit_io(dev, g_queue_depth);
				dev->is_new = false;
			}

			check_io(dev);
		}

		/*
		 * Check for hotplug events.
		 */
		if (spdk_nvme_probe(NULL, probe_cb, attach_cb, remove_cb) != 0) {
			fprintf(stderr, "spdk_nvme_probe() failed\n");
			break;
		}

		/*
		 * Check for devices which were hot-removed and have finished
		 * processing outstanding I/Os.
		 *
		 * unregister_dev() may remove devs from the list, so use the
		 * removal-safe iterator.
		 */
		TAILQ_FOREACH_SAFE(dev, &g_devs, tailq, dev_tmp) {
			if (dev->is_removed && dev->current_queue_depth == 0) {
				unregister_dev(dev);
			}
		}

		now = spdk_get_ticks();
		if (now > tsc_end) {
			break;
		}
		if (now > next_stats_tsc) {
			print_stats();
			next_stats_tsc += g_tsc_rate;
		}
	}

	TAILQ_FOREACH_SAFE(dev, &g_devs, tailq, dev_tmp) {
		drain_io(dev);
		unregister_dev(dev);
	}

	return 0;
}

static void usage(char *program_name)
{
	printf("%s options", program_name);
	printf("\n");
	printf("\t[-t time in seconds]\n");
}

static int
parse_args(int argc, char **argv)
{
	int op;

	/* default value*/
	g_time_in_sec = 0;

	while ((op = getopt(argc, argv, "t:")) != -1) {
		switch (op) {
		case 't':
			g_time_in_sec = atoi(optarg);
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!g_time_in_sec) {
		usage(argv[0]);
		return 1;
	}

	optind = 1;
	return 0;
}


static int
register_controllers(void)
{
	printf("Initializing NVMe Controllers\n");

	if (spdk_nvme_probe(NULL, probe_cb, attach_cb, remove_cb) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	return 0;
}

static char *ealargs[] = {
	"hotplug",
	"-c 0x1",
	"-n 4",
};

int main(int argc, char **argv)
{
	int rc;

	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	rc = rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]), ealargs);
	if (rc < 0) {
		fprintf(stderr, "could not initialize dpdk\n");
		return 1;
	}

	task_pool = rte_mempool_create("task_pool", 8192,
				       sizeof(struct perf_task),
				       64, 0, NULL, NULL, task_ctor, NULL,
				       SOCKET_ID_ANY, 0);

	g_tsc_rate = spdk_get_ticks_hz();

	/* Detect the controllers that are plugged in at startup. */
	if (register_controllers() != 0) {
		return 1;
	}

	printf("Initialization complete. Starting I/O...\n");
	rc = io_loop();

	return rc;
}
