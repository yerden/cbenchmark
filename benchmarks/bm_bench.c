/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Registered benchmarks: selection, corpus, implementation setup, the
 * pointer sweep, the collapse check and adapter calibration, and the epochs
 * of timed batches.
 * Runs on the worker lcore; the records are written afterwards by the main
 * lcore.
 */

#include <alloca.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_common.h>
#include <rte_malloc.h>

#include "bm_internal.h"

/* untimed calls of the collapse check, and the share of distinct inputs */
#define COLLAPSE_CALLS 65536
#define COLLAPSE_DIV 16

/* bounds of the FLW_REGISTER() section, provided by the linker */
extern const flw_bench * const __start_flw_benches[] __attribute__((weak));
extern const flw_bench * const __stop_flw_benches[] __attribute__((weak));

static size_t
bench_count(void)
{
	if (__start_flw_benches == NULL)
		return 0;
	return __stop_flw_benches - __start_flw_benches;
}

const char *
bm_mode_name(flw_mode mode)
{
	return mode == FLW_CHAINED ? "chained" : "throughput";
}

/* "Crc32/throughput/table" */
static void
full_name(char *buf, size_t len, const flw_bench *b, flw_mode mode, const flw_impl *impl)
{
	snprintf(buf, len, "%s/%s/%s", b->name, bm_mode_name(mode), impl->name);
}

static int
selected(const regex_t *re, const char *name)
{
	return re == NULL || regexec(re, name, 0, NULL, 0) == 0;
}

static int
supports(const flw_impl *impl, flw_mode mode)
{
	if (mode == FLW_THROUGHPUT)
		return impl->fn != NULL;
	return impl->chain != NULL || impl->fn != NULL;
}

/* the modes to run b in: its own, or the --mode override; returns how many */
static int
run_modes(const struct bm_ctx *ctx, const flw_bench *b, flw_mode *modes)
{
	switch (ctx->opts.mode) {
	case BM_MODE_THROUGHPUT:
		modes[0] = FLW_THROUGHPUT;
		return 1;
	case BM_MODE_CHAINED:
		modes[0] = FLW_CHAINED;
		return 1;
	case BM_MODE_BOTH:
		modes[0] = FLW_THROUGHPUT;
		modes[1] = FLW_CHAINED;
		return 2;
	case BM_MODE_DEFAULT:
		break;
	}
	modes[0] = b->mode;
	return 1;
}

int
bm_list_benches(const struct bm_ctx *ctx)
{
	char name[BM_NAME_MAX];
	flw_mode modes[2];
	size_t i, j;
	int m, nm;

	for (i = 0; i < bench_count(); i++) {
		const flw_bench *b = __start_flw_benches[i];

		nm = run_modes(ctx, b, modes);
		for (m = 0; m < nm; m++) {
			for (j = 0; j < b->nimpls; j++) {
				if (!supports(&b->impls[j], modes[m]))
					continue;
				full_name(name, sizeof(name), b, modes[m], &b->impls[j]);
				if (selected(ctx->bench_re, name))
					printf("%s\n", name);
			}
		}
	}
	return BM_EXIT_OK;
}

/* a broken definition is a bug in the benchmark file, not a measurement */
static int
check_bench(const flw_bench *b)
{
	size_t j;

	if (b->name == NULL || b->gen == NULL || b->impls == NULL || b->nimpls == 0) {
		fprintf(stderr, "benchmark %s: name, gen and impls are required\n",
			b->name ? b->name : "(null)");
		return -1;
	}
	if (b->item_size == 0 ||
		b->count == 0 ||
		(b->count & (b->count - 1)) != 0 ||
		b->count > SIZE_MAX / b->item_size) {
		fprintf(stderr, "benchmark %s: count must be a power of two, item_size > 0\n",
			b->name);
		return -1;
	}
	for (j = 0; j < b->nimpls; j++) {
		const flw_impl *impl = &b->impls[j];

		if (impl->name == NULL || !supports(impl, b->mode)) {
			fprintf(stderr,
				"benchmark %s: impl %zu needs a name, and for its mode %s: %s\n",
				b->name, j, bm_mode_name(b->mode),
				b->mode == FLW_THROUGHPUT ? "fn" : "chain or fn");
			return -1;
		}
		if (impl->next != NULL && impl->fn == NULL) {
			fprintf(stderr, "benchmark %s: impl %s has next, needs fn\n", b->name,
				impl->name);
			return -1;
		}
		/* word in/out reads the first 8 bytes of every item */
		if (impl->chain != NULL && b->item_size < sizeof(uint64_t)) {
			fprintf(stderr, "benchmark %s: impl %s has chain, needs item_size >= 8\n",
				b->name, impl->name);
			return -1;
		}
	}
	return 0;
}

/* share of rejected samples above which the run warns about the environment */
#define REJECT_WARN 0.10

/*
 * After an epoch: flag the samples whose cycles/TSC ratio is off the
 * epoch's median. With exclude_kernel, interrupt time counts in TSC but
 * not in core cycles, so a drop means an interrupt or a frequency change.
 */
static int
flag_ratio(struct bm_sample *s, size_t n)
{
	double *v, med;
	size_t i;

	v = malloc(n * sizeof(*v));
	if (v == NULL)
		return -1;
	for (i = 0; i < n; i++)
		v[i] = s[i].tsc ? (double)s[i].ctr[BM_CTR_CYCLES] / s[i].tsc : 0;
	med = bm_median_d(v, n);
	for (i = 0; i < n; i++) {
		double ratio = s[i].tsc ? (double)s[i].ctr[BM_CTR_CYCLES] / s[i].tsc : 0;

		if (ratio < med * (1 - BM_RATIO_TOL) || ratio > med * (1 + BM_RATIO_TOL))
			s[i].flags |= BM_REJ_RATIO;
	}
	free(v);
	return 0;
}

/* per call, in the calibration unit: core cycles, or TSC ticks without PMU */
static double
sample_per_call(const struct bm_ctx *ctx, const struct bm_sample *s)
{
	uint64_t v = ctx->calib.unit_cycles ? s->ctr[BM_CTR_CYCLES] : s->tsc;

	return (double)v / (double)s->iters;
}

/*
 * Median per call over all samples, the floor verdict and the per-op value.
 * Throughput: a result on the floor (within the fit tolerance, or below)
 * says only that the snippet costs at most the floor. Chained: a result
 * below the floor lies off the calibration line, where subtracting a is
 * wrong. Both checks are per call; per op divides by R last.
 */
static int
summarize(const struct bm_ctx *ctx, struct bm_result *r)
{
	double *v;
	size_t i;

	v = malloc(r->nsamples * sizeof(*v));
	if (v == NULL)
		return -1;
	for (i = 0; i < r->nsamples; i++) {
		const struct bm_sample *s = &r->samples[i];

		r->rej_ratio += !!(s->flags & BM_REJ_RATIO);
		r->rej_ctx += !!(s->flags & BM_REJ_CTX);
		r->rej_migrate += !!(s->flags & BM_REJ_MIGRATE);
		if (s->flags == 0)
			v[r->kept++] = sample_per_call(ctx, s);
	}
	/* nothing kept: the median of everything, and the warning says so */
	if (r->kept == 0)
		for (i = 0; i < r->nsamples; i++)
			v[i] = sample_per_call(ctx, &r->samples[i]);
	r->per_call = bm_median_d(v, r->kept ? r->kept : r->nsamples);
	free(v);

	r->harness_bound = bm_calib_bound(r->mode, r->per_call, r->floor);
	r->harness_bound_uncertain = r->harness_bound !=
				     bm_calib_bound(r->mode, r->per_call, r->risk.floor_alt);
	r->per_op = (r->per_call - (r->subtract ? r->a : 0)) / r->ops_per_call;
	if (r->subtract && r->risk.a_uncertain)
		r->per_op_err = (r->risk.a_alt > r->a ? r->risk.a_alt - r->a :
							r->a - r->risk.a_alt) /
				r->ops_per_call;
	return 0;
}

/* a chained sweep on a pointer: its slope and confirmation, as at startup */
static void
check_sweep(struct bm_ctx *ctx, const char *what, const struct bm_calib_fit *f,
	const struct bm_calib_risk *risk)
{
	if (ctx->calib.unit_cycles && (f->slope < f->expected * (1 - BM_CALIB_SLOPE_TOL) ||
					      f->slope > f->expected * (1 + BM_CALIB_SLOPE_TOL)))
		bm_warn(ctx, "%s: calibration slope %.3f, expected %.2f +- %.0f%%", what, f->slope,
			f->expected, BM_CALIB_SLOPE_TOL * 100);
	bm_warn_risk(ctx, what, risk, ctx->opts.subtract != BM_SUB_NONE);
}

/*
 * Chained on a pointer through the runner's pointer dependency: the glue is
 * the same for every implementation, so it is swept once, by the first one
 * that needs it. The references never read the corpus, so any corpus does.
 * Returns -1 if out of memory.
 */
static int
prepare_pointer(struct bm_ctx *ctx, const struct bm_job *job)
{
	struct bm_calib *cal = &ctx->calib;

	if (cal->pointer_done)
		return 0;
	if (bm_calib_pointer(&ctx->clock.pmu, job->corpus, NULL, NULL, &ctx->calib_opts, cal->ptr,
		    &cal->pointer, &cal->pointer_risk) != 0)
		return -1;
	cal->pointer_done = 1;
	check_sweep(ctx, "chained mode on a pointer", &cal->pointer, &cal->pointer_risk);
	return 0;
}

/*
 * Chained through an adapter: check that next() keeps feeding varied
 * inputs, then calibrate the glue through this adapter. Returns 1 to go
 * on, 0 if the implementation is skipped, -1 if out of memory.
 */
static int
prepare_adapter(struct bm_ctx *ctx, const struct bm_job *job, struct bm_result *r)
{
	size_t want = job->corpus->count < COLLAPSE_CALLS ? job->corpus->count : COLLAPSE_CALLS;
	char what[BM_NAME_MAX + 16];
	long distinct;

	want = want / COLLAPSE_DIV ? want / COLLAPSE_DIV : 1;
	distinct = bm_job_distinct_inputs(job, COLLAPSE_CALLS);
	if (distinct < 0)
		return -1;
	r->distinct_inputs = distinct;
	if ((size_t)distinct < want) {
		bm_warn(ctx,
			"%s: chain collapses: next() gave %ld distinct inputs in %d calls, "
			"expected at least %zu",
			r->name, distinct, COLLAPSE_CALLS, want);
		ctx->failed = 1;
		return 0;
	}

	if (bm_calib_pointer(&ctx->clock.pmu, job->corpus, job->next, job->state, &ctx->calib_opts,
		    r->adapter_pts, &r->adapter_fit, &r->risk) != 0)
		return -1;
	r->adapter = 1;
	snprintf(what, sizeof(what), "%s, its adapter", r->name);
	check_sweep(ctx, what, &r->adapter_fit, &r->risk);
	return 1;
}

/* the harness cost and floor that apply to r */
static void
pick_calibration(const struct bm_ctx *ctx, struct bm_result *r)
{
	const struct bm_calib_fit *f;

	if (r->mode == FLW_THROUGHPUT) {
		f = &ctx->calib.throughput;
		r->risk = ctx->calib.throughput_risk;
	} else if (r->adapter) {
		f = &r->adapter_fit; /* r->risk set by prepare_adapter() */
	} else if (r->pointer) {
		f = &ctx->calib.pointer;
		r->risk = ctx->calib.pointer_risk;
	} else {
		f = &ctx->calib.chained;
		r->risk = ctx->calib.chained_risk;
	}
	r->a = f->a;
	r->floor = f->floor;
	r->subtract = ctx->opts.subtract == BM_SUB_ALL ||
		      (ctx->opts.subtract == BM_SUB_CHAINED && r->mode == FLW_CHAINED);
}

/*
 * Layout per epoch: the corpus starts at a random cache line in its first
 * page, plus a random page out of SLACK_PAGES; the stack moves down by a
 * random number of cache lines, up to one page.
 */
#define LAYOUT_PAGE 4096
#define SLACK_PAGES 16
#define LINE 64

/* the corpus of one epoch, placed at offset in its allocation */
struct placement {
	void *mem;
	flw_corpus c;
};

/* allocate and generate; returns 1 on success, 0 if gen failed, -1 if out of memory */
static int
place_corpus(struct bm_ctx *ctx, const flw_bench *b, size_t offset, struct placement *pl)
{
	size_t bytes = b->item_size * b->count;

	pl->mem = rte_malloc_socket("bm_corpus", bytes + (SLACK_PAGES + 1) * LAYOUT_PAGE,
		LAYOUT_PAGE, ctx->worker.socket);
	if (pl->mem == NULL)
		return -1;
	pl->c = (flw_corpus) {
		.items = (char *)pl->mem + offset,
		.item_size = b->item_size,
		.count = b->count,
		.bytes_total = bytes,
	};
	/* the same seed every epoch: the same bytes at a new address */
	if (b->gen(&pl->c, ctx->opts.seed, b->count, b->gen_arg) != 0) {
		rte_free(pl->mem);
		return 0;
	}
	return 1;
}

/* per-epoch warm-up, then K batches; samples flagged as they are taken */
static __attribute__((noinline)) void
run_batches(struct bm_ctx *ctx, const struct bm_job *job, struct bm_result *r, unsigned int e)
{
	const struct bm_pmu *pmu = &ctx->clock.pmu;
	struct bm_thread_mark m0, m1;
	struct bm_sample *s;
	struct bm_mark d;
	size_t base = 0, n = r->iters;
	unsigned int k;

	for (k = 0; k < BM_BENCH_WARMUP; k++) {
		bm_job_run(pmu, job, base, n, &d);
		base += n;
	}
	for (k = 0; k < ctx->opts.samples; k++) {
		bm_thread_mark(&m0);
		bm_job_run(pmu, job, base, n, &d);
		bm_thread_mark(&m1);
		base += n;
		s = &r->samples[r->nsamples++];
		s->epoch = e;
		s->batch = k;
		s->iters = n;
		s->tsc = d.tsc;
		memcpy(s->ctr, d.ctr, sizeof(s->ctr));
		s->ctxsw = (uint32_t)(m1.ctxsw - m0.ctxsw);
		if (s->ctxsw != 0)
			s->flags |= BM_REJ_CTX;
		if (m1.cpu != m0.cpu)
			s->flags |= BM_REJ_MIGRATE;
	}
}

/* run_batches() with the stack moved down by shift bytes */
static __attribute__((noinline)) void
run_batches_shifted(size_t shift, struct bm_ctx *ctx, const struct bm_job *job, struct bm_result *r,
	unsigned int e)
{
	volatile char *pad = alloca(shift + 1);

	pad[0] = 0;
	run_batches(ctx, job, r, e);
	flw_use((uintptr_t)pad);
}

/* the console line of a finished implementation */
static void
report(struct bm_ctx *ctx, const struct bm_result *r)
{
	if (r->kept == 0)
		bm_warn(ctx, "%s: all %zu samples rejected; its result is unfiltered", r->name,
			r->nsamples);
	else if (r->nsamples - r->kept > REJECT_WARN * r->nsamples)
		bm_warn(ctx,
			"%s: %zu of %zu samples rejected (ratio %zu, ctx %zu, migrate %zu): "
			"something on the machine disturbs the measurement",
			r->name, r->nsamples - r->kept, r->nsamples, r->rej_ratio, r->rej_ctx,
			r->rej_migrate);

	fprintf(stderr, "%s: N=%zu, %.2f %s/call", r->name, r->iters, r->per_call,
		ctx->calib.unit_cycles ? "cycles" : "tsc");
	if (r->kept < r->nsamples)
		fprintf(stderr, " (%zu of %zu samples rejected)", r->nsamples - r->kept,
			r->nsamples);
	if (r->harness_bound && !r->harness_bound_uncertain) {
		fprintf(stderr, ", harness-bound\n");
		return;
	}
	fprintf(stderr, ", %.2f/op%s", r->per_op, r->subtract ? " corrected" : "");
	if (r->per_op_err > 0)
		fprintf(stderr, " +- %.2f (calibration uncertain)", r->per_op_err);
	if (r->harness_bound_uncertain)
		fprintf(stderr, ", harness-bound uncertain (floor %.2f or %.2f)", r->risk.floor,
			r->risk.floor_alt);
	fprintf(stderr, "\n");
}

/*
 * E epochs. Each places the corpus at a new offset, sets the implementation
 * up again, measures the timer overhead and runs warm-up and K batches with
 * the stack shifted. The pointer sweep, collapse check, adapter calibration
 * and batch size come from the first epoch. Returns 0 (also when skipped),
 * -1 on a harness error.
 */
static int
run_impl(struct bm_ctx *ctx, const flw_bench *b, struct bm_result *r)
{
	const struct bm_opts *o = &ctx->opts;
	const struct bm_pmu *pmu = &ctx->clock.pmu;
	const flw_impl *impl = &b->impls[r->impl];
	uint64_t target = (uint64_t)(o->batch_ms * 1e-3 * (double)ctx->clock.tsc_hz);
	struct bm_job job = {.mode = r->mode};
	struct placement pl;
	struct bm_epoch *ep;
	unsigned int e;
	int ret;

	if (r->mode == FLW_THROUGHPUT) {
		job.fn = impl->fn;
	} else if (impl->chain != NULL) {
		job.chain = impl->chain;
	} else {
		job.fn = impl->fn;
		job.next = impl->next;
		r->pointer = impl->next == NULL;
	}
	r->ops_per_call = impl->ops_per_call ? impl->ops_per_call : 1;

	r->samples = calloc((size_t)o->epochs * o->samples, sizeof(*r->samples));
	r->epochs = calloc(o->epochs, sizeof(*r->epochs));
	if (r->samples == NULL || r->epochs == NULL) {
		fprintf(stderr, "out of memory\n");
		return -1;
	}

	for (e = 0; e < o->epochs; e++) {
		ep = &r->epochs[e];
		ep->corpus_offset = bm_splitmix64(&ctx->layout_rng) % SLACK_PAGES * LAYOUT_PAGE +
				    bm_splitmix64(&ctx->layout_rng) % (LAYOUT_PAGE / LINE) * LINE;
		ep->stack_offset = bm_splitmix64(&ctx->layout_rng) % (LAYOUT_PAGE / LINE) * LINE;

		ret = place_corpus(ctx, b, ep->corpus_offset, &pl);
		if (ret < 0) {
			fprintf(stderr, "out of memory\n");
			return -1;
		}
		if (ret == 0) {
			bm_warn(ctx, "%s: corpus generator failed, skipped", r->name);
			ctx->failed = 1;
			return 0;
		}
		r->corpus_bytes = pl.c.bytes_total;
		job.corpus = &pl.c;
		job.state = NULL;
		if (impl->setup != NULL) {
			job.state = impl->setup(&pl.c, b->gen_arg);
			if (job.state == NULL) {
				bm_warn(ctx, "%s: setup failed, skipped", r->name);
				ctx->failed = 1;
				rte_free(pl.mem);
				return 0;
			}
		}

		ret = 1;
		if (e == 0) {
			if (job.next != NULL)
				ret = prepare_adapter(ctx, &job, r);
			else if (r->pointer && prepare_pointer(ctx, &job) != 0)
				ret = -1;
			if (ret > 0) {
				pick_calibration(ctx, r);
				r->iters = bm_job_size(pmu, &job, target);
			}
		}
		if (ret > 0)
			ret = bm_timer_measure(pmu, &ep->timer, BM_EPOCH_TIMER_WINDOWS) == 0 ? 1 :
											       -1;
		if (ret > 0)
			run_batches_shifted(ep->stack_offset, ctx, &job, r, e);

		if (impl->teardown != NULL)
			impl->teardown(job.state);
		rte_free(pl.mem);
		if (ret <= 0) {
			if (ret < 0)
				fprintf(stderr, "out of memory\n");
			return ret;
		}
		r->nepochs++;
		if (ctx->calib.unit_cycles &&
			flag_ratio(&r->samples[r->nsamples - o->samples], o->samples) != 0) {
			fprintf(stderr, "out of memory\n");
			return -1;
		}
	}

	r->ran = 1;
	if (summarize(ctx, r) != 0) {
		fprintf(stderr, "out of memory\n");
		return -1;
	}
	report(ctx, r);
	return 0;
}

/* selected implementations of one benchmark, one after another */
static int
run_bench(struct bm_ctx *ctx, const flw_bench *b, struct bm_result *res, size_t nres)
{
	size_t i;
	int ret = 0;

	for (i = 0; i < nres && ret == 0; i++)
		ret = run_impl(ctx, b, &res[i]);
	return ret;
}

int
bm_run_benches(struct bm_ctx *ctx)
{
	struct bm_result *r;
	size_t i, j, first, total = 0;
	char name[BM_NAME_MAX];
	flw_mode modes[2];
	int m, nm;

	for (i = 0; i < bench_count(); i++) {
		if (check_bench(__start_flw_benches[i]) != 0)
			return -1;
		total += 2 * __start_flw_benches[i]->nimpls;
	}
	ctx->results = calloc(total ? total : 1, sizeof(*ctx->results));
	/* layout offsets: reproducible from the seed, different from the corpus stream */
	ctx->layout_rng = ctx->opts.seed ^ 0x6c61796f75747321ull;
	if (ctx->results == NULL) {
		fprintf(stderr, "out of memory\n");
		return -1;
	}

	for (i = 0; i < bench_count(); i++) {
		const flw_bench *b = __start_flw_benches[i];

		first = ctx->nresults;
		nm = run_modes(ctx, b, modes);
		for (m = 0; m < nm; m++) {
			for (j = 0; j < b->nimpls; j++) {
				full_name(name, sizeof(name), b, modes[m], &b->impls[j]);
				if (!selected(ctx->bench_re, name))
					continue;
				if (!supports(&b->impls[j], modes[m])) {
					bm_warn(ctx, "%s: no %s, skipped", name,
						modes[m] == FLW_THROUGHPUT ? "fn" : "chain or fn");
					continue;
				}
				r = &ctx->results[ctx->nresults++];
				r->bench = b;
				r->impl = j;
				r->mode = modes[m];
				snprintf(r->name, sizeof(r->name), "%s", name);
			}
		}
		if (ctx->nresults > first &&
			run_bench(ctx, b, &ctx->results[first], ctx->nresults - first) != 0)
			return -1;
	}
	if (ctx->nresults == 0)
		bm_warn(ctx, "no benchmark selected");
	return 0;
}

void
bm_results_free(struct bm_ctx *ctx)
{
	size_t i;

	for (i = 0; i < ctx->nresults; i++) {
		free(ctx->results[i].samples);
		free(ctx->results[i].epochs);
	}
	free(ctx->results);
	ctx->results = NULL;
	ctx->nresults = 0;
}
