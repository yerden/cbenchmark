/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Environment checks: governor, turbo, SMT sibling load, core isolation,
 * core type, page size and build. Every finding is recorded in the env
 * record; a problem warns under the dev profile and fails the run (exit 1)
 * under ci.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_memory.h>

#include "bm_internal.h"

#define SYSFS_CPU "/sys/devices/system/cpu"

/* share of the run the SMT sibling may be busy before it is a problem */
#define SIBLING_BUSY_MAX 0.05

void
bm_problem(struct bm_ctx *ctx, const char *fmt, ...)
{
	char buf[768];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	bm_warn(ctx, "%s", buf);
	if (strcmp(ctx->opts.profile, "ci") == 0)
		ctx->failed = 1;
}

/* first line of a file, without the newline; -1 if unreadable */
static int
read_line(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");
	char *nl;

	if (f == NULL)
		return -1;
	if (fgets(buf, (int)len, f) == NULL) {
		fclose(f);
		return -1;
	}
	fclose(f);
	nl = strchr(buf, '\n');
	if (nl != NULL)
		*nl = '\0';
	return 0;
}

/* is cpu in a cpulist like "1-3,5"? */
static int
in_cpulist(const char *list, int cpu)
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

/* the first cpu in a cpulist other than cpu, or -1 */
static int
other_in_cpulist(const char *list, int cpu)
{
	const char *p = list;
	char *end;
	long lo, hi, c;

	while (*p != '\0') {
		lo = strtol(p, &end, 10);
		if (end == p)
			return -1;
		hi = lo;
		if (*end == '-')
			hi = strtol(end + 1, &end, 10);
		for (c = lo; c <= hi; c++)
			if (c != cpu)
				return (int)c;
		if (*end != ',')
			return -1;
		p = end + 1;
	}
	return -1;
}

/* busy and total jiffies of one cpu from /proc/stat; -1 if not found */
static int
cpu_jiffies(int cpu, unsigned long long *busy, unsigned long long *total)
{
	unsigned long long v[8] = {0};
	char line[256], tag[16];
	FILE *f;
	int i, ret = -1;

	f = fopen("/proc/stat", "r");
	if (f == NULL)
		return -1;
	snprintf(tag, sizeof(tag), "cpu%d ", cpu);
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, tag, strlen(tag)) != 0)
			continue;
		/* user nice system idle iowait irq softirq steal */
		if (sscanf(line + strlen(tag), "%llu %llu %llu %llu %llu %llu %llu %llu", &v[0],
			    &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) >= 4) {
			*total = 0;
			for (i = 0; i < 8; i++)
				*total += v[i];
			*busy = *total - v[3] - v[4];
			ret = 0;
		}
		break;
	}
	fclose(f);
	return ret;
}

/* microcode revision of cpu from /proc/cpuinfo */
static void
microcode(int cpu, char *buf, size_t len)
{
	char line[256];
	int cur = -1;
	FILE *f;

	snprintf(buf, len, "unknown");
	f = fopen("/proc/cpuinfo", "r");
	if (f == NULL)
		return;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (sscanf(line, "processor : %d", &cur) == 1)
			continue;
		if (cur == cpu && strncmp(line, "microcode", 9) == 0) {
			char *v = strchr(line, ':');

			if (v != NULL) {
				snprintf(buf, len, "%s", v + 2);
				buf[strcspn(buf, "\n")] = '\0';
			}
			break;
		}
	}
	fclose(f);
}

/* turbo: 1 on, 0 off, -1 unknown (intel_pstate, else cpufreq boost) */
static int
turbo_state(void)
{
	char buf[16];

	if (read_line(SYSFS_CPU "/intel_pstate/no_turbo", buf, sizeof(buf)) == 0)
		return atoi(buf) == 0;
	if (read_line(SYSFS_CPU "/cpufreq/boost", buf, sizeof(buf)) == 0)
		return atoi(buf) != 0;
	return -1;
}

void
bm_check_start(struct bm_ctx *ctx)
{
	struct bm_sys *sys = &ctx->sys;
	int cpu = ctx->worker.cpu;
	char path[256], buf[256];

	snprintf(path, sizeof(path), SYSFS_CPU "/cpu%d/cpufreq/scaling_governor", cpu);
	if (read_line(path, sys->governor, sizeof(sys->governor)) != 0)
		snprintf(sys->governor, sizeof(sys->governor), "unknown");
	snprintf(path, sizeof(path), SYSFS_CPU "/cpu%d/cpufreq/energy_performance_preference", cpu);
	if (read_line(path, sys->epp, sizeof(sys->epp)) != 0)
		snprintf(sys->epp, sizeof(sys->epp), "unknown");
	sys->turbo = turbo_state();
	microcode(cpu, sys->microcode, sizeof(sys->microcode));

	sys->smt_active = -1;
	if (read_line(SYSFS_CPU "/smt/active", buf, sizeof(buf)) == 0)
		sys->smt_active = atoi(buf);
	sys->sibling = -1;
	snprintf(path, sizeof(path), SYSFS_CPU "/cpu%d/topology/thread_siblings_list", cpu);
	if (read_line(path, buf, sizeof(buf)) == 0)
		sys->sibling = other_in_cpulist(buf, cpu);
	sys->sibling_busy = -1;
	if (sys->sibling >= 0 && cpu_jiffies(sys->sibling, &sys->sib_busy0, &sys->sib_total0) != 0)
		sys->sibling = -1;

	sys->isolated = read_line(SYSFS_CPU "/isolated", buf, sizeof(buf)) == 0 &&
			in_cpulist(buf, cpu);
	sys->nohz_full = read_line(SYSFS_CPU "/nohz_full", buf, sizeof(buf)) == 0 &&
			 in_cpulist(buf, cpu);

	if (strcmp(sys->governor, "unknown") != 0 && strcmp(sys->governor, "performance") != 0)
		bm_problem(ctx,
			"governor of cpu %d is %s, not performance: the frequency moves, expect "
			"ratio rejections",
			cpu, sys->governor);
	if (sys->turbo == 1)
		bm_problem(ctx,
			"turbo is on: the frequency steps as other cores wake, expect ratio "
			"rejections");
	if (!sys->isolated && !sys->nohz_full)
		bm_problem(ctx,
			"cpu %d is not isolated (isolcpus= or nohz_full=): other tasks and "
			"interrupts may run on it",
			cpu);
	if (strcmp(ctx->clock.pmu.core_type, "atom") == 0)
		bm_problem(ctx, "cpu %d is an E-core (atom): results differ from P-cores", cpu);

	if (strcmp(BM_BUILDTYPE, "plain") == 0 || strcmp(BM_BUILDTYPE, "debug") == 0)
		bm_problem(ctx, "build type %s: benchmarks must be built optimized", BM_BUILDTYPE);
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
	bm_problem(ctx, "built with a sanitizer: timings are distorted");
#endif
#ifndef __OPTIMIZE__
	bm_problem(ctx, "built without optimization");
#endif
}

void
bm_check_end(struct bm_ctx *ctx)
{
	struct bm_sys *sys = &ctx->sys;
	unsigned long long busy, total;

	if (sys->sibling < 0 || cpu_jiffies(sys->sibling, &busy, &total) != 0)
		return;
	if (total > sys->sib_total0)
		sys->sibling_busy = (double)(busy - sys->sib_busy0) /
				    (double)(total - sys->sib_total0);
	if (sys->sibling_busy > SIBLING_BUSY_MAX)
		bm_problem(ctx,
			"SMT sibling cpu %d of the worker was busy %.0f%% of the run: it slows "
			"the benchmark without being seen by sample filtering; keep it idle or "
			"take it offline",
			sys->sibling, 100 * sys->sibling_busy);
}

void
bm_check_page(struct bm_ctx *ctx, const void *addr)
{
	const struct rte_memseg_list *msl = rte_mem_virt2memseg_list(addr);

	ctx->sys.page_size = msl != NULL ? msl->page_sz : 0;
}

json_t *
bm_sys_fields(const struct bm_ctx *ctx)
{
	const struct bm_sys *sys = &ctx->sys;
	json_t *turbo, *smt, *sibling, *busy;

	turbo = sys->turbo < 0 ? json_null() : json_boolean(sys->turbo);
	smt = sys->smt_active < 0 ? json_null() : json_boolean(sys->smt_active);
	sibling = sys->sibling < 0 ? json_null() : json_integer(sys->sibling);
	busy = sys->sibling_busy < 0 ? json_null() : json_real(sys->sibling_busy);

	/* clang-format off */
	return json_pack("{s:s, s:o, s:s, s:o, s:o, s:o, s:b, s:b, s:s, s:I, s:s, s:s, s:b}",
			 "governor", sys->governor,
			 "turbo", turbo,
			 "epp", sys->epp,
			 "smt_active", smt,
			 "sibling", sibling,
			 "sibling_busy", busy,
			 "isolated", sys->isolated,
			 "nohz_full", sys->nohz_full,
			 "microcode", sys->microcode,
			 "page_size", (json_int_t)sys->page_size,
			 "compiler", __VERSION__,
			 "c_args", BM_CARGS,
#ifdef __CET__
			 "cf_protection", 1);
#else
			 "cf_protection", 0);
#endif
	/* clang-format on */
}
