/* SPDX-License-Identifier: BSD-3-Clause */

/* Measurement core: counter setup, TSC checks, timer overhead. */

#define _GNU_SOURCE /* syscall(), sched_getcpu(), RUSAGE_THREAD */

#include "bm_core.h"

#include <cpuid.h>
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define SYSFS_PMU "/sys/bus/event_source/devices"

/* extended hardware event type for hybrid PMUs (PERF_PMU_TYPE_SHIFT) */
#define PMU_TYPE_SHIFT 32

const char *
bm_pmu_mode_name(enum bm_pmu_mode mode)
{
	switch (mode) {
	case BM_PMU_RDPMC:
		return "rdpmc";
	case BM_PMU_READ:
		return "read";
	case BM_PMU_NONE:
		break;
	}
	return "none";
}

static int
read_file(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");
	size_t n;

	if (f == NULL)
		return -1;
	n = fread(buf, 1, len - 1, f);
	fclose(f);
	buf[n] = '\0';
	buf[strcspn(buf, "\n")] = '\0';
	return 0;
}

/* is cpu in a cpulist such as "0-7,16,18-19" */
static int
cpu_in_list(const char *list, int cpu)
{
	const char *p = list;
	char *end;
	long lo, hi;

	while (*p != '\0') {
		lo = strtol(p, &end, 10);
		if (end == p)
			return 0;
		hi = lo;
		if (*end == '-')
			hi = strtol(end + 1, &end, 10);
		if (cpu >= lo && cpu <= hi)
			return 1;
		if (*end != ',')
			return 0;
		p = end + 1;
	}
	return 0;
}

/*
 * Pick the PMU for the worker core. On hybrid Intel CPUs the generic
 * cycles event must name the PMU of the core type (cpu_core or cpu_atom)
 * through the extended type in config. Returns that PMU type, or 0 for
 * the default PMU.
 */
static uint64_t
pmu_select(struct bm_pmu *pmu, int cpu)
{
	static const char * const types[] = {"core", "atom"};
	char path[128], buf[256];
	unsigned int i;
	int hybrid = 0;

	for (i = 0; i < 2; i++) {
		snprintf(path, sizeof(path), SYSFS_PMU "/cpu_%s/cpus", types[i]);
		if (read_file(path, buf, sizeof(buf)) != 0)
			continue;
		hybrid = 1;
		if (!cpu_in_list(buf, cpu))
			continue;
		snprintf(path, sizeof(path), SYSFS_PMU "/cpu_%s/type", types[i]);
		if (read_file(path, buf, sizeof(buf)) != 0)
			break;
		snprintf(pmu->name, sizeof(pmu->name), "cpu_%s", types[i]);
		snprintf(pmu->core_type, sizeof(pmu->core_type), "%s", types[i]);
		return strtoull(buf, NULL, 10);
	}

	snprintf(pmu->name, sizeof(pmu->name), "cpu");
	snprintf(pmu->core_type, sizeof(pmu->core_type), "%s", hybrid ? "unknown" : "uniform");
	return 0;
}

static int
event_open(uint64_t pmu_type, uint64_t config, int group_fd)
{
	struct perf_event_attr a;

	memset(&a, 0, sizeof(a));
	a.size = sizeof(a);
	a.type = PERF_TYPE_HARDWARE;
	a.config = config | pmu_type << PMU_TYPE_SHIFT;
	a.read_format = PERF_FORMAT_GROUP;
	/* only a group leader can be pinned; members follow it */
	a.pinned = group_fd == -1;
	a.exclude_kernel = 1;
	a.exclude_hv = 1;
	return syscall(SYS_perf_event_open, &a, 0, -1, group_fd, PERF_FLAG_FD_CLOEXEC);
}

void
bm_pmu_open(struct bm_pmu *pmu, enum bm_pmu_mode want, int cpu)
{
	static const uint64_t config[BM_CTR_MAX] = {
		[BM_CTR_CYCLES] = PERF_COUNT_HW_CPU_CYCLES,
		[BM_CTR_INSTR] = PERF_COUNT_HW_INSTRUCTIONS,
	};
	long page = sysconf(_SC_PAGESIZE);
	uint64_t type;
	int i, fd, rdpmc = 1;

	memset(pmu, 0, sizeof(*pmu));
	for (i = 0; i < BM_CTR_MAX; i++)
		pmu->fd[i] = -1;
	type = pmu_select(pmu, cpu);
	pmu->mode = BM_PMU_NONE;
	if (want == BM_PMU_NONE)
		return;

	for (i = 0; i < BM_CTR_MAX; i++) {
		fd = event_open(type, config[i], pmu->fd[BM_CTR_CYCLES]);
		if (fd < 0) {
			/* without cycles there is no PMU; without the rest
			 * there is still a useful one */
			if (i == BM_CTR_CYCLES) {
				pmu->open_errno = errno;
				return;
			}
			break;
		}
		pmu->fd[i] = fd;
		pmu->nctr++;
	}

	for (i = 0; i < pmu->nctr && want == BM_PMU_RDPMC; i++) {
		pmu->pg[i] = mmap(NULL, page, PROT_READ, MAP_SHARED, pmu->fd[i], 0);
		if (pmu->pg[i] == MAP_FAILED) {
			pmu->pg[i] = NULL;
			rdpmc = 0;
		} else if (!pmu->pg[i]->cap_user_rdpmc) {
			rdpmc = 0;
		}
	}
	pmu->mode = want == BM_PMU_RDPMC && rdpmc ? BM_PMU_RDPMC : BM_PMU_READ;
}

void
bm_pmu_close(struct bm_pmu *pmu)
{
	long page = sysconf(_SC_PAGESIZE);
	int i;

	for (i = 0; i < BM_CTR_MAX; i++) {
		if (pmu->pg[i] != NULL)
			munmap(pmu->pg[i], page);
		if (pmu->fd[i] >= 0)
			close(pmu->fd[i]);
		pmu->pg[i] = NULL;
		pmu->fd[i] = -1;
	}
	pmu->nctr = 0;
	pmu->mode = BM_PMU_NONE;
}

void
bm_pmu_read_group(const struct bm_pmu *pmu, uint64_t *ctr)
{
	/* PERF_FORMAT_GROUP: nr, then one value per member */
	uint64_t buf[1 + BM_CTR_MAX];
	ssize_t n;
	uint64_t i;

	n = read(pmu->fd[BM_CTR_CYCLES], buf, sizeof(buf));
	if (n < (ssize_t)sizeof(uint64_t))
		buf[0] = 0;
	for (i = 0; i < (uint64_t)pmu->nctr; i++)
		ctr[i] = i < buf[0] ? buf[1 + i] : 0;
}

int
bm_invariant_tsc(void)
{
	unsigned int eax, ebx, ecx, edx;

	if (__get_cpuid(0x80000000, &eax, &ebx, &ecx, &edx) == 0 || eax < 0x80000007)
		return 0;
	__get_cpuid(0x80000007, &eax, &ebx, &ecx, &edx);
	return (edx >> 8) & 1;
}

static uint64_t
mono_raw_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
}

uint64_t
bm_tsc_hz_measure(unsigned int ms)
{
	struct timespec req = {ms / 1000, (long)(ms % 1000) * 1000000};
	uint64_t t0, t1, c0, c1;

	/* pair each clock read with a TSC read, then sleep in between */
	t0 = mono_raw_ns();
	c0 = bm_rdtscp();
	while (nanosleep(&req, &req) != 0 && errno == EINTR)
		;
	t1 = mono_raw_ns();
	c1 = bm_rdtscp();
	if (t1 <= t0)
		return 0;
	return (uint64_t)((double)(c1 - c0) * 1e9 / (double)(t1 - t0));
}

int
bm_perf_paranoid(void)
{
	char buf[32];

	if (read_file("/proc/sys/kernel/perf_event_paranoid", buf, sizeof(buf)) != 0)
		return INT32_MIN;
	return atoi(buf);
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

uint64_t
bm_median_u64(uint64_t *v, size_t n)
{
	if (n == 0)
		return 0;
	qsort(v, n, sizeof(*v), cmp_u64);
	if (n % 2)
		return v[n / 2];
	return v[n / 2 - 1] + (v[n / 2] - v[n / 2 - 1]) / 2;
}

uint64_t
bm_mad_u64(uint64_t *v, size_t n, uint64_t med)
{
	size_t i;

	for (i = 0; i < n; i++)
		v[i] = v[i] > med ? v[i] - med : med - v[i];
	return bm_median_u64(v, n);
}

int
bm_timer_measure(const struct bm_pmu *pmu, struct bm_timer *t, unsigned int n)
{
	struct bm_mark a, b;
	uint64_t *v;
	unsigned int i, warm = n / 10;
	int c;

	memset(t, 0, sizeof(*t));
	/* row 0 is the TSC, row 1 + c is counter c */
	v = malloc((size_t)n * (1 + BM_CTR_MAX) * sizeof(*v));
	if (v == NULL)
		return -1;

	for (i = 0; i < warm + n; i++) {
		bm_begin(pmu, &a);
		bm_end(pmu, &b);
		if (i < warm)
			continue;
		v[i - warm] = b.tsc - a.tsc;
		for (c = 0; c < pmu->nctr; c++)
			v[(size_t)(1 + c) * n + i - warm] = b.ctr[c] - a.ctr[c];
	}

	t->n = n;
	t->tsc = bm_median_u64(v, n);
	t->tsc_mad = bm_mad_u64(v, n, t->tsc);
	for (c = 0; c < pmu->nctr; c++) {
		uint64_t *row = v + (size_t)(1 + c) * n;

		t->ctr[c] = bm_median_u64(row, n);
		t->ctr_mad[c] = bm_mad_u64(row, n, t->ctr[c]);
	}
	free(v);
	return 0;
}

static int
cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

double
bm_median_d(double *v, size_t n)
{
	if (n == 0)
		return 0;
	qsort(v, n, sizeof(*v), cmp_d);
	if (n % 2)
		return v[n / 2];
	return (v[n / 2 - 1] + v[n / 2]) / 2;
}

double
bm_mad_d(double *v, size_t n, double med)
{
	size_t i;

	for (i = 0; i < n; i++)
		v[i] = v[i] > med ? v[i] - med : med - v[i];
	return bm_median_d(v, n);
}

#define BM_PRAGMA(x) _Pragma(#x)
#define BM_UNROLL_LOOP(u) BM_PRAGMA(GCC unroll u)

static inline void
mark_diff(struct bm_mark *d, const struct bm_mark *t0, const struct bm_mark *t1)
{
	int i;

	d->tsc = t1->tsc - t0->tsc;
	for (i = 0; i < BM_CTR_MAX; i++)
		d->ctr[i] = t1->ctr[i] - t0->ctr[i];
}

/* the timed loops; noinline keeps one copy, the same for every caller */
static __attribute__((noinline)) void
loop_throughput(const struct bm_pmu *pmu, flw_fn fn, void *state, const flw_corpus *c, size_t base,
	size_t n, struct bm_mark *d)
{
	const char *items = c->items;
	size_t isz = c->item_size, mask = c->count - 1, i;
	struct bm_mark t0 = {0}, t1 = {0};

	bm_begin(pmu, &t0);
	BM_UNROLL_LOOP(BM_UNROLL)
	for (i = 0; i < n; i++)
		flw_use(fn(items + ((base + i) & mask) * isz, state));
	bm_end(pmu, &t1);
	mark_diff(d, &t0, &t1);
}

static __attribute__((noinline)) void
loop_chained(const struct bm_pmu *pmu, flw_chain_fn fn, void *state, const flw_corpus *c,
	size_t base, size_t n, struct bm_mark *d)
{
	const char *items = c->items;
	size_t isz = c->item_size, mask = c->count - 1, i;
	struct bm_mark t0 = {0}, t1 = {0};
	uint64_t in, out = 0;

	bm_begin(pmu, &t0);
	BM_UNROLL_LOOP(BM_UNROLL)
	for (i = 0; i < n; i++) {
		memcpy(&in, items + ((base + i) & mask) * isz, sizeof(in));
		out = fn(flw_chain_dep(in, out), state);
	}
	bm_end(pmu, &t1);
	flw_use(out);
	mark_diff(d, &t0, &t1);
}

static __attribute__((noinline)) void
loop_adapter(const struct bm_pmu *pmu, flw_fn fn, flw_next_fn next, void *state,
	const flw_corpus *c, size_t base, size_t n, struct bm_mark *d)
{
	const char *items = c->items;
	size_t isz = c->item_size, mask = c->count - 1, i;
	struct bm_mark t0 = {0}, t1 = {0};
	uint64_t out = 0;

	bm_begin(pmu, &t0);
	BM_UNROLL_LOOP(BM_UNROLL)
	for (i = 0; i < n; i++)
		out = fn(next(out, items + ((base + i) & mask) * isz), state);
	bm_end(pmu, &t1);
	flw_use(out);
	mark_diff(d, &t0, &t1);
}

void
bm_job_run(const struct bm_pmu *pmu, const struct bm_job *job, size_t base, size_t n,
	struct bm_mark *d)
{
	if (job->mode == FLW_THROUGHPUT)
		loop_throughput(pmu, job->fn, job->state, job->corpus, base, n, d);
	else if (job->chain != NULL)
		loop_chained(pmu, job->chain, job->state, job->corpus, base, n, d);
	else
		loop_adapter(pmu, job->fn, job->next, job->state, job->corpus, base, n, d);
}

long
bm_job_distinct_inputs(const struct bm_job *job, size_t n)
{
	const flw_corpus *c = job->corpus;
	const char *items = c->items;
	size_t slots = 1, mask, i, h;
	const void **set, *p;
	uint64_t out = 0;
	long distinct = 0;
	int null_seen = 0;

	/* open addressing, at most half full */
	while (slots < 2 * n)
		slots *= 2;
	mask = slots - 1;
	set = calloc(slots, sizeof(*set));
	if (set == NULL)
		return -1;

	for (i = 0; i < n; i++) {
		p = job->next(out, items + (i & (c->count - 1)) * c->item_size);
		out = job->fn(p, job->state);
		if (p == NULL) {
			distinct += !null_seen;
			null_seen = 1;
			continue;
		}
		h = (size_t)(((uintptr_t)p * 0x9e3779b97f4a7c15ull) >> 17) & mask;
		while (set[h] != NULL && set[h] != p)
			h = (h + 1) & mask;
		if (set[h] == NULL) {
			set[h] = p;
			distinct++;
		}
	}
	flw_use(out);
	free(set);
	return distinct;
}

/* largest batch: a snippet this cheap is broken, not fast */
#define BM_MAX_BATCH ((size_t)1 << 32)

size_t
bm_job_size(const struct bm_pmu *pmu, const struct bm_job *job, uint64_t target_tsc)
{
	struct bm_mark d;
	size_t n = BM_UNROLL;
	uint64_t want;

	for (;;) {
		bm_job_run(pmu, job, 0, n, &d);
		if (d.tsc >= target_tsc || n >= BM_MAX_BATCH)
			break;
		n *= 2;
	}

	/* refine once: n * target / measured, rounded up to the unroll factor */
	if (d.tsc > 0) {
		want = ((uint64_t)n * target_tsc + d.tsc - 1) / d.tsc;
		n = (want + BM_UNROLL - 1) / BM_UNROLL * BM_UNROLL;
	}
	return n > 0 ? n : BM_UNROLL;
}

/*
 * getrusage() rather than a context-switch event in the perf group: a
 * software event has no rdpmc index, so it would push every group read to
 * read(2), and it needs no PMU access at all.
 */
void
bm_thread_mark(struct bm_thread_mark *m)
{
	struct rusage ru;

	m->cpu = sched_getcpu();
	m->ctxsw = getrusage(RUSAGE_THREAD, &ru) == 0 ? ru.ru_nvcsw + ru.ru_nivcsw : 0;
}
