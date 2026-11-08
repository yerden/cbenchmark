/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * JSONL output: one compact JSON record per line, written with jansson,
 * and the calibration and self-test records.
 */

#include <errno.h>
#include <string.h>

#include "bm_internal.h"

int
bm_out_open(struct bm_out *out, const char *path)
{
	out->path = path;
	if (strcmp(path, "-") == 0) {
		out->f = stdout;
		return 0;
	}

	/* each run truncates its file */
	out->f = fopen(path, "w");
	if (out->f == NULL) {
		fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
		return -1;
	}
	return 0;
}

int
bm_out_write(struct bm_out *out, json_t *rec)
{
	if (json_dumpf(rec, out->f, JSON_COMPACT) != 0 || fputc('\n', out->f) == EOF) {
		fprintf(stderr, "cannot write %s\n", out->path);
		return -1;
	}
	return 0;
}

int
bm_out_close(struct bm_out *out)
{
	int ret;

	if (out->f == stdout)
		ret = fflush(out->f);
	else
		ret = fclose(out->f);
	if (ret != 0) {
		fprintf(stderr, "cannot write %s: %s\n", out->path, strerror(errno));
		return -1;
	}
	return 0;
}

static json_t *
points_append(json_t *arr, const struct bm_calib_point *p, int np)
{
	int i;

	for (i = 0; arr != NULL && i < np; i++) {
		/* clang-format off */
		json_t *pt = json_pack("{s:s, s:i, s:I, s:f, s:f, s:f, s:f, s:f, s:f, s:b}",
				       "ref", p[i].ref,
				       "k", (int)p[i].k,
				       "iters", (json_int_t)p[i].iters,
				       "per_op", p[i].per_op,
				       "spread", p[i].spread,
				       "gap", p[i].gap,
				       "tsc", p[i].tsc,
				       "instr", p[i].instr,
				       "resid", p[i].resid,
				       "on_line", p[i].on_line);
		/* clang-format on */
		if (json_array_append_new(arr, pt) != 0) {
			json_decref(arr);
			return NULL;
		}
	}
	return arr;
}

/* the fields both calibration records share */
static json_t *
calib_common(const struct bm_ctx *ctx, const char *mode, const struct bm_calib_fit *f,
	const struct bm_calib_risk *risk, json_t *points)
{
	const struct bm_calib *cal = &ctx->calib;
	json_t *rec;

	if (points == NULL)
		return NULL;
	/* clang-format off */
	rec = json_pack("{s:s, s:s, s:s, s:f, s:f, s:f, s:f, s:i, s:i, s:i, s:f, s:i, s:f, s:f, s:b, s:b, s:I, s:o}",
			"type", "calibration",
			"mode", mode,
			"unit", cal->unit_cycles ? "cycles" : "tsc",
			"a", f->a,
			"floor", f->floor,
			"slope", f->slope,
			"expected_slope", f->expected,
			"floor_k", (int)f->floor_k,
			"fit_points", f->npts,
			"unroll", BM_UNROLL,
			"batch_ms", ctx->opts.batch_ms,
			"rounds", (int)ctx->calib_opts.rounds,
			"a_alt", risk->a_alt,
			"floor_alt", risk->floor_alt,
			"a_uncertain", risk->a_uncertain,
			"floor_uncertain", risk->floor_uncertain,
			"timer_tsc", (json_int_t)cal->timer.tsc,
			"points", points);
	/* clang-format on */
	if (rec != NULL &&
		cal->unit_cycles &&
		json_object_set_new(rec, "timer_cycles",
			json_integer((json_int_t)cal->timer.ctr[BM_CTR_CYCLES])) != 0) {
		json_decref(rec);
		return NULL;
	}
	return rec;
}

json_t *
bm_calib_chained_record(const struct bm_ctx *ctx)
{
	const struct bm_calib *cal = &ctx->calib;
	json_t *pts, *rec;

	pts = points_append(json_array(), cal->imul, 1 + BM_CALIB_CHAIN);
	pts = points_append(pts, cal->add, BM_CALIB_CHAIN);
	rec = calib_common(ctx, "chained", &cal->chained, &cal->chained_risk, pts);
	if (rec == NULL)
		return NULL;
	/* the add chain checks the fit; a and the floors come from imul */
	if (json_object_set_new(rec, "add_a", json_real(cal->add_fit.a)) != 0 ||
		json_object_set_new(rec, "add_slope", json_real(cal->add_fit.slope)) != 0 ||
		json_object_set_new(rec, "add_expected_slope", json_real(cal->add_fit.expected)) !=
			0) {
		json_decref(rec);
		return NULL;
	}
	return rec;
}

json_t *
bm_calib_throughput_record(const struct bm_ctx *ctx)
{
	const struct bm_calib *cal = &ctx->calib;

	return calib_common(ctx, "throughput", &cal->throughput, &cal->throughput_risk,
		points_append(json_array(), cal->indep, 1 + BM_CALIB_INDEP));
}

static const char *
verdict(int ok)
{
	return ok > 0 ? "pass" : ok == 0 ? "fail" : "skipped";
}

json_t *
bm_selftest_record(const struct bm_ctx *ctx)
{
	const struct bm_calib *cal = &ctx->calib;

	/* clang-format off */
	return json_pack("{s:s, s:s, s:s, s:f, s:f, s:s, s:f, s:f, s:s, s:I, s:I, s:s}",
			 "type", "selftest",
			 "unit", cal->unit_cycles ? "cycles" : "tsc",
			 "slope", verdict(cal->slope_ok),
			 "slope_value", cal->chained.slope,
			 "slope_tol", BM_CALIB_SLOPE_TOL,
			 "ratio", verdict(cal->ratio_ok),
			 "ratio_median", cal->ratio_median,
			 "ratio_mad", cal->ratio_mad,
			 "timer", verdict(cal->timer_ok),
			 "timer_tsc", (json_int_t)cal->timer.tsc,
			 "timer_spread", (json_int_t)cal->timer_spread,
			 "confirmed", verdict(!cal->chained_risk.a_uncertain &&
					      !cal->chained_risk.floor_uncertain &&
					      !cal->throughput_risk.floor_uncertain));
	/* clang-format on */
}

/* an adapter's own chained sweep, like a calibration record */
static json_t *
adapter_calib(const struct bm_result *r)
{
	const struct bm_calib_fit *f = &r->adapter_fit;
	json_t *pts = points_append(json_array(), r->adapter_pts, 1 + BM_CALIB_CHAIN);

	if (pts == NULL)
		return NULL;
	/* clang-format off */
	return json_pack("{s:f, s:f, s:f, s:f, s:i, s:i, s:f, s:f, s:I, s:o}",
			 "a", f->a,
			 "floor", f->floor,
			 "slope", f->slope,
			 "expected_slope", f->expected,
			 "floor_k", (int)f->floor_k,
			 "fit_points", f->npts,
			 "a_alt", r->risk.a_alt,
			 "floor_alt", r->risk.floor_alt,
			 "distinct_inputs", (json_int_t)r->distinct_inputs,
			 "points", pts);
	/* clang-format on */
}

json_t *
bm_bench_record(const struct bm_ctx *ctx, const struct bm_result *r)
{
	json_t *rec, *rej;

	/* clang-format off */
	rec = json_pack("{s:s, s:s, s:s, s:i, s:I, s:i, s:i, s:i, s:i, s:i, s:I, s:I, s:s, s:f, s:f, s:f, s:f, s:b, s:b, s:b}",
			"type", "bench",
			"name", r->name,
			"mode", bm_mode_name(r->mode),
			"impl", (int)r->impl,
			"iters", (json_int_t)r->iters,
			"ops_per_call", (int)r->ops_per_call,
			"unroll", BM_UNROLL,
			"epochs", (int)ctx->opts.epochs,
			"samples", (int)ctx->opts.samples,
			"warmup", BM_BENCH_WARMUP,
			"seed", (json_int_t)ctx->opts.seed,
			"corpus_bytes", (json_int_t)r->corpus_bytes,
			"calibration", r->adapter ? "adapter" : "startup",
			"a", r->a,
			"floor", r->floor,
			"a_alt", r->risk.a_alt,
			"floor_alt", r->risk.floor_alt,
			"subtract", r->subtract,
			"harness_bound", r->harness_bound,
			"harness_bound_uncertain", r->harness_bound_uncertain);
	rej = json_pack("{s:I, s:I, s:I, s:I}",
			"kept", (json_int_t)r->kept,
			"rejected_ratio", (json_int_t)r->rej_ratio,
			"rejected_ctx", (json_int_t)r->rej_ctx,
			"rejected_migrate", (json_int_t)r->rej_migrate);
	/* clang-format on */
	if (rec == NULL) {
		json_decref(rej);
		return NULL;
	}
	/* takes rej in every case */
	if (json_object_update_new(rec, rej) != 0) {
		json_decref(rec);
		return NULL;
	}
	if (rec != NULL &&
		r->adapter &&
		json_object_set_new(rec, "adapter", adapter_calib(r)) != 0) {
		json_decref(rec);
		return NULL;
	}
	return rec;
}

json_t *
bm_epoch_record(const struct bm_ctx *ctx, const struct bm_result *r, unsigned int e)
{
	const struct bm_epoch *ep = &r->epochs[e];
	json_t *rec;

	/* clang-format off */
	rec = json_pack("{s:s, s:s, s:i, s:I, s:I, s:I}",
			"type", "epoch",
			"bench", r->name,
			"epoch", (int)e,
			"corpus_offset", (json_int_t)ep->corpus_offset,
			"stack_offset", (json_int_t)ep->stack_offset,
			"timer_tsc", (json_int_t)ep->timer.tsc);
	/* clang-format on */
	if (rec != NULL &&
		ctx->calib.unit_cycles &&
		json_object_set_new(rec, "timer_cycles",
			json_integer((json_int_t)ep->timer.ctr[BM_CTR_CYCLES])) != 0) {
		json_decref(rec);
		return NULL;
	}
	return rec;
}

json_t *
bm_sample_record(const struct bm_ctx *ctx, const struct bm_result *r, const struct bm_sample *s)
{
	static const char * const ctr_name[BM_CTR_MAX] = {"cycles", "instr"};
	const struct bm_pmu *pmu = &ctx->clock.pmu;
	json_t *rec;
	int c;

	/* clang-format off */
	rec = json_pack("{s:s, s:s, s:i, s:i, s:I, s:I}",
			"type", "sample",
			"bench", r->name,
			"epoch", (int)s->epoch,
			"batch", (int)s->batch,
			"iters", (json_int_t)s->iters,
			"tsc", (json_int_t)s->tsc);
	/* clang-format on */
	/* counters that are not open are left out, as in the timer record */
	for (c = 0; rec != NULL && c < pmu->nctr; c++) {
		if (json_object_set_new(rec, ctr_name[c], json_integer((json_int_t)s->ctr[c])) !=
			0) {
			json_decref(rec);
			return NULL;
		}
	}
	if (rec != NULL &&
		(json_object_set_new(rec, "ctx_sw", json_integer(s->ctxsw)) != 0 ||
			json_object_set_new(rec, "flags", json_integer(s->flags)) != 0)) {
		json_decref(rec);
		return NULL;
	}
	return rec;
}
