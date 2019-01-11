#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <stddef.h>
#include <signal.h>
#include <inttypes.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>

#include "../arch/arch.h"
#include "../lib/types.h"
#include "../os/io_uring.h"

#define barrier()	__asm__ __volatile__("": : :"memory")

#define min(a, b)		((a < b) ? (a) : (b))

struct io_sq_ring {
	unsigned *head;
	unsigned *tail;
	unsigned *ring_mask;
	unsigned *ring_entries;
	unsigned *array;
};

struct io_cq_ring {
	unsigned *head;
	unsigned *tail;
	unsigned *ring_mask;
	unsigned *ring_entries;
	struct io_uring_cqe *cqes;
};

#define DEPTH			32

#define BATCH_SUBMIT		8
#define BATCH_COMPLETE		8

#define BS			4096

#define MAX_FDS			16

static unsigned sq_ring_mask, cq_ring_mask;

struct file {
	unsigned long max_blocks;
	int fd;
};

struct submitter {
	pthread_t thread;
	int ring_fd;
	struct drand48_data rand;
	struct io_sq_ring sq_ring;
	struct io_uring_sqe *sqes;
	struct iovec iovecs[DEPTH];
	struct io_cq_ring cq_ring;
	int inflight;
	unsigned long reaps;
	unsigned long done;
	unsigned long calls;
	unsigned long cachehit, cachemiss;
	volatile int finish;
	struct file files[MAX_FDS];
	unsigned nr_files;
	unsigned cur_file;
};

static struct submitter submitters[1];
static volatile int finish;

static int polled = 1;		/* use IO polling */
static int fixedbufs = 0;	/* use fixed user buffers */
static int buffered = 0;	/* use buffered IO, not O_DIRECT */
static int sq_thread_poll = 0;	/* use kernel submission/poller thread */
static int sq_thread_cpu = -1;	/* pin above thread to this CPU */

static int io_uring_register_buffers(struct submitter *s)
{
	struct io_uring_register_buffers reg = {
		.iovecs = s->iovecs,
		.nr_iovecs = DEPTH
	};

	return syscall(__NR_sys_io_uring_register, s->ring_fd,
			IORING_REGISTER_BUFFERS, &reg);
}

static int io_uring_setup(unsigned entries, struct io_uring_params *p)
{
	return syscall(__NR_sys_io_uring_setup, entries, p);
}

static int io_uring_enter(struct submitter *s, unsigned int to_submit,
			  unsigned int min_complete, unsigned int flags)
{
	return syscall(__NR_sys_io_uring_enter, s->ring_fd, to_submit,
			min_complete, flags);
}

static int gettid(void)
{
	return syscall(__NR_gettid);
}

static void init_io(struct submitter *s, unsigned index)
{
	struct io_uring_sqe *sqe = &s->sqes[index];
	unsigned long offset;
	struct file *f;
	long r;

	f = &s->files[s->cur_file];
	s->cur_file++;
	if (s->cur_file == s->nr_files)
		s->cur_file = 0;

	lrand48_r(&s->rand, &r);
	offset = (r % (f->max_blocks - 1)) * BS;

	sqe->flags = 0;
	sqe->opcode = IORING_OP_READV;
	if (fixedbufs) {
		sqe->addr = s->iovecs[index].iov_base;
		sqe->len = BS;
		sqe->buf_index = index;
		sqe->flags |= IOSQE_FIXED_BUFFER;
	} else {
		sqe->addr = &s->iovecs[index];
		sqe->len = 1;
		sqe->buf_index = 0;
	}
	sqe->ioprio = 0;
	sqe->fd = f->fd;
	sqe->off = offset;
}

static int prep_more_ios(struct submitter *s, int max_ios)
{
	struct io_sq_ring *ring = &s->sq_ring;
	unsigned index, tail, next_tail, prepped = 0;

	next_tail = tail = *ring->tail;
	do {
		next_tail++;
		barrier();
		if (next_tail == *ring->head)
			break;

		index = tail & sq_ring_mask;
		init_io(s, index);
		ring->array[index] = index;
		prepped++;
		tail = next_tail;
	} while (prepped < max_ios);

	if (*ring->tail != tail) {
		/* order tail store with writes to sqes above */
		barrier();
		*ring->tail = tail;
		barrier();
	}
	return prepped;
}

static int get_file_size(struct file *f)
{
	struct stat st;

	if (fstat(f->fd, &st) < 0)
		return -1;
	if (S_ISBLK(st.st_mode)) {
		unsigned long long bytes;

		if (ioctl(f->fd, BLKGETSIZE64, &bytes) != 0)
			return -1;

		f->max_blocks = bytes / BS;
		return 0;
	} else if (S_ISREG(st.st_mode)) {
		f->max_blocks = st.st_size / BS;
		return 0;
	}

	return -1;
}

static int reap_events(struct submitter *s)
{
	struct io_cq_ring *ring = &s->cq_ring;
	struct io_uring_cqe *cqe;
	unsigned head, reaped = 0;

	head = *ring->head;
	do {
		barrier();
		if (head == *ring->tail)
			break;
		cqe = &ring->cqes[head & cq_ring_mask];
		if (cqe->res != BS) {
			printf("io: unexpected ret=%d\n", cqe->res);
			return -1;
		}
		if (cqe->flags & IOCQE_FLAG_CACHEHIT)
			s->cachehit++;
		else
			s->cachemiss++;
		reaped++;
		head++;
	} while (1);

	s->inflight -= reaped;
	*ring->head = head;
	barrier();
	return reaped;
}

static void *submitter_fn(void *data)
{
	struct submitter *s = data;
	int ret, prepped;

	printf("submitter=%d\n", gettid());

	srand48_r(pthread_self(), &s->rand);

	prepped = 0;
	do {
		int to_wait, to_submit, this_reap, to_prep;

		if (!prepped && s->inflight < DEPTH) {
			to_prep = min(DEPTH - s->inflight, BATCH_SUBMIT);
			prepped = prep_more_ios(s, to_prep);
		}
		s->inflight += prepped;
submit_more:
		to_submit = prepped;
submit:
		if (s->inflight + BATCH_SUBMIT < DEPTH)
			to_wait = 0;
		else
			to_wait = min(s->inflight + to_submit, BATCH_COMPLETE);

		ret = io_uring_enter(s, to_submit, to_wait,
					IORING_ENTER_GETEVENTS);
		s->calls++;

		this_reap = reap_events(s);
		if (this_reap == -1)
			break;
		s->reaps += this_reap;

		if (ret >= 0) {
			if (!ret) {
				to_submit = 0;
				if (s->inflight)
					goto submit;
				continue;
			} else if (ret < to_submit) {
				int diff = to_submit - ret;

				s->done += ret;
				prepped -= diff;
				goto submit_more;
			}
			s->done += ret;
			prepped = 0;
			continue;
		} else if (ret < 0) {
			if (errno == EAGAIN) {
				if (s->finish)
					break;
				if (this_reap)
					goto submit;
				to_submit = 0;
				goto submit;
			}
			printf("io_submit: %s\n", strerror(errno));
			break;
		}
	} while (!s->finish);

	finish = 1;
	return NULL;
}

static void sig_int(int sig)
{
	printf("Exiting on signal %d\n", sig);
	submitters[0].finish = 1;
	finish = 1;
}

static void arm_sig_int(void)
{
	struct sigaction act;

	memset(&act, 0, sizeof(act));
	act.sa_handler = sig_int;
	act.sa_flags = SA_RESTART;
	sigaction(SIGINT, &act, NULL);
}

static int setup_ring(struct submitter *s)
{
	struct io_sq_ring *sring = &s->sq_ring;
	struct io_cq_ring *cring = &s->cq_ring;
	struct io_uring_params p;
	int ret, fd;
	void *ptr;

	memset(&p, 0, sizeof(p));

	if (polled)
		p.flags |= IORING_SETUP_IOPOLL;
	if (sq_thread_poll) {
		p.flags |= IORING_SETUP_SQPOLL;
		if (sq_thread_cpu != -1)
			p.flags |= IORING_SETUP_SQ_AFF;
	}

	fd = io_uring_setup(DEPTH, &p);
	if (fd < 0) {
		perror("io_uring_setup");
		return 1;
	}
	s->ring_fd = fd;

	if (fixedbufs) {
		ret = io_uring_register_buffers(s);
		if (ret < 0) {
			perror("io_uring_register");
			return 1;
		}
	}

	ptr = mmap(0, p.sq_off.array + p.sq_entries * sizeof(__u32),
			PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd,
			IORING_OFF_SQ_RING);
	printf("sq_ring ptr = 0x%p\n", ptr);
	sring->head = ptr + p.sq_off.head;
	sring->tail = ptr + p.sq_off.tail;
	sring->ring_mask = ptr + p.sq_off.ring_mask;
	sring->ring_entries = ptr + p.sq_off.ring_entries;
	sring->array = ptr + p.sq_off.array;
	sq_ring_mask = *sring->ring_mask;

	s->sqes = mmap(0, p.sq_entries * sizeof(struct io_uring_sqe),
			PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd,
			IORING_OFF_SQES);
	printf("sqes ptr    = 0x%p\n", s->sqes);

	ptr = mmap(0, p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe),
			PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd,
			IORING_OFF_CQ_RING);
	printf("cq_ring ptr = 0x%p\n", ptr);
	cring->head = ptr + p.cq_off.head;
	cring->tail = ptr + p.cq_off.tail;
	cring->ring_mask = ptr + p.cq_off.ring_mask;
	cring->ring_entries = ptr + p.cq_off.ring_entries;
	cring->cqes = ptr + p.cq_off.cqes;
	cq_ring_mask = *cring->ring_mask;
	return 0;
}

int main(int argc, char *argv[])
{
	struct submitter *s = &submitters[0];
	unsigned long done, calls, reap, cache_hit, cache_miss;
	int err, i, flags, fd;
	struct rlimit rlim;
	void *ret;

	if (argc < 2) {
		printf("%s: filename\n", argv[0]);
		return 1;
	}

	flags = O_RDONLY;
	if (!buffered)
		flags |= O_DIRECT;

	i = 1;
	while (i < argc) {
		struct file *f = &s->files[s->nr_files];

		fd = open(argv[i], flags);
		if (fd < 0) {
			perror("open");
			return 1;
		}
		f->fd = fd;
		if (get_file_size(f)) {
			printf("failed getting size of device/file\n");
			return 1;
		}
		if (f->max_blocks <= 1) {
			printf("Zero file/device size?\n");
			return 1;
		}
		f->max_blocks--;

		printf("Added file %s\n", argv[i]);
		s->nr_files++;
		i++;
	}

	rlim.rlim_cur = RLIM_INFINITY;
	rlim.rlim_max = RLIM_INFINITY;
	if (setrlimit(RLIMIT_MEMLOCK, &rlim) < 0) {
		perror("setrlimit");
		return 1;
	}

	arm_sig_int();

	for (i = 0; i < DEPTH; i++) {
		void *buf;

		if (posix_memalign(&buf, BS, BS)) {
			printf("failed alloc\n");
			return 1;
		}
		s->iovecs[i].iov_base = buf;
		s->iovecs[i].iov_len = BS;
	}

	err = setup_ring(s);
	if (err) {
		printf("ring setup failed: %s, %d\n", strerror(errno), err);
		return 1;
	}
	printf("polled=%d, fixedbufs=%d, buffered=%d", polled, fixedbufs, buffered);
	printf(" QD=%d, sq_ring=%d, cq_ring=%d\n", DEPTH, *s->sq_ring.ring_entries, *s->cq_ring.ring_entries);

	pthread_create(&s->thread, NULL, submitter_fn, s);

	cache_hit = cache_miss = reap = calls = done = 0;
	do {
		unsigned long this_done = 0;
		unsigned long this_reap = 0;
		unsigned long this_call = 0;
		unsigned long this_cache_hit = 0;
		unsigned long this_cache_miss = 0;
		unsigned long rpc = 0, ipc = 0;
		double hit = 0.0;

		sleep(1);
		this_done += s->done;
		this_call += s->calls;
		this_reap += s->reaps;
		this_cache_hit += s->cachehit;
		this_cache_miss += s->cachemiss;
		if (this_cache_hit && this_cache_miss) {
			unsigned long hits, total;

			hits = this_cache_hit - cache_hit;
			total = hits + this_cache_miss - cache_miss;
			hit = (double) hits / (double) total;
			hit *= 100.0;
		}
		if (this_call - calls) {
			rpc = (this_done - done) / (this_call - calls);
			ipc = (this_reap - reap) / (this_call - calls);
		}
		printf("IOPS=%lu, IOS/call=%lu/%lu, inflight=%u (head=%u tail=%u), Cachehit=%0.2f%%\n",
				this_done - done, rpc, ipc, s->inflight,
				*s->cq_ring.head, *s->cq_ring.tail, hit);
		done = this_done;
		calls = this_call;
		reap = this_reap;
		cache_hit = s->cachehit;
		cache_miss = s->cachemiss;
	} while (!finish);

	pthread_join(s->thread, &ret);
	close(s->ring_fd);
	return 0;
}