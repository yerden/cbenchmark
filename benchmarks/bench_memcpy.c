/* SPDX-License-Identifier: BSD-3-Clause */

/* libc memcpy against a byte-by-byte loop, at several copy sizes. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"

struct copy_state {
	size_t n; /* read at run time, so memcpy stays a call into libc */
	uint8_t *dst;
};

static uint64_t
splitmix64(uint64_t *s)
{
	uint64_t z = (*s += 0x9e3779b97f4a7c15ull);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

static int
gen_random_buffers(flw_corpus *c, uint64_t seed, size_t count, void *arg)
{
	uint8_t *p = c->items;
	size_t n = c->item_size * count;
	size_t i;

	(void)arg;
	for (i = 0; i < n; i += sizeof(uint64_t)) {
		uint64_t v = splitmix64(&seed);

		memcpy(p + i, &v, n - i < sizeof(v) ? n - i : sizeof(v));
	}
	return 0;
}

static void *
copy_setup(const flw_corpus *c, void *arg)
{
	struct copy_state *st;

	(void)arg;
	st = malloc(sizeof(*st));
	if (st == NULL)
		return NULL;
	st->n = c->item_size;
	/* aligned_alloc wants a size that is a multiple of the alignment */
	st->dst = aligned_alloc(64, (c->item_size + 63) & ~(size_t)63);
	if (st->dst == NULL) {
		free(st);
		return NULL;
	}
	return st;
}

static void
copy_teardown(void *state)
{
	struct copy_state *st = state;

	free(st->dst);
	free(st);
}

/*
 * Both return the last copied byte, not the size: in chained mode the next
 * copy then waits until this one is readable.
 */
static uint64_t
copy_libc(const void *in, void *state)
{
	struct copy_state *st = state;

	memcpy(st->dst, in, st->n);
	return st->dst[st->n - 1];
}

/*
 * one byte per step: keep GCC from turning the loop back into a memcpy call
 * or into vector moves
 */
__attribute__((optimize("no-tree-loop-distribute-patterns", "no-tree-vectorize"))) static uint64_t
copy_bytewise(const void *in, void *state)
{
	struct copy_state *st = state;
	const uint8_t *s = in;
	/* locals: a byte store may alias *st, so st->n would be reloaded per byte */
	uint8_t *d = st->dst;
	size_t i, n = st->n;

	for (i = 0; i < n; i++)
		d[i] = s[i];
	return d[n - 1];
}

static const flw_impl copy_impls[] = {
	{
		.name = "memcpy",
		.fn = copy_libc,
		.setup = copy_setup,
		.teardown = copy_teardown,
	},
	{
		.name = "bytewise",
		.fn = copy_bytewise,
		.setup = copy_setup,
		.teardown = copy_teardown,
	},
};

/* one benchmark per copy size; each corpus is 16 KiB, so it stays in L1 */
#define COPY_BENCH(sz)                                                \
	static const flw_bench copy_bench_##sz = {                    \
		.name = "Memcpy" #sz,                                 \
		.mode = FLW_THROUGHPUT,                               \
		.gen = gen_random_buffers,                            \
		.item_size = (sz),                                    \
		.count = (16 * 1024) / (sz),                          \
		.impls = copy_impls,                                  \
		.nimpls = sizeof(copy_impls) / sizeof(copy_impls[0]), \
	};                                                            \
	FLW_REGISTER(copy_bench_##sz)

COPY_BENCH(16);
COPY_BENCH(64);
COPY_BENCH(256);
COPY_BENCH(1024);

int
main(int argc, char **argv)
{
	return flw_main(argc, argv);
}
