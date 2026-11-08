/* SPDX-License-Identifier: BSD-3-Clause */

/* Runner internals shared between bm_run.c, bm_env.c and bm_out.c. */

#ifndef BM_INTERNAL_H
#define BM_INTERNAL_H

#include <jansson.h>
#include <regex.h>
#include <stdint.h>
#include <stdio.h>

#include "bm_calib.h"
#include "bm_core.h"

/* exit codes, see the guide's "Exit codes" table */
#define BM_EXIT_OK 0
#define BM_EXIT_FAIL 1
#define BM_EXIT_ERROR 99

#define BM_SCHEMA 1

/* build identity, from Meson */
#ifndef BM_BUILDTYPE
#define BM_BUILDTYPE "unknown"
#endif
#ifndef BM_CARGS
#define BM_CARGS ""
#endif

/* longest "Name/mode/impl" */
#define BM_NAME_MAX 256
/* batches run and discarded at the start of every epoch */
#define BM_BENCH_WARMUP 5
/* empty windows of the per-epoch timer overhead */
#define BM_EPOCH_TIMER_WINDOWS 1000

/* --mode: the benchmark's own mode, or an override */
enum bm_mode_sel { BM_MODE_DEFAULT, BM_MODE_THROUGHPUT, BM_MODE_CHAINED, BM_MODE_BOTH };

/* --subtract: where the calibrated harness cost a is meant to be subtracted */
enum bm_subtract { BM_SUB_CHAINED, BM_SUB_ALL, BM_SUB_NONE };

struct bm_opts {
	const char *bench_re;	   /* --bench, NULL selects all */
	const char *out;	   /* --out, "-" is stdout */
	const char *profile;	   /* --profile, "dev" or "ci" */
	uint64_t seed;		   /* --seed */
	enum bm_pmu_mode pmu;	   /* --pmu, the most capable mode to try */
	double batch_ms;	   /* --batch-time */
	unsigned int epochs;	   /* --epochs, E */
	unsigned int samples;	   /* --samples, K batches per epoch */
	enum bm_mode_sel mode;	   /* --mode */
	enum bm_subtract subtract; /* --subtract */
	double imul_latency;	   /* --ref-latency=imul:N */
	double add_latency;	   /* --ref-latency=add:N */
	int list;		   /* --list */
};

/* where the measurement ran, filled in on the worker lcore */
struct bm_worker {
	unsigned int lcore;
	unsigned int socket;
	int cpu;     /* sched_getcpu() */
	int on_main; /* no worker lcore, ran on the main lcore */
};

/* clocks and counters, set up and checked on the worker lcore */
struct bm_clock {
	int invariant_tsc;
	uint64_t tsc_hz;	  /* rte_get_tsc_hz() */
	uint64_t tsc_hz_measured; /* against CLOCK_MONOTONIC_RAW */
	int perf_paranoid;
	struct bm_pmu pmu;
	struct bm_timer timer; /* startup timer overhead */
};

/* one batch */
struct bm_sample {
	unsigned int epoch, batch;
	uint64_t iters;
	uint64_t tsc;
	uint64_t ctr[BM_CTR_MAX];
	uint32_t ctxsw; /* context switches during the batch */
	uint32_t flags; /* BM_REJ_* reasons; 0 means kept */
};

/* one epoch's layout and timer overhead */
struct bm_epoch {
	size_t corpus_offset; /* bytes from the start of the corpus allocation */
	size_t stack_offset;  /* bytes the stack was moved down */
	struct bm_timer timer;
};

/* one selected implementation of one benchmark, in one mode */
struct bm_result {
	const flw_bench *bench;
	size_t impl; /* index into bench->impls */
	flw_mode mode;
	char name[BM_NAME_MAX];
	int ran;		   /* 0: skipped, no records */
	size_t iters;		   /* batch size N, fixed for all epochs */
	size_t corpus_bytes;	   /* working set, as the generator left it */
	unsigned int ops_per_call; /* R, at least 1 */

	/* harness cost and floor that apply, per call in the calibration unit */
	double a, floor;
	int subtract; /* a is meant to be subtracted (--subtract) */
	/* chained through an adapter: its own sweep and the collapse check */
	int adapter;
	struct bm_calib_point adapter_pts[1 + BM_CALIB_CHAIN];
	struct bm_calib_fit adapter_fit;
	long distinct_inputs;
	/* the calibration that applies, confirmed or not by the second-fastest rounds */
	struct bm_calib_risk risk;

	double per_call;	     /* median over all samples */
	double per_op;		     /* (per_call - a if subtracted) / R */
	double per_op_err;	     /* how far per_op moves with a_alt, 0 if a is confirmed */
	int harness_bound;	     /* on the throughput floor, or below the chained floor */
	int harness_bound_uncertain; /* the alternative floor gives the other verdict */
	size_t kept;		     /* samples with no rejection flag; per_call uses these */
	size_t rej_ratio, rej_ctx, rej_migrate; /* samples with each flag */
	struct bm_sample *samples;		/* epoch by epoch, K each */
	size_t nsamples;
	struct bm_epoch *epochs;
	unsigned int nepochs;
};

/* what the environment checks found, for the env record */
struct bm_sys {
	char governor[32], epp[48], microcode[32];
	int turbo;	/* 1 on, 0 off, -1 unknown */
	int smt_active; /* -1 unknown */
	int sibling;	/* the worker core's other hardware thread, -1 none online */
	unsigned long long sib_busy0, sib_total0; /* its /proc/stat jiffies at start */
	double sibling_busy;			  /* busy share during the run, -1 unknown */
	int isolated, nohz_full;		  /* worker cpu in isolcpus / nohz_full */
	size_t page_size;			  /* behind the calibration corpus */
};

struct bm_ctx {
	struct bm_opts opts;
	struct bm_worker worker;
	struct bm_clock clock;
	struct bm_sys sys;
	struct bm_calib calib;
	struct bm_calib_opts calib_opts; /* also used for adapter sweeps */
	const regex_t *bench_re;	 /* compiled --bench, NULL selects all */
	struct bm_result *results;
	size_t nresults;
	uint64_t layout_rng;   /* draws the per-epoch offsets */
	int failed;	       /* a check failed under the ci profile: exit 1 */
	const char **eal_argv; /* EAL arguments as given, before rte_eal_init */
	int eal_argc;
	json_t *warnings; /* array of strings, goes into the env record */
};

/* print a warning to stderr and add it to the env record */
void bm_warn(struct bm_ctx *ctx, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/*
 * Warn when a calibration is not confirmed, saying what it risks: how far
 * corrected results may be off (if a_matters) and which results may get the
 * wrong harness-bound verdict. what names the calibration.
 */
void bm_warn_risk(struct bm_ctx *ctx, const char *what, const struct bm_calib_risk *r,
	int a_matters);

/* bm_env.c */
json_t *bm_env_record(const struct bm_ctx *ctx);
json_t *bm_timer_record(const struct bm_ctx *ctx);

/* bm_check.c */
/* a warning that fails the run (exit 1) under the ci profile */
void bm_problem(struct bm_ctx *ctx, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* on the worker: read the machine's state, report problems */
void bm_check_start(struct bm_ctx *ctx);
/* on the worker, after the benchmarks: how busy the SMT sibling was */
void bm_check_end(struct bm_ctx *ctx);
/* page size behind an rte_malloc() allocation */
void bm_check_page(struct bm_ctx *ctx, const void *addr);
json_t *bm_sys_fields(const struct bm_ctx *ctx);

/* bm_bench.c */
const char *bm_mode_name(flw_mode mode);
int bm_list_benches(const struct bm_ctx *ctx);
/* run the selected benchmarks on the calling lcore; -1 on a harness error */
int bm_run_benches(struct bm_ctx *ctx);
void bm_results_free(struct bm_ctx *ctx);

/* bm_out.c: calibration, self-test, bench and sample records */
json_t *bm_calib_chained_record(const struct bm_ctx *ctx);
json_t *bm_calib_throughput_record(const struct bm_ctx *ctx);
json_t *bm_selftest_record(const struct bm_ctx *ctx);
json_t *bm_bench_record(const struct bm_ctx *ctx, const struct bm_result *r);
json_t *bm_epoch_record(const struct bm_ctx *ctx, const struct bm_result *r, unsigned int e);
json_t *bm_sample_record(const struct bm_ctx *ctx, const struct bm_result *r,
	const struct bm_sample *s);

/* bm_out.c */
struct bm_out {
	FILE *f;
	const char *path;
};

int bm_out_open(struct bm_out *out, const char *path);
int bm_out_write(struct bm_out *out, json_t *rec);
int bm_out_close(struct bm_out *out);

#endif /* BM_INTERNAL_H */
