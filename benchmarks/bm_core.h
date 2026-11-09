/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Measurement core: TSC and PMU counters, fenced windows, timer overhead,
 * timed loops and batch sizing. Plain C plus perf_event_open(2), no DPDK.
 */

#ifndef BM_CORE_H
#define BM_CORE_H

#include <linux/perf_event.h>
#include <stddef.h>
#include <stdint.h>

#include "bench.h"

/* harness loop unroll factor U, a build-time constant; see the guide */
#ifndef BM_UNROLL
#define BM_UNROLL 1
#endif

/* how core cycles and the other hardware counters are read */
enum bm_pmu_mode {
	BM_PMU_NONE,  /* TSC only */
	BM_PMU_READ,  /* read(2) on the group leader, one syscall per mark */
	BM_PMU_RDPMC, /* rdpmc through the mmap'd perf_event_mmap_page */
};

/* counters in the perf group; BM_CTR_CYCLES is the leader */
enum { BM_CTR_CYCLES, BM_CTR_INSTR, BM_CTR_MAX };

struct bm_pmu {
	enum bm_pmu_mode mode;
	int nctr; /* counters opened: 0, 1 (cycles) or 2 (+instructions) */
	int fd[BM_CTR_MAX];
	struct perf_event_mmap_page *pg[BM_CTR_MAX];
	char name[16];	    /* "cpu", "cpu_core", "cpu_atom" */
	char core_type[16]; /* "uniform", "core", "atom", "unknown" */
	int open_errno;	    /* why the cycles counter did not open, or 0 */
};

/* one end of a window */
struct bm_mark {
	uint64_t tsc;
	uint64_t ctr[BM_CTR_MAX];
};

/* empty-window cost, median and MAD over n windows */
struct bm_timer {
	unsigned int n;
	uint64_t tsc, tsc_mad;
	uint64_t ctr[BM_CTR_MAX], ctr_mad[BM_CTR_MAX];
};

/*
 * Open the counters on the calling thread. want is the most capable mode
 * to use: BM_PMU_RDPMC tries rdpmc first and falls back to read(2).
 * cpu selects the PMU on hybrid CPUs. Never fails: without a PMU the
 * mode is BM_PMU_NONE.
 */
void bm_pmu_open(struct bm_pmu *pmu, enum bm_pmu_mode want, int cpu);
void bm_pmu_close(struct bm_pmu *pmu);
const char *bm_pmu_mode_name(enum bm_pmu_mode mode);

/* read(2) of the whole group; the slow path of bm_pmu_read() */
void bm_pmu_read_group(const struct bm_pmu *pmu, uint64_t *ctr);

int bm_invariant_tsc(void);
/* TSC rate measured against CLOCK_MONOTONIC_RAW over ms milliseconds */
uint64_t bm_tsc_hz_measure(unsigned int ms);
/* /proc/sys/kernel/perf_event_paranoid, or INT32_MIN if unreadable */
int bm_perf_paranoid(void);

/* measure n empty windows; returns -1 if out of memory */
int bm_timer_measure(const struct bm_pmu *pmu, struct bm_timer *t, unsigned int n);

/*
 * A timed loop over a corpus: throughput (fn, independent iterations) or
 * chained. Chained runs chain word in/out when it is set: each corpus item,
 * read as its first 8 bytes, is made to depend on the previous output with
 * flw_chain_dep(). Otherwise it runs fn on the item's address made to
 * depend on the previous output the same way (the pointer dependency).
 */
struct bm_job {
	flw_mode mode;
	flw_fn fn;
	flw_chain_fn chain;
	void *state;
	const flw_corpus *corpus;
};

/* one batch of n iterations starting at corpus index base; d is end - start */
void bm_job_run(const struct bm_pmu *pmu, const struct bm_job *job, size_t base, size_t n,
	struct bm_mark *d);

/* why a sample is rejected: bits of its flags */
enum {
	BM_REJ_RATIO = 1 << 0,	 /* cycles/TSC off its epoch's median: frequency or interrupt */
	BM_REJ_CTX = 1 << 1,	 /* the thread was switched out during the batch */
	BM_REJ_MIGRATE = 1 << 2, /* the thread changed CPU during the batch */
};

/* allowed relative distance of a sample's cycles/TSC from its epoch's median */
#define BM_RATIO_TOL 0.02

/* the thread's CPU and context switches so far, read outside the timed window */
struct bm_thread_mark {
	int cpu;
	long ctxsw;
};

void bm_thread_mark(struct bm_thread_mark *m);

/*
 * Batch size for a target batch duration in TSC ticks, like Go's b.N:
 * double n until one batch takes the target, then refine once. A multiple
 * of BM_UNROLL.
 */
size_t bm_job_size(const struct bm_pmu *pmu, const struct bm_job *job, uint64_t target_tsc);

static inline uint64_t
bm_splitmix64(uint64_t *s)
{
	uint64_t z = (*s += 0x9e3779b97f4a7c15ull);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

/* sorts v in place */
uint64_t bm_median_u64(uint64_t *v, size_t n);
/* sorts v in place */
double bm_median_d(double *v, size_t n);
/* median absolute deviation from med; overwrites v */
double bm_mad_d(double *v, size_t n, double med);
/* median absolute deviation from med; sorts v in place */
uint64_t bm_mad_u64(uint64_t *v, size_t n, uint64_t med);

static inline void
bm_lfence(void)
{
	__asm__ volatile("lfence" ::: "memory");
}

static inline uint64_t
bm_rdtsc(void)
{
	uint32_t lo, hi;

	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)::"memory");
	return (uint64_t)hi << 32 | lo;
}

static inline uint64_t
bm_rdtscp(void)
{
	uint32_t lo, hi, aux;

	__asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux)::"memory");
	return (uint64_t)hi << 32 | lo;
}

/*
 * Seqlock read of one counter from its user page, as documented in
 * linux/perf_event.h. Returns -1 when the counter is not on the PMU right
 * now (index 0), so the caller can fall back to read(2).
 */
static inline int
bm_rdpmc_page(const struct perf_event_mmap_page *pg, uint64_t *val)
{
	const volatile struct perf_event_mmap_page *pc = pg;
	uint32_t seq, idx, lo, hi;
	uint64_t offset;
	int64_t pmc;
	int width;

	do {
		seq = pc->lock;
		__asm__ volatile("" ::: "memory");
		idx = pc->index;
		offset = pc->offset;
		width = pc->pmc_width;
		if (!pc->cap_user_rdpmc || idx == 0)
			return -1;
		__asm__ volatile("rdpmc" : "=a"(lo), "=d"(hi) : "c"(idx - 1));
		pmc = (int64_t)((uint64_t)hi << 32 | lo);
		pmc = (int64_t)((uint64_t)pmc << (64 - width)) >> (64 - width);
		offset += pmc;
		__asm__ volatile("" ::: "memory");
	} while (pc->lock != seq);

	*val = offset;
	return 0;
}

/*
 * Read all counters. rev reverses the order (the end of a window reads
 * the counters in the reverse of the begin order, so the windows nest).
 */
static inline void
bm_pmu_read(const struct bm_pmu *pmu, uint64_t *ctr, int rev)
{
	int i, k;

	switch (pmu->mode) {
	case BM_PMU_RDPMC:
		for (k = 0; k < pmu->nctr; k++) {
			i = rev ? pmu->nctr - 1 - k : k;
			if (bm_rdpmc_page(pmu->pg[i], &ctr[i]) != 0) {
				bm_pmu_read_group(pmu, ctr);
				return;
			}
		}
		break;
	case BM_PMU_READ:
		bm_pmu_read_group(pmu, ctr);
		break;
	case BM_PMU_NONE:
		break;
	}
}

/*
 * Window: lfence; rdtsc; core counters ... core counters; rdtscp; lfence.
 * The core-cycle reads sit right inside the TSC reads, so both windows
 * nest the same work.
 */
static inline void
bm_begin(const struct bm_pmu *pmu, struct bm_mark *m)
{
	bm_lfence();
	m->tsc = bm_rdtsc();
	bm_lfence();
	bm_pmu_read(pmu, m->ctr, 0);
	bm_lfence();
}

static inline void
bm_end(const struct bm_pmu *pmu, struct bm_mark *m)
{
	bm_lfence();
	bm_pmu_read(pmu, m->ctr, 1);
	m->tsc = bm_rdtscp();
	bm_lfence();
}

#endif /* BM_CORE_H */
