/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Runner: EAL init, argument parsing, benchmark selection, worker lcore
 * launch and output. Measurement itself lives in bm_core.c.
 */

#define _GNU_SOURCE /* sched_getcpu() */

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <regex.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_malloc.h>

#include "bench.h"
#include "bm_internal.h"

/* tolerance of the TSC rate cross-check, and how long it sleeps */
#define TSC_CHECK_MS 100
#define TSC_TOLERANCE 0.001
/* empty windows measured for the timer overhead */
#define TIMER_WINDOWS 1000
/* calibration corpus: 8 KiB of words, fits in L1 */
#define CALIB_ITEMS 1024
#define CALIB_WARMUP 5
#define CALIB_BATCHES 15
/* passes over the whole sweep; each point keeps its lowest */
#define CALIB_ROUNDS 3
/* defaults of --epochs and --samples, and their upper bound */
#define DEF_EPOCHS 10
#define DEF_SAMPLES 30
#define MAX_COUNT 100000

void
bm_warn(struct bm_ctx *ctx, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	fprintf(stderr, "warning: %s\n", buf);
	json_array_append_new(ctx->warnings, json_string(buf));
}

void
bm_warn_risk(struct bm_ctx *ctx, const char *what, const struct bm_calib_risk *r, int a_matters)
{
	const char *unit = ctx->calib.unit_cycles ? "cycles" : "TSC ticks";
	double lo = r->floor < r->floor_alt ? r->floor : r->floor_alt;
	double hi = r->floor < r->floor_alt ? r->floor_alt : r->floor;
	char a_part[256] = "", floor_part[256] = "";

	a_matters = a_matters && r->a_uncertain;
	if (!a_matters && !r->floor_uncertain)
		return;
	if (a_matters)
		snprintf(a_part, sizeof(a_part),
			"\n  harness cost a = %.2f %s, could be %.2f"
			"\n  -> corrected results may be off by up to %.2f %s per call",
			r->a, unit, r->a_alt, r->a_alt > r->a ? r->a_alt - r->a : r->a - r->a_alt,
			unit);
	if (r->floor_uncertain)
		snprintf(floor_part, sizeof(floor_part),
			"\n  floor = %.2f %s, could be %.2f"
			"\n  -> results between %.2f and %.2f %s per call may be wrongly marked, "
			"or not marked, harness-bound",
			r->floor, unit, r->floor_alt, lo, hi, unit);
	bm_warn(ctx,
		"calibration uncertain (%s), interference on the core:%s%s"
		"\n  Quiet the machine (idle SMT sibling, no heavy apps) and run again.",
		what, a_part, floor_part);
}

static void
usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [EAL args] -- [bench args]\n"
		"  --bench=REGEX      select benchmarks by name\n"
		"  --out=PATH         JSONL output, '-' for stdout (default)\n"
		"  --seed=S           seed for corpus and layout offsets\n"
		"  --profile=dev|ci   environment policy (default dev)\n"
		"  --pmu=auto|read|none  how to read core cycles (default auto)\n"
		"  --mode=throughput|chained|both  override the benchmarks' modes\n"
		"  --subtract=chained|all|none  where harness cost is subtracted (default chained)\n"
		"  --epochs=E         epochs per implementation (default 10)\n"
		"  --samples=K        batches per epoch (default 30)\n"
		"  --batch-time=MS    target batch duration (default 1)\n"
		"  --ref-latency=imul:N|add:N  override a reference latency\n"
		"  --list             print benchmark names and exit\n",
		prog);
}

/* "imul:N" or "add:N" */
static int
parse_ref_latency(struct bm_opts *opts, const char *arg)
{
	const char *colon = strchr(arg, ':');
	double *lat, v;
	char *end;

	if (colon == NULL)
		return -1;
	if ((size_t)(colon - arg) == 4 && strncmp(arg, "imul", 4) == 0)
		lat = &opts->imul_latency;
	else if ((size_t)(colon - arg) == 3 && strncmp(arg, "add", 3) == 0)
		lat = &opts->add_latency;
	else
		return -1;
	v = strtod(colon + 1, &end);
	if (colon[1] == '\0' || *end != '\0' || !(v > 0))
		return -1;
	*lat = v;
	return 0;
}

/* --epochs, --samples: 1 to MAX_COUNT */
static int
parse_count(unsigned int *v, const char *arg)
{
	unsigned long x;
	char *end;

	errno = 0;
	x = strtoul(arg, &end, 10);
	if (errno != 0 || *arg < '0' || *arg > '9' || *end != '\0' || x == 0 || x > MAX_COUNT)
		return -1;
	*v = x;
	return 0;
}

static int
parse_args(struct bm_opts *opts, int argc, char **argv)
{
	enum {
		OPT_BENCH = 256,
		OPT_OUT,
		OPT_SEED,
		OPT_PROFILE,
		OPT_PMU,
		OPT_MODE,
		OPT_SUBTRACT,
		OPT_EPOCHS,
		OPT_SAMPLES,
		OPT_BATCH,
		OPT_REFLAT,
		OPT_LIST
	};
	static const struct option longopts[] = {
		{"bench", required_argument, NULL, OPT_BENCH},
		{"out", required_argument, NULL, OPT_OUT},
		{"seed", required_argument, NULL, OPT_SEED},
		{"profile", required_argument, NULL, OPT_PROFILE},
		{"pmu", required_argument, NULL, OPT_PMU},
		{"mode", required_argument, NULL, OPT_MODE},
		{"subtract", required_argument, NULL, OPT_SUBTRACT},
		{"epochs", required_argument, NULL, OPT_EPOCHS},
		{"samples", required_argument, NULL, OPT_SAMPLES},
		{"batch-time", required_argument, NULL, OPT_BATCH},
		{"ref-latency", required_argument, NULL, OPT_REFLAT},
		{"list", no_argument, NULL, OPT_LIST},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};
	char *end;
	int opt;

	opts->bench_re = NULL;
	opts->out = "-";
	opts->profile = "dev";
	opts->seed = 1;
	opts->pmu = BM_PMU_RDPMC;
	opts->batch_ms = 1;
	opts->epochs = DEF_EPOCHS;
	opts->samples = DEF_SAMPLES;
	opts->mode = BM_MODE_DEFAULT;
	opts->subtract = BM_SUB_CHAINED;
	opts->imul_latency = 3;
	opts->add_latency = 1;
	opts->list = 0;

	optind = 1;
	while ((opt = getopt_long(argc, argv, "h", longopts, NULL)) != -1) {
		switch (opt) {
		case OPT_BENCH:
			opts->bench_re = optarg;
			break;
		case OPT_OUT:
			opts->out = optarg;
			break;
		case OPT_SEED:
			errno = 0;
			opts->seed = strtoull(optarg, &end, 0);
			/* JSON integers are signed 64-bit */
			if (errno != 0 ||
				*optarg == '\0' ||
				*end != '\0' ||
				opts->seed > INT64_MAX) {
				fprintf(stderr, "invalid --seed: %s\n", optarg);
				return -1;
			}
			break;
		case OPT_PROFILE:
			if (strcmp(optarg, "dev") != 0 && strcmp(optarg, "ci") != 0) {
				fprintf(stderr, "invalid --profile: %s\n", optarg);
				return -1;
			}
			opts->profile = optarg;
			break;
		case OPT_PMU:
			if (strcmp(optarg, "auto") == 0) {
				opts->pmu = BM_PMU_RDPMC;
			} else if (strcmp(optarg, "read") == 0) {
				opts->pmu = BM_PMU_READ;
			} else if (strcmp(optarg, "none") == 0) {
				opts->pmu = BM_PMU_NONE;
			} else {
				fprintf(stderr, "invalid --pmu: %s\n", optarg);
				return -1;
			}
			break;
		case OPT_MODE:
			if (strcmp(optarg, "throughput") == 0) {
				opts->mode = BM_MODE_THROUGHPUT;
			} else if (strcmp(optarg, "chained") == 0) {
				opts->mode = BM_MODE_CHAINED;
			} else if (strcmp(optarg, "both") == 0) {
				opts->mode = BM_MODE_BOTH;
			} else {
				fprintf(stderr, "invalid --mode: %s\n", optarg);
				return -1;
			}
			break;
		case OPT_SUBTRACT:
			if (strcmp(optarg, "chained") == 0) {
				opts->subtract = BM_SUB_CHAINED;
			} else if (strcmp(optarg, "all") == 0) {
				opts->subtract = BM_SUB_ALL;
			} else if (strcmp(optarg, "none") == 0) {
				opts->subtract = BM_SUB_NONE;
			} else {
				fprintf(stderr, "invalid --subtract: %s\n", optarg);
				return -1;
			}
			break;
		case OPT_EPOCHS:
			if (parse_count(&opts->epochs, optarg) != 0) {
				fprintf(stderr, "invalid --epochs: %s\n", optarg);
				return -1;
			}
			break;
		case OPT_SAMPLES:
			if (parse_count(&opts->samples, optarg) != 0) {
				fprintf(stderr, "invalid --samples: %s\n", optarg);
				return -1;
			}
			break;
		case OPT_BATCH:
			opts->batch_ms = strtod(optarg, &end);
			if (*optarg == '\0' || *end != '\0' || !(opts->batch_ms > 0)) {
				fprintf(stderr, "invalid --batch-time: %s\n", optarg);
				return -1;
			}
			break;
		case OPT_REFLAT:
			if (parse_ref_latency(opts, optarg) != 0) {
				fprintf(stderr, "invalid --ref-latency: %s\n", optarg);
				return -1;
			}
			break;
		case OPT_LIST:
			opts->list = 1;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return -1;
		}
	}

	if (optind < argc) {
		fprintf(stderr, "unexpected argument: %s\n", argv[optind]);
		return -1;
	}
	return 0;
}

/* keep the EAL arguments for the env record; rte_eal_init() permutes argv */
static int
save_eal_args(struct bm_ctx *ctx, int argc, char **argv)
{
	int n;

	for (n = 1; n < argc && strcmp(argv[n], "--") != 0; n++)
		;
	ctx->eal_argc = n - 1;
	ctx->eal_argv = calloc(n, sizeof(*ctx->eal_argv));
	if (ctx->eal_argv == NULL)
		return -1;
	memcpy(ctx->eal_argv, argv + 1, (n - 1) * sizeof(*ctx->eal_argv));
	return 0;
}

/* TSC checks, counter setup and timer overhead, on the worker lcore */
static int
setup_clock(struct bm_ctx *ctx)
{
	struct bm_clock *c = &ctx->clock;
	struct bm_pmu *pmu = &c->pmu;
	double dev;

	c->invariant_tsc = bm_invariant_tsc();
	if (!c->invariant_tsc)
		bm_problem(ctx,
			"TSC is not invariant (cpuid 0x80000007): tsc and ns are not reliable");

	c->tsc_hz = rte_get_tsc_hz();
	c->tsc_hz_measured = bm_tsc_hz_measure(TSC_CHECK_MS);
	dev = ((double)c->tsc_hz_measured - (double)c->tsc_hz) / (double)c->tsc_hz;
	if (dev > TSC_TOLERANCE || dev < -TSC_TOLERANCE)
		bm_warn(ctx,
			"TSC rate from DPDK (%" PRIu64
			" Hz) differs from CLOCK_MONOTONIC_RAW (%" PRIu64 " Hz) by %.3f%%",
			c->tsc_hz, c->tsc_hz_measured, dev * 100);

	c->perf_paranoid = bm_perf_paranoid();
	bm_pmu_open(pmu, ctx->opts.pmu, ctx->worker.cpu);
	if (ctx->opts.pmu == BM_PMU_NONE)
		bm_problem(ctx, "PMU disabled by --pmu=none: TSC-only mode");
	else if (pmu->mode == BM_PMU_NONE)
		bm_problem(ctx,
			"no PMU (perf_event_open: %s, perf_event_paranoid=%d): TSC-only mode, "
			"frequency changes and interrupts go undetected; needs perf_event_paranoid "
			"<= 2 or CAP_PERFMON (in Docker: --cap-add=PERFMON)",
			strerror(pmu->open_errno), c->perf_paranoid);
	else if (pmu->mode == BM_PMU_READ && ctx->opts.pmu == BM_PMU_RDPMC)
		bm_warn(ctx, "rdpmc unavailable (cap_user_rdpmc=0): counters are read with "
			     "read(2), timer overhead is higher");
	if (pmu->mode != BM_PMU_NONE && pmu->nctr <= BM_CTR_INSTR)
		bm_warn(ctx, "instructions counter unavailable: no instr/op or IPC");
	if (strcmp(pmu->core_type, "unknown") == 0)
		bm_problem(ctx, "hybrid CPU, but core %d is in neither cpu_core nor cpu_atom",
			ctx->worker.cpu);

	return bm_timer_measure(pmu, &c->timer, TIMER_WINDOWS);
}

static int
calibrate(struct bm_ctx *ctx)
{
	struct bm_calib *cal = &ctx->calib;
	struct bm_calib_opts o = {
		.target_tsc = (uint64_t)(ctx->opts.batch_ms * 1e-3 * (double)ctx->clock.tsc_hz),
		.warmup = CALIB_WARMUP,
		.batches = CALIB_BATCHES,
		.rounds = CALIB_ROUNDS,
		.imul_latency = ctx->opts.imul_latency,
		.add_latency = ctx->opts.add_latency,
	};
	flw_corpus c = {
		.item_size = sizeof(uint64_t),
		.count = CALIB_ITEMS,
		.bytes_total = CALIB_ITEMS * sizeof(uint64_t),
	};
	uint64_t seed = ctx->opts.seed, *w;
	const char *why;
	size_t i;
	int ret, cpu;

	w = rte_malloc_socket("bm_calib", c.bytes_total, RTE_CACHE_LINE_SIZE, ctx->worker.socket);
	if (w == NULL)
		return -1;
	for (i = 0; i < CALIB_ITEMS; i++)
		w[i] = bm_splitmix64(&seed);
	c.items = w;
	bm_check_page(ctx, w);

	ctx->calib_opts = o;
	ret = bm_calibrate(&ctx->clock.pmu, &c, &o, cal);
	cpu = sched_getcpu();
	rte_free(w);
	if (ret != 0)
		return -1;

	if (cal->slope_ok == 0) {
		/* say which part is wrong, as far as the evidence goes */
		if (cpu != ctx->worker.cpu)
			why = "the worker migrated: pinning is wrong";
		else if (cal->ratio_ok == 0)
			why = "the TSC/core-cycle ratio moves: frequency control is wrong";
		else
			why = "frequency is steady and the worker stayed put: suspect a busy SMT "
			      "sibling or other load on the core, else the counters";
		bm_warn(ctx, "self-test: chained imul slope %.3f, expected %.2f +- %.0f%%: %s",
			cal->chained.slope, cal->chained.expected, BM_CALIB_SLOPE_TOL * 100, why);
		if (strcmp(ctx->opts.profile, "ci") == 0)
			ctx->failed = 1;
	} else if (cal->slope_ok < 0) {
		bm_warn(ctx, "self-test skipped: no PMU, calibration is in TSC ticks");
	}
	bm_warn_risk(ctx, "chained mode", &cal->chained_risk, 1);
	bm_warn_risk(ctx, "throughput mode", &cal->throughput_risk,
		ctx->opts.subtract == BM_SUB_ALL);
	if (cal->ratio_ok == 0)
		bm_warn(ctx,
			"self-test: TSC/core-cycle ratio spread %.2f%% across calibration "
			"batches: frequency is moving",
			100 * cal->ratio_mad / cal->ratio_median);
	if (!cal->timer_ok)
		bm_warn(ctx,
			"self-test: timer overhead unstable, repeats spread over %" PRIu64
			" TSC ticks",
			cal->timer_spread);
	return 0;
}

static int
worker_main(void *arg)
{
	struct bm_ctx *ctx = arg;
	struct bm_worker *w = &ctx->worker;
	int ret;

	w->lcore = rte_lcore_id();
	w->socket = rte_lcore_to_socket_id(w->lcore);
	w->cpu = sched_getcpu();
	if (setup_clock(ctx) != 0) {
		fprintf(stderr, "out of memory\n");
		return -1;
	}
	bm_check_start(ctx);
	if (calibrate(ctx) != 0) {
		fprintf(stderr, "out of memory\n");
		return -1;
	}
	ret = bm_run_benches(ctx);
	bm_check_end(ctx);
	return ret;
}

static int
run(struct bm_ctx *ctx)
{
	unsigned int lcore;
	int ret;

	/* the benchmark runs on the first worker lcore from -l */
	lcore = rte_get_next_lcore(-1, 1, 0);
	if (lcore >= RTE_MAX_LCORE) {
		bm_warn(ctx, "no worker lcore, running on the main lcore %u", rte_lcore_id());
		ctx->worker.on_main = 1;
		return worker_main(ctx);
	}

	ret = rte_eal_remote_launch(worker_main, ctx, lcore);
	if (ret != 0) {
		fprintf(stderr, "cannot launch lcore %u: %s\n", lcore, strerror(-ret));
		return -1;
	}
	return rte_eal_wait_lcore(lcore);
}

/* write and release one record; NULL means a builder ran out of memory */
static int
write_one(struct bm_out *out, json_t *rec)
{
	int ret;

	if (rec == NULL) {
		fprintf(stderr, "out of memory\n");
		return -1;
	}
	ret = bm_out_write(out, rec);
	json_decref(rec);
	return ret;
}

static int
write_records(struct bm_ctx *ctx)
{
	static json_t *(* const build[])(const struct bm_ctx *) = {
		bm_env_record,
		bm_timer_record,
		bm_calib_chained_record,
		bm_calib_throughput_record,
		bm_selftest_record,
	};
	struct bm_out out;
	size_t i, j;
	int ret = 0;

	if (bm_out_open(&out, ctx->opts.out) != 0)
		return -1;

	for (i = 0; i < sizeof(build) / sizeof(build[0]) && ret == 0; i++)
		ret = write_one(&out, build[i](ctx));

	/* per implementation: its bench record, then each epoch record and its samples */
	for (i = 0; i < ctx->nresults && ret == 0; i++) {
		const struct bm_result *r = &ctx->results[i];

		if (!r->ran)
			continue;
		ret = write_one(&out, bm_bench_record(ctx, r));
		for (j = 0; j < r->nsamples && ret == 0; j++) {
			if (j % ctx->opts.samples == 0)
				ret = write_one(&out,
					bm_epoch_record(ctx, r, j / ctx->opts.samples));
			if (ret == 0)
				ret = write_one(&out, bm_sample_record(ctx, r, &r->samples[j]));
		}
	}

	if (bm_out_close(&out) != 0)
		ret = -1;
	return ret;
}

int
flw_main(int argc, char **argv)
{
	struct bm_ctx ctx;
	regex_t re;
	int i, ret, status;

	memset(&ctx, 0, sizeof(ctx));
	/* not opened yet; bm_pmu_close() skips negative descriptors */
	for (i = 0; i < BM_CTR_MAX; i++)
		ctx.clock.pmu.fd[i] = -1;
	ctx.warnings = json_array();
	if (ctx.warnings == NULL || save_eal_args(&ctx, argc, argv) != 0) {
		fprintf(stderr, "out of memory\n");
		return BM_EXIT_ERROR;
	}

	ret = rte_eal_init(argc, argv);
	if (ret < 0) {
		fprintf(stderr, "EAL init failed: %s\n", rte_strerror(rte_errno));
		return BM_EXIT_ERROR;
	}
	argc -= ret;
	argv += ret;

	if (parse_args(&ctx.opts, argc, argv) != 0) {
		status = BM_EXIT_ERROR;
		goto out_eal;
	}

	if (ctx.opts.bench_re != NULL &&
		regcomp(&re, ctx.opts.bench_re, REG_EXTENDED | REG_NOSUB) != 0) {
		fprintf(stderr, "invalid --bench regex: %s\n", ctx.opts.bench_re);
		status = BM_EXIT_ERROR;
		goto out_eal;
	}

	ctx.bench_re = ctx.opts.bench_re ? &re : NULL;
	if (ctx.opts.list) {
		status = bm_list_benches(&ctx);
	} else if (run(&ctx) != 0) {
		status = BM_EXIT_ERROR;
	} else if (write_records(&ctx) != 0) {
		status = BM_EXIT_FAIL;
	} else {
		status = ctx.failed ? BM_EXIT_FAIL : BM_EXIT_OK;
	}

	if (ctx.opts.bench_re != NULL)
		regfree(&re);
out_eal:
	bm_results_free(&ctx);
	bm_pmu_close(&ctx.clock.pmu);
	rte_eal_cleanup();
	json_decref(ctx.warnings);
	free(ctx.eal_argv);
	return status;
}
