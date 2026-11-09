/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Calibration: reference snippets with a known cost, run through the same
 * loops as user snippets, give the harness cost and check the counters.
 */

#ifndef BM_CALIB_H
#define BM_CALIB_H

#include "bm_core.h"

#define BM_CALIB_CHAIN 5 /* k = 1, 2, 4, 8, 16 */
#define BM_CALIB_INDEP 6 /* s = 1, 2, 4, 8, 16, 32 */
/* most points in one sweep: ref/nop plus the largest sweep */
#define BM_CALIB_MAXPTS (1 + BM_CALIB_INDEP)

struct bm_calib_opts {
	uint64_t target_tsc;  /* batch duration */
	unsigned int warmup;  /* batches discarded per point */
	unsigned int batches; /* batches kept per point and round */
	unsigned int rounds;  /* passes over the whole sweep; each point keeps its lowest */
	double imul_latency;  /* expected, core cycles */
	double add_latency;
};

/* one reference snippet at one size */
struct bm_calib_point {
	char ref[48];	   /* "ref/imul-chain-4" */
	unsigned int k;	   /* chain length or streams; 0 for ref/nop */
	size_t iters;	   /* batch size */
	double per_op;	   /* median per op, in the calibration unit: the lowest round */
	double spread;	   /* highest round's median minus the lowest */
	double gap;	   /* second-lowest round's median minus the lowest */
	double tsc, instr; /* median TSC ticks and instructions per op */
	double resid;	   /* per_op minus the fitted line */
	int on_line;	   /* used for the fit */
};

struct bm_calib_fit {
	double a;	 /* intercept with the expected slope: the harness cost */
	double slope;	 /* fitted slope */
	double expected; /* expected slope */
	double floor;	 /* per_op below which the harness hides the snippet */
	unsigned int floor_k;
	int npts; /* points on the line */
};

/*
 * What a disturbed calibration means for results: a and the floor fitted
 * on each point's fastest round (used) and again on its second-fastest.
 * When the two agree the calibration is confirmed.
 */
struct bm_calib_risk {
	double a, a_alt;
	double floor, floor_alt;
	int a_uncertain;     /* a moves by more than BM_CALIB_FIT_ABS_TOL */
	int floor_uncertain; /* the floor moves by more than the fit tolerance */
};

struct bm_calib {
	int unit_cycles; /* per_op in core cycles; 0: TSC ticks, no PMU */
	/* chained: ref/nop, then ref/imul-chain-k; ref/add-chain-k */
	struct bm_calib_point imul[1 + BM_CALIB_CHAIN];
	struct bm_calib_point add[BM_CALIB_CHAIN];
	/* throughput: ref/nop, then ref/imul-indep-s */
	struct bm_calib_point indep[1 + BM_CALIB_INDEP];
	struct bm_calib_fit chained, add_fit, throughput;
	/* the same fits on each point's second-fastest round */
	struct bm_calib_fit chained_alt, throughput_alt;
	struct bm_calib_risk chained_risk, throughput_risk;
	/* chained on a pointer: swept by the first implementation that needs it */
	int pointer_done;
	struct bm_calib_point ptr[1 + BM_CALIB_CHAIN];
	struct bm_calib_fit pointer;
	struct bm_calib_risk pointer_risk;

	/* self-test; *_ok is 1 pass, 0 fail, -1 skipped (no PMU) */
	int slope_ok, ratio_ok, timer_ok;
	double ratio_median, ratio_mad; /* cycles / TSC over all batches */
	struct bm_timer timer;		/* median over the repeats */
	uint64_t timer_spread;		/* max - min of the repeats' medians, in the unit */
};

/* points within this relative (or absolute, per op) distance are on the line */
#define BM_CALIB_FIT_REL_TOL 0.03
#define BM_CALIB_FIT_ABS_TOL 0.5

/* allowed relative error of the fitted chained slope */
#define BM_CALIB_SLOPE_TOL 0.03

/*
 * Run the sweeps on the calling thread. c is a corpus of at least 8-byte
 * items. Returns -1 if out of memory.
 */
int bm_calibrate(const struct bm_pmu *pmu, const flw_corpus *c, const struct bm_calib_opts *o,
	struct bm_calib *cal);

/*
 * The chained sweep through the pointer dependency: ref/ptr-nop and
 * ref/ptr-imul-chain-k called on the item's address. p holds
 * 1 + BM_CALIB_CHAIN points. Returns -1 if out of memory.
 */
int bm_calib_pointer(const struct bm_pmu *pmu, const flw_corpus *c, const struct bm_calib_opts *o,
	struct bm_calib_point *p, struct bm_calib_fit *f, struct bm_calib_risk *risk);

/* is per_call harness-bound against floor? throughput: on it; chained: below it */
int bm_calib_bound(flw_mode mode, double per_call, double floor);

#endif /* BM_CALIB_H */
