/* SPDX-License-Identifier: BSD-3-Clause */

/* Reference snippets, calibration sweeps, line fit and self-test. */

#include "bm_calib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REF_MUL 0x9e3779b97f4a7c15ull

#define RATIO_TOL 0.01	   /* relative MAD of cycles / TSC */
#define TIMER_REPEATS 5	   /* timer overhead measurements */
#define TIMER_WINDOWS 1000 /* empty windows per measurement */
#define TIMER_ABS_TOL 2	   /* spread always allowed, in the unit */
#define TIMER_REL_TOL 0.05 /* relative spread of the repeats */

/*
 * The references are written in asm so the compiler cannot fold the chain,
 * and are called through the same pointer and loops as user snippets.
 */

/* k dependent imul: latency k * imul */
#define REF_IMUL_CHAIN(k)                                                                 \
	static uint64_t ref_imul_chain_##k(uint64_t x, void *state)                       \
	{                                                                                 \
		(void)state;                                                              \
		__asm__(".rept " #k "\n\timul %1, %0\n\t.endr" : "+r"(x) : "r"(REF_MUL)); \
		return x;                                                                 \
	}

/* k dependent add: latency k * add */
#define REF_ADD_CHAIN(k)                                                                 \
	static uint64_t ref_add_chain_##k(uint64_t x, void *state)                       \
	{                                                                                \
		(void)state;                                                             \
		__asm__(".rept " #k "\n\tadd %1, %0\n\t.endr" : "+r"(x) : "r"(REF_MUL)); \
		return x;                                                                \
	}

/* s independent imul from the same input: s per op at one imul per cycle */
#define REF_IMUL_INDEP(s)                                                                         \
	static uint64_t ref_imul_indep_##s(const void *in, void *state)                           \
	{                                                                                         \
		uint64_t x, y;                                                                    \
                                                                                                  \
		(void)state;                                                                      \
		memcpy(&x, in, sizeof(x));                                                        \
		__asm__(".rept " #s "\n\timul $0x7f4a7c15, %1, %0\n\t.endr" : "=&r"(y) : "r"(x)); \
		return y;                                                                         \
	}

/*
 * k dependent imul on the pointer fn is called with, for the pointer
 * sweep. The pointer is not read: the snippet's own loads are part of its
 * cost.
 */
#define REF_PTR_IMUL_CHAIN(k)                                                             \
	static uint64_t ref_ptr_imul_chain_##k(const void *p, void *state)                \
	{                                                                                 \
		uint64_t x = (uint64_t)(uintptr_t)p;                                      \
                                                                                          \
		(void)state;                                                              \
		__asm__(".rept " #k "\n\timul %1, %0\n\t.endr" : "+r"(x) : "r"(REF_MUL)); \
		return x;                                                                 \
	}

REF_IMUL_CHAIN(1)
REF_IMUL_CHAIN(2)
REF_IMUL_CHAIN(4)
REF_IMUL_CHAIN(8)
REF_IMUL_CHAIN(16)
REF_ADD_CHAIN(1)
REF_ADD_CHAIN(2)
REF_ADD_CHAIN(4)
REF_ADD_CHAIN(8)
REF_ADD_CHAIN(16)
REF_PTR_IMUL_CHAIN(1)
REF_PTR_IMUL_CHAIN(2)
REF_PTR_IMUL_CHAIN(4)
REF_PTR_IMUL_CHAIN(8)
REF_PTR_IMUL_CHAIN(16)
REF_IMUL_INDEP(1)
REF_IMUL_INDEP(2)
REF_IMUL_INDEP(4)
REF_IMUL_INDEP(8)
REF_IMUL_INDEP(16)
REF_IMUL_INDEP(32)

/* returns its input: the harness floor */
static uint64_t
ref_nop_chain(uint64_t x, void *state)
{
	(void)state;
	return x;
}

static uint64_t
ref_nop(const void *in, void *state)
{
	(void)state;
	return (uint64_t)(uintptr_t)in;
}

static const unsigned int chain_k[BM_CALIB_CHAIN] = {1, 2, 4, 8, 16};
static const flw_chain_fn imul_chain[BM_CALIB_CHAIN] = {
	ref_imul_chain_1,
	ref_imul_chain_2,
	ref_imul_chain_4,
	ref_imul_chain_8,
	ref_imul_chain_16,
};
static const flw_chain_fn add_chain[BM_CALIB_CHAIN] = {
	ref_add_chain_1,
	ref_add_chain_2,
	ref_add_chain_4,
	ref_add_chain_8,
	ref_add_chain_16,
};
static const flw_fn ptr_imul_chain[BM_CALIB_CHAIN] = {
	ref_ptr_imul_chain_1,
	ref_ptr_imul_chain_2,
	ref_ptr_imul_chain_4,
	ref_ptr_imul_chain_8,
	ref_ptr_imul_chain_16,
};
static const unsigned int indep_s[BM_CALIB_INDEP] = {1, 2, 4, 8, 16, 32};
static const flw_fn imul_indep[BM_CALIB_INDEP] = {
	ref_imul_indep_1,
	ref_imul_indep_2,
	ref_imul_indep_4,
	ref_imul_indep_8,
	ref_imul_indep_16,
	ref_imul_indep_32,
};

static double
absd(double x)
{
	return x < 0 ? -x : x;
}

/* per-batch values of the point being measured, and ratios of all batches */
struct scratch {
	double *per_op, *tsc, *instr;
	double *ratio;
	size_t nratio;
};

/* one reference in a sweep: what to run, and where its result goes */
struct ref {
	flw_mode mode;
	flw_chain_fn chain;
	flw_fn fn;
	struct bm_calib_point *p;
};

/* most references in one sweep: the startup sweep */
#define MAX_REFS (1 + 2 * BM_CALIB_CHAIN + 1 + BM_CALIB_INDEP)

/* one round of one point: warm-up, then the kept batches */
static void
measure_round(const struct bm_pmu *pmu, const struct bm_job *job, size_t n,
	const struct bm_calib_opts *o, struct scratch *s, double *per_op, double *tsc,
	double *instr)
{
	struct bm_mark d;
	size_t base = 0;
	unsigned int b, i;

	for (b = 0; b < o->warmup + o->batches; b++) {
		bm_job_run(pmu, job, base, n, &d);
		base += n;
		if (b < o->warmup)
			continue;
		i = b - o->warmup;
		s->tsc[i] = (double)d.tsc / n;
		s->per_op[i] = pmu->nctr > BM_CTR_CYCLES ? (double)d.ctr[BM_CTR_CYCLES] / n :
							   s->tsc[i];
		s->instr[i] = pmu->nctr > BM_CTR_INSTR ? (double)d.ctr[BM_CTR_INSTR] / n : 0;
		if (pmu->nctr > BM_CTR_CYCLES && d.tsc > 0)
			s->ratio[s->nratio++] = (double)d.ctr[BM_CTR_CYCLES] / d.tsc;
	}
	*per_op = bm_median_d(s->per_op, o->batches);
	*tsc = bm_median_d(s->tsc, o->batches);
	*instr = bm_median_d(s->instr, o->batches);
}

/*
 * Measure every reference o->rounds times. Each round goes through the
 * whole sweep, so the rounds of one point lie far apart in time and one
 * burst of interference hits at most one of them. Interference only adds
 * cycles: each point keeps its lowest round; the second-lowest (gap) gives
 * a second fit that confirms a and the floor, or says how far off they may be.
 */
static void
sweep(const struct bm_pmu *pmu, const flw_corpus *c, struct ref *refs, int nrefs,
	const struct bm_calib_opts *o, struct scratch *s)
{
	struct bm_job job = {.corpus = c};
	double per_op, tsc, instr, lo2[MAX_REFS], hi[MAX_REFS];
	unsigned int r;
	int i;

	for (i = 0; i < nrefs; i++) {
		job.mode = refs[i].mode;
		job.chain = refs[i].chain;
		job.fn = refs[i].fn;
		refs[i].p->iters = bm_job_size(pmu, &job, o->target_tsc);
	}
	for (r = 0; r < o->rounds; r++) {
		for (i = 0; i < nrefs; i++) {
			struct bm_calib_point *p = refs[i].p;

			job.mode = refs[i].mode;
			job.chain = refs[i].chain;
			job.fn = refs[i].fn;
			measure_round(pmu, &job, p->iters, o, s, &per_op, &tsc, &instr);
			if (r == 0 || per_op < p->per_op) {
				lo2[i] = r == 0 ? per_op : p->per_op;
				p->per_op = per_op;
				p->tsc = tsc;
				p->instr = instr;
			} else if (r == 1 || per_op < lo2[i]) {
				lo2[i] = per_op;
			}
			hi[i] = r == 0 || per_op > hi[i] ? per_op : hi[i];
		}
	}
	for (i = 0; i < nrefs; i++) {
		refs[i].p->spread = hi[i] - refs[i].p->per_op;
		refs[i].p->gap = lo2[i] - refs[i].p->per_op;
	}
}

static void
fit_line(const double *x, const double *y, int n, double *a, double *b)
{
	double mx = 0, my = 0, sxx = 0, sxy = 0;
	int i;

	for (i = 0; i < n; i++) {
		mx += x[i];
		my += y[i];
	}
	mx /= n;
	my /= n;
	for (i = 0; i < n; i++) {
		sxx += (x[i] - mx) * (x[i] - mx);
		sxy += (x[i] - mx) * (y[i] - my);
	}
	*b = sxx > 0 ? sxy / sxx : 0;
	*a = my - *b * mx;
}

/*
 * Fit per_op = a + slope * k on the points where the snippet dominates.
 * Start from the two largest k and extend the line downward while the
 * next point stays on it; below that the harness hides the snippet. a is
 * the intercept with the expected slope (the free intercept without PMU,
 * where the expected slope in TSC ticks is unknown).
 */
static void
fit_sweep(struct bm_calib_point *p, int np, double expected, int unit_cycles,
	struct bm_calib_fit *f)
{
	double x[BM_CALIB_MAXPTS], y[BM_CALIB_MAXPTS], a, b, pred, sum = 0;
	int lo = np - 2, i;

	for (i = 0; i < np; i++) {
		x[i] = p[i].k;
		y[i] = p[i].per_op;
	}
	while (lo > 0) {
		fit_line(x + lo, y + lo, np - lo, &a, &b);
		pred = a + b * x[lo - 1];
		if (absd(y[lo - 1] - pred) >
			BM_CALIB_FIT_REL_TOL * absd(pred) + BM_CALIB_FIT_ABS_TOL)
			break;
		lo--;
	}
	fit_line(x + lo, y + lo, np - lo, &a, &b);

	for (i = lo; i < np; i++)
		sum += y[i] - expected * x[i];
	f->a = unit_cycles ? sum / (np - lo) : a;
	f->slope = b;
	f->expected = expected;
	f->floor = y[lo];
	f->floor_k = p[lo].k;
	f->npts = np - lo;
	for (i = 0; i < np; i++) {
		p[i].resid = y[i] - (a + b * x[i]);
		p[i].on_line = i >= lo;
	}
}

/* the fit again on each point's second-fastest round */
static void
fit_alt(const struct bm_calib_point *p, int np, double expected, int unit_cycles,
	struct bm_calib_fit *f)
{
	struct bm_calib_point q[BM_CALIB_MAXPTS];
	int i;

	for (i = 0; i < np; i++) {
		q[i] = p[i];
		q[i].per_op += p[i].gap;
	}
	fit_sweep(q, np, expected, unit_cycles, f);
}

static void
risk_set(struct bm_calib_risk *r, const struct bm_calib_fit *f, const struct bm_calib_fit *alt)
{
	r->a = f->a;
	r->a_alt = alt->a;
	r->floor = f->floor;
	r->floor_alt = alt->floor;
	r->a_uncertain = absd(alt->a - f->a) > BM_CALIB_FIT_ABS_TOL;
	r->floor_uncertain = absd(alt->floor - f->floor) >
			     BM_CALIB_FIT_REL_TOL * f->floor + BM_CALIB_FIT_ABS_TOL;
}

int
bm_calib_bound(flw_mode mode, double per_call, double floor)
{
	double tol = BM_CALIB_FIT_REL_TOL * floor + BM_CALIB_FIT_ABS_TOL;

	if (mode == FLW_THROUGHPUT)
		return per_call <= floor + tol;
	return per_call < floor - tol;
}

static void
point_init(struct bm_calib_point *p, const char *ref, unsigned int k)
{
	memset(p, 0, sizeof(*p));
	if (k > 0)
		snprintf(p->ref, sizeof(p->ref), "%s-%u", ref, k);
	else
		snprintf(p->ref, sizeof(p->ref), "%s", ref);
	p->k = k;
}

/*
 * Timer overhead TIMER_REPEATS times; keep the median of each field. The
 * spread is judged in core cycles when there is a PMU: the TSC cost of a
 * window moves with the core frequency.
 */
static int
timer_selftest(const struct bm_pmu *pmu, struct bm_calib *cal)
{
	struct bm_timer t[TIMER_REPEATS];
	uint64_t v[TIMER_REPEATS], lo, hi;
	int r, c;

	for (r = 0; r < TIMER_REPEATS; r++)
		if (bm_timer_measure(pmu, &t[r], TIMER_WINDOWS) != 0)
			return -1;

	for (r = 0; r < TIMER_REPEATS; r++)
		v[r] = cal->unit_cycles ? t[r].ctr[BM_CTR_CYCLES] : t[r].tsc;
	lo = hi = v[0];
	for (r = 0; r < TIMER_REPEATS; r++) {
		lo = v[r] < lo ? v[r] : lo;
		hi = v[r] > hi ? v[r] : hi;
	}

#define TIMER_MEDIAN(field)                                         \
	do {                                                        \
		for (r = 0; r < TIMER_REPEATS; r++)                 \
			v[r] = t[r].field;                          \
		cal->timer.field = bm_median_u64(v, TIMER_REPEATS); \
	} while (0)

	TIMER_MEDIAN(tsc);
	TIMER_MEDIAN(tsc_mad);
	for (c = 0; c < pmu->nctr; c++) {
		TIMER_MEDIAN(ctr[c]);
		TIMER_MEDIAN(ctr_mad[c]);
	}
#undef TIMER_MEDIAN

	cal->timer.n = TIMER_WINDOWS;
	cal->timer_spread = hi - lo;
	cal->timer_ok = hi - lo <= TIMER_ABS_TOL || hi - lo <= TIMER_REL_TOL * lo;
	return 0;
}

/* buffers for o->batches per round and the ratios of npoints * rounds rounds */
static double *
scratch_init(struct scratch *s, const struct bm_calib_opts *o, size_t npoints)
{
	double *buf = calloc(3 * o->batches + npoints * o->rounds * o->batches, sizeof(*buf));

	if (buf == NULL)
		return NULL;
	s->per_op = buf;
	s->tsc = s->per_op + o->batches;
	s->instr = s->tsc + o->batches;
	s->ratio = s->instr + o->batches;
	s->nratio = 0;
	return buf;
}

int
bm_calibrate(const struct bm_pmu *pmu, const flw_corpus *c, const struct bm_calib_opts *o,
	struct bm_calib *cal)
{
	struct ref refs[MAX_REFS];
	struct scratch s;
	double *buf;
	int i, n = 0;

	memset(cal, 0, sizeof(*cal));
	cal->unit_cycles = pmu->nctr > BM_CTR_CYCLES;
	buf = scratch_init(&s, o, MAX_REFS);
	if (buf == NULL)
		return -1;

	/* chained */
	point_init(&cal->imul[0], "ref/nop", 0);
	refs[n++] = (struct ref) {.mode = FLW_CHAINED, .chain = ref_nop_chain, .p = &cal->imul[0]};
	for (i = 0; i < BM_CALIB_CHAIN; i++) {
		point_init(&cal->imul[1 + i], "ref/imul-chain", chain_k[i]);
		refs[n++] = (struct ref) {.mode = FLW_CHAINED,
			.chain = imul_chain[i],
			.p = &cal->imul[1 + i]};
	}
	for (i = 0; i < BM_CALIB_CHAIN; i++) {
		point_init(&cal->add[i], "ref/add-chain", chain_k[i]);
		refs[n++] = (struct ref) {.mode = FLW_CHAINED,
			.chain = add_chain[i],
			.p = &cal->add[i]};
	}
	/* throughput: the floor is what ref/nop cannot go below */
	point_init(&cal->indep[0], "ref/nop", 0);
	refs[n++] = (struct ref) {.mode = FLW_THROUGHPUT, .fn = ref_nop, .p = &cal->indep[0]};
	for (i = 0; i < BM_CALIB_INDEP; i++) {
		point_init(&cal->indep[1 + i], "ref/imul-indep", indep_s[i]);
		refs[n++] = (struct ref) {.mode = FLW_THROUGHPUT,
			.fn = imul_indep[i],
			.p = &cal->indep[1 + i]};
	}
	sweep(pmu, c, refs, n, o, &s);

	fit_sweep(cal->imul, 1 + BM_CALIB_CHAIN, o->imul_latency, cal->unit_cycles, &cal->chained);
	fit_sweep(cal->add, BM_CALIB_CHAIN, o->add_latency, cal->unit_cycles, &cal->add_fit);
	/* one imul per cycle per op and stream */
	fit_sweep(cal->indep, 1 + BM_CALIB_INDEP, 1.0, cal->unit_cycles, &cal->throughput);
	cal->throughput.floor = cal->indep[0].per_op;
	cal->throughput.floor_k = 0;
	fit_alt(cal->imul, 1 + BM_CALIB_CHAIN, o->imul_latency, cal->unit_cycles,
		&cal->chained_alt);
	fit_alt(cal->indep, 1 + BM_CALIB_INDEP, 1.0, cal->unit_cycles, &cal->throughput_alt);
	cal->throughput_alt.floor = cal->indep[0].per_op + cal->indep[0].gap;
	cal->throughput_alt.floor_k = 0;
	risk_set(&cal->chained_risk, &cal->chained, &cal->chained_alt);
	risk_set(&cal->throughput_risk, &cal->throughput, &cal->throughput_alt);

	/* self-test: timer last, once the sweeps have settled the frequency */
	if (timer_selftest(pmu, cal) != 0) {
		free(buf);
		return -1;
	}

	/* self-test: slope, then the steadiness of cycles / TSC */
	if (cal->unit_cycles) {
		cal->slope_ok = absd(cal->chained.slope - cal->chained.expected) <=
				BM_CALIB_SLOPE_TOL * cal->chained.expected;
		cal->ratio_median = bm_median_d(s.ratio, s.nratio);
		cal->ratio_mad = bm_mad_d(s.ratio, s.nratio, cal->ratio_median);
		cal->ratio_ok = cal->ratio_mad <= RATIO_TOL * cal->ratio_median;
	} else {
		cal->slope_ok = -1;
		cal->ratio_ok = -1;
	}

	free(buf);
	return 0;
}

int
bm_calib_pointer(const struct bm_pmu *pmu, const flw_corpus *c, const struct bm_calib_opts *o,
	struct bm_calib_point *p, struct bm_calib_fit *f, struct bm_calib_risk *risk)
{
	struct bm_calib_fit alt;
	struct ref refs[1 + BM_CALIB_CHAIN];
	struct scratch s;
	double *buf;
	int i;

	buf = scratch_init(&s, o, 1 + BM_CALIB_CHAIN);
	if (buf == NULL)
		return -1;

	point_init(&p[0], "ref/ptr-nop", 0);
	refs[0] = (struct ref) {.mode = FLW_CHAINED, .fn = ref_nop, .p = &p[0]};
	for (i = 0; i < BM_CALIB_CHAIN; i++) {
		point_init(&p[1 + i], "ref/ptr-imul-chain", chain_k[i]);
		refs[1 + i] = (struct ref) {.mode = FLW_CHAINED,
			.fn = ptr_imul_chain[i],
			.p = &p[1 + i]};
	}
	sweep(pmu, c, refs, 1 + BM_CALIB_CHAIN, o, &s);
	fit_sweep(p, 1 + BM_CALIB_CHAIN, o->imul_latency, pmu->nctr > BM_CTR_CYCLES, f);
	fit_alt(p, 1 + BM_CALIB_CHAIN, o->imul_latency, pmu->nctr > BM_CTR_CYCLES, &alt);
	risk_set(risk, f, &alt);

	free(buf);
	return 0;
}
