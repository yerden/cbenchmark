/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Environment record: identity (commit, CPU, worker placement, versions,
 * arguments), the clock and counter setup, and what the environment checks
 * (bm_check.c) found.
 */

#include <cpuid.h>
#include <limits.h>
#include <string.h>
#include <sys/utsname.h>

#include <rte_version.h>

#include "bm_internal.h"
#include "bm_version.h"

/* brand string from cpuid 0x80000002..4, trimmed */
static void
cpu_brand(char *buf, size_t len)
{
	unsigned int regs[12];
	unsigned int eax, ebx, ecx, edx, i;
	char *s, *e;

	snprintf(buf, len, "unknown");
	if (__get_cpuid(0x80000000, &eax, &ebx, &ecx, &edx) == 0 || eax < 0x80000004)
		return;
	for (i = 0; i < 3; i++)
		__get_cpuid(0x80000002 + i, &regs[4 * i], &regs[4 * i + 1], &regs[4 * i + 2],
			&regs[4 * i + 3]);

	s = (char *)regs;
	s[sizeof(regs) - 1] = '\0';
	while (*s == ' ')
		s++;
	e = s + strlen(s);
	while (e > s && e[-1] == ' ')
		*--e = '\0';
	snprintf(buf, len, "%s", s);
}

/* "DPDK 25.11.2" -> "25.11.2" */
static const char *
dpdk_version(void)
{
	const char *v = rte_version();

	if (strncmp(v, "DPDK ", 5) == 0)
		v += 5;
	return v;
}

/* counter names, as used for the keys of sample and timer records */
static const char * const ctr_names[BM_CTR_MAX] = {
	[BM_CTR_CYCLES] = "cycles",
	[BM_CTR_INSTR] = "instr",
};

static json_t *
clock_fields(const struct bm_clock *c)
{
	const struct bm_pmu *pmu = &c->pmu;
	json_t *ctrs, *paranoid;
	int i;

	ctrs = json_array();
	for (i = 0; ctrs != NULL && i < pmu->nctr; i++)
		json_array_append_new(ctrs, json_string(ctr_names[i]));
	paranoid = c->perf_paranoid == INT32_MIN ? json_null() : json_integer(c->perf_paranoid);

	/* clang-format off */
	return json_pack("{s:b, s:I, s:I, s:b, s:s, s:s, s:s, s:o?, s:o?}",
			 "invariant_tsc", c->invariant_tsc,
			 "tsc_hz", (json_int_t)c->tsc_hz,
			 "tsc_hz_measured", (json_int_t)c->tsc_hz_measured,
			 "pmu", pmu->mode != BM_PMU_NONE,
			 "pmu_mode", bm_pmu_mode_name(pmu->mode),
			 "pmu_name", pmu->name,
			 "core_type", pmu->core_type,
			 "counters", ctrs,
			 "perf_event_paranoid", paranoid);
	/* clang-format on */
}

json_t *
bm_timer_record(const struct bm_ctx *ctx)
{
	const struct bm_timer *t = &ctx->clock.timer;
	char key[32];
	json_t *rec;
	int i;

	rec = json_pack("{s:s, s:i, s:I, s:I}", "type", "timer", "n", (int)t->n, "tsc",
		(json_int_t)t->tsc, "tsc_mad", (json_int_t)t->tsc_mad);
	for (i = 0; rec != NULL && i < ctx->clock.pmu.nctr; i++) {
		snprintf(key, sizeof(key), "%s_mad", ctr_names[i]);
		if (json_object_set_new(rec, ctr_names[i], json_integer(t->ctr[i])) != 0 ||
			json_object_set_new(rec, key, json_integer(t->ctr_mad[i])) != 0) {
			json_decref(rec);
			return NULL;
		}
	}
	return rec;
}

json_t *
bm_env_record(const struct bm_ctx *ctx)
{
	struct utsname uts;
	char cpu[64];
	json_t *rec, *eal, *clock, *sys;
	int i;

	cpu_brand(cpu, sizeof(cpu));
	if (uname(&uts) != 0)
		snprintf(uts.release, sizeof(uts.release), "unknown");

	eal = json_array();
	if (eal == NULL)
		return NULL;
	for (i = 0; i < ctx->eal_argc; i++)
		json_array_append_new(eal, json_string(ctx->eal_argv[i]));

	/* clang-format off */
	rec = json_pack("{s:s, s:i, s:s, s:s, s:i, s:i, s:i, s:b, s:s, s:s, "
			"s:s, s:s, s:I, s:o, s:O}",
			"type", "env",
			"schema", BM_SCHEMA,
			"git", BM_GIT_DESCRIBE,
			"cpu", cpu,
			"core", ctx->worker.cpu,
			"lcore", (int)ctx->worker.lcore,
			"socket", (int)ctx->worker.socket,
			"on_main_lcore", ctx->worker.on_main,
			"kernel", uts.release,
			"dpdk", dpdk_version(),
			"build_type", BM_BUILDTYPE,
			"profile", ctx->opts.profile,
			"seed", (json_int_t)ctx->opts.seed,
			"eal_args", eal,
			"warnings", ctx->warnings);
	/* clang-format on */

	clock = clock_fields(&ctx->clock);
	sys = bm_sys_fields(ctx);
	if (rec == NULL ||
		clock == NULL ||
		sys == NULL ||
		json_object_update(rec, clock) != 0 ||
		json_object_update(rec, sys) != 0) {
		json_decref(rec);
		rec = NULL;
	}
	json_decref(clock);
	json_decref(sys);
	return rec;
}
