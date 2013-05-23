#include "../fio.h"
#include "../profile.h"
#include "../parse.h"

/*
 * 1x loads
 */
#define R_LOAD		2000
#define W_LOAD		1000

#define SAMPLE_SEC	3600		/* 1h checks */

struct act_pass_criteria {
	unsigned int max_usec;
	unsigned int max_perm;
};
#define ACT_MAX_CRIT	3

static struct act_pass_criteria act_pass[ACT_MAX_CRIT] = {
	{
		.max_usec =	1000,
		.max_perm =	50,
	},
	{
		.max_usec =	8000,
		.max_perm =	10,
	},
	{
		.max_usec = 	64000,
		.max_perm =	1,
	},
};

struct act_run_data {
	struct fio_mutex *mutex;
	unsigned int pending;

	uint64_t lat_buckets[ACT_MAX_CRIT];
	uint64_t total_ios;
};
static struct act_run_data *act_run_data;

struct act_prof_data {
	struct timeval sample_tv;
	uint64_t lat_buckets[ACT_MAX_CRIT];
	uint64_t total_ios;
	uint64_t cum_lat_buckets[ACT_MAX_CRIT];
	uint64_t cum_total_ios;
};

static char *device_names;
static unsigned int load = 1;
static unsigned int prep;
static unsigned int threads_per_queue;
static unsigned int num_read_blocks;
static unsigned int write_size;

#define ACT_MAX_OPTS	128
static const char *act_opts[ACT_MAX_OPTS] = {
	"direct=1",
	"ioengine=sync",
	"random_generator=lfsr",
	"group_reporting=1",
	"thread",
	NULL,
};
static unsigned int opt_idx = 5;
static unsigned int org_idx;

static int act_add_opt(const char *format, ...) __attribute__ ((__format__ (__printf__, 1, 2)));

static struct fio_option options[] = {
	{
		.name	= "device-names",
		.lname	= "device-names",
		.type	= FIO_OPT_STR_STORE,
		.roff1	= &device_names,
		.help	= "Devices to use",
		.category = FIO_OPT_C_PROFILE,
		.group	= FIO_OPT_G_ACT,
	},
	{
		.name	= "load",
		.lname	= "Load multiplier",
		.type	= FIO_OPT_INT,
		.roff1	= &load,
		.help	= "ACT load multipler (default 1x)",
		.def	= "1",
		.category = FIO_OPT_C_PROFILE,
		.group	= FIO_OPT_G_ACT,
	},
	{
		.name	= "threads-per-queue",
		.lname	= "Number of read IO threads per device",
		.type	= FIO_OPT_INT,
		.roff1	= &threads_per_queue,
		.help	= "Number of read IO threads per device",
		.def	= "8",
		.category = FIO_OPT_C_PROFILE,
		.group	= FIO_OPT_G_ACT,
	},
	{
		.name	= "read-req-num-512-blocks",
		.lname	= "Number of 512b blocks to read",
		.type	= FIO_OPT_INT,
		.roff1	= &num_read_blocks,
		.help	= "Number of 512b blocks to read at the time",
		.def	= "3",
		.category = FIO_OPT_C_PROFILE,
		.group	= FIO_OPT_G_ACT,
	},
	{
		.name	= "large-block-op-kbytes",
		.lname	= "Size of large block ops (writes)",
		.type	= FIO_OPT_INT,
		.roff1	= &write_size,
		.help	= "Size of large block ops (writes)",
		.def	= "128k",
		.category = FIO_OPT_C_PROFILE,
		.group	= FIO_OPT_G_ACT,
	},
	{
		.name	= "prep",
		.lname	= "Run ACT prep phase",
		.type	= FIO_OPT_STR_SET,
		.roff1	= &prep,
		.help	= "Set to run ACT prep phase",
		.category = FIO_OPT_C_PROFILE,
		.group	= FIO_OPT_G_ACT,
	},
	{
		.name	= NULL,
	},
};

static int act_add_opt(const char *str, ...)
{
	char buffer[512];
	va_list args;
	size_t len;

	if (opt_idx == ACT_MAX_OPTS) {
		log_err("act: ACT_MAX_OPTS is too small\n");
		return 1;
	}

	va_start(args, str);
	len = vsnprintf(buffer, sizeof(buffer), str, args);
	va_end(args);

	if (len)
		act_opts[opt_idx++] = strdup(buffer);

	return 0;
}

static int act_add_rw(const char *dev, int reads)
{
	if (act_add_opt("name=act-%s-%s", reads ? "read" : "write", dev))
		return 1;
	if (act_add_opt("filename=%s", dev))
		return 1;
	if (act_add_opt("rw=%s", reads ? "randread" : "randwrite"))
		return 1;
	if (reads) {
		int rload = load * R_LOAD / threads_per_queue;

		if (act_add_opt("numjobs=%u", threads_per_queue))
			return 1;
		if (act_add_opt("rate_iops=%u", rload))
			return 1;
		if (act_add_opt("bs=%u", num_read_blocks * 512))
			return 1;
	} else {
		const int rsize = write_size / (num_read_blocks * 512);
		int wload = (load * W_LOAD + rsize - 1) / rsize;

		if (act_add_opt("rate_iops=%u", wload))
			return 1;
		if (act_add_opt("bs=%u", write_size))
			return 1;
	}

	return 0;
}

static int act_add_dev_prep(const char *dev)
{
	/* Add sequential zero phase */
	if (act_add_opt("name=act-prep-zeroes-%s", dev))
		return 1;
	if (act_add_opt("filename=%s", dev))
		return 1;
	if (act_add_opt("bs=1M"))
		return 1;
	if (act_add_opt("zero_buffers"))
		return 1;
	if (act_add_opt("rw=write"))
		return 1;

	/* Randomly overwrite device */
	if (act_add_opt("name=act-prep-salt-%s", dev))
		return 1;
	if (act_add_opt("stonewall"))
		return 1;
	if (act_add_opt("filename=%s", dev))
		return 1;
	if (act_add_opt("bs=4k"))
		return 1;
	if (act_add_opt("ioengine=libaio"))
		return 1;
	if (act_add_opt("iodepth=64"))
		return 1;
	if (act_add_opt("rw=randwrite"))
		return 1;

	return 0;
}

static int act_add_dev(const char *dev)
{
	if (prep)
		return act_add_dev_prep(dev);

	if (act_add_opt("runtime=24h"))
		return 1;
	if (act_add_opt("time_based=1"))
		return 1;

	if (act_add_rw(dev, 1))
		return 1;
	if (act_add_rw(dev, 0))
		return 1;

	return 0;
}

/*
 * Fill our private options into the command line
 */
static int act_prep_cmdline(void)
{
	if (!device_names) {
		log_err("act: need device-names\n");
		return 1;
	}

	org_idx = opt_idx;

	do {
		char *dev;

		dev = strsep(&device_names, ",");
		if (!dev)
			break;

		if (act_add_dev(dev)) {
			log_err("act: failed adding device to the mix\n");
			break;
		}
	} while (1);

	return 0;
}

static int act_io_u_lat(struct thread_data *td, uint64_t usec)
{
	struct act_prof_data *apd = td->prof_data;
	int i, ret = 0;
	double perm;

	if (prep)
		return 0;

	apd->total_ios++;

	for (i = ACT_MAX_CRIT - 1; i >= 0; i--) {
		if (usec > act_pass[i].max_usec) {
			apd->lat_buckets[i]++;
			break;
		}
	}

	if (time_since_now(&apd->sample_tv) < SAMPLE_SEC)
		return 0;

	/* SAMPLE_SEC has passed, check criteria for pass */
	for (i = 0; i < ACT_MAX_CRIT; i++) {
		perm = (1000.0 * apd->lat_buckets[i]) / apd->total_ios;
		if (perm < act_pass[i].max_perm)
			continue;

		log_err("act: %f%% exceeds pass criteria of %f%%\n", perm / 10.0, (double) act_pass[i].max_perm / 10.0);
		ret = 1;
		break;
	}

	for (i = 0; i < ACT_MAX_CRIT; i++) {
		apd->cum_lat_buckets[i] += apd->lat_buckets[i];
		apd->lat_buckets[i] = 0;
	}

	apd->cum_total_ios += apd->total_ios;
	apd->total_ios = 0;

	fio_gettime(&apd->sample_tv, NULL);
	return ret;
}

static void get_act_ref(void)
{
	fio_mutex_down(act_run_data->mutex);
	act_run_data->pending++;
	fio_mutex_up(act_run_data->mutex);
}

static void act_show_all_stats(void)
{
	unsigned int i;

	log_info("         trans                  device\n");
	log_info("         %%>(ms)                 %%>(ms)\n");
	log_info(" slice");

	for (i = 0; i < ACT_MAX_CRIT; i++)
		log_info("\t%2u", act_pass[i].max_usec / 1000);
	for (i = 0; i < ACT_MAX_CRIT; i++)
		log_info("\t%2u", act_pass[i].max_usec / 1000);

	log_info("\n");
	log_info(" -----   ------ ------ ------   ------ ------ ------\n");
	log_info("     1");

	for (i = 0; i < ACT_MAX_CRIT; i++) {
		double perc;

		perc = 100.0 * (double) act_run_data->lat_buckets[i] / (double) act_run_data->total_ios;
		log_info("\t%2.2f", perc);
	}
	for (i = 0; i < ACT_MAX_CRIT; i++) {
		double perc;

		perc = 100.0 * (double) act_run_data->lat_buckets[i] / (double) act_run_data->total_ios;
		log_info("\t%2.2f", perc);
	}

}

static void put_act_ref(struct thread_data *td)
{
	struct act_prof_data *apd = td->prof_data;
	unsigned int i;

	fio_mutex_down(act_run_data->mutex);

	for (i = 0; i < ACT_MAX_CRIT; i++) {
		act_run_data->lat_buckets[i] += apd->cum_lat_buckets[i];
		act_run_data->lat_buckets[i] += apd->lat_buckets[i];
	}

	act_run_data->total_ios += apd->cum_total_ios + apd->total_ios;

	if (!--act_run_data->pending)
		act_show_all_stats();

	fio_mutex_up(act_run_data->mutex);
}

static int act_td_init(struct thread_data *td)
{
	struct act_prof_data *apd;

	get_act_ref();

	apd = calloc(sizeof(*apd), 1);
	fio_gettime(&apd->sample_tv, NULL);
	td->prof_data = apd;
	return 0;
}

static void act_td_exit(struct thread_data *td)
{
	put_act_ref(td);
	free(td->prof_data);
	td->prof_data = NULL;
}

static struct prof_io_ops act_io_ops = {
	.td_init	= act_td_init,
	.td_exit	= act_td_exit,
	.io_u_lat	= act_io_u_lat,
};

static struct profile_ops act_profile = {
	.name		= "act",
	.desc		= "ACT Aerospike like benchmark",
	.options	= options,
	.prep_cmd	= act_prep_cmdline,
	.cmdline	= act_opts,
	.io_ops		= &act_io_ops,
};

static void fio_init act_register(void)
{
	act_run_data = calloc(sizeof(*act_run_data), 1);
	act_run_data->mutex = fio_mutex_init(FIO_MUTEX_UNLOCKED);

	if (register_profile(&act_profile))
		log_err("fio: failed to register profile 'act'\n");
}

static void fio_exit act_unregister(void)
{
	while (org_idx && org_idx < opt_idx)
		free((void *) act_opts[++org_idx]);

	unregister_profile(&act_profile);
	fio_mutex_remove(act_run_data->mutex);
	free(act_run_data);
	act_run_data = NULL;
}