/* scratch: step 5 acceptance, chained mode through the full glue */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"

#define MUL 0x9e3779b97f4a7c15ull

static int
gen(flw_corpus *c, uint64_t seed, size_t count, void *arg)
{
	uint64_t *w = c->items;
	(void)arg;
	for (size_t i = 0; i < count; i++)
		w[i] = seed += MUL;
	return 0;
}

/* word in/out: k dependent imul */
#define WCHAIN(k)                                                                     \
	static uint64_t w_##k(uint64_t x, void *st)                                   \
	{                                                                             \
		(void)st;                                                             \
		__asm__(".rept " #k "\n\timul %1, %0\n\t.endr" : "+r"(x) : "r"(MUL)); \
		return x;                                                             \
	}
/* clang-format off */
WCHAIN(1)
WCHAIN(2)
WCHAIN(4)
WCHAIN(8)
WCHAIN(16)
/* clang-format on */

/* on a pointer: load the item word, then k dependent imul */
#define ACHAIN(k)                                                                     \
	static uint64_t a_##k(const void *p, void *st)                                \
	{                                                                             \
		uint64_t x;                                                           \
		(void)st;                                                             \
		memcpy(&x, p, 8);                                                     \
		__asm__(".rept " #k "\n\timul %1, %0\n\t.endr" : "+r"(x) : "r"(MUL)); \
		return x;                                                             \
	}
/* clang-format off */
ACHAIN(4)
ACHAIN(16)
/* clang-format on */

/* walk state: the corpus, so each step can pick the next item */
struct walk {
	const uint64_t *items;
	size_t mask;
};

static void *
walk_setup(const flw_corpus *c, void *arg)
{
	struct walk *w = malloc(sizeof(*w));

	(void)arg;
	if (w != NULL) {
		w->items = c->items;
		w->mask = c->count - 1;
	}
	return w;
}

/* a walk inside one call: each item picks the next, 8 steps (ops_per_call = 8) */
static uint64_t
walk_8(uint64_t in, void *state)
{
	const struct walk *w = state;
	int i;

	for (i = 0; i < 8; i++)
		in = w->items[(in >> 32) & w->mask];
	return in;
}

static const flw_impl impls[] = {
	{.name = "imul-1", .chain = w_1},
	{.name = "imul-2", .chain = w_2},
	{.name = "imul-4", .chain = w_4},
	{.name = "imul-8", .chain = w_8},
	{.name = "imul-16", .chain = w_16},
	{.name = "imul-16-R8", .chain = w_16, .ops_per_call = 8},
	{.name = "ptr-imul-4", .fn = a_4},
	{.name = "ptr-imul-16", .fn = a_16},
	{.name = "walk-R8",
		.chain = walk_8,
		.setup = walk_setup,
		.teardown = free,
		.ops_per_call = 8},
};
static const flw_bench b = {.name = "Chain",
	.mode = FLW_CHAINED,
	.gen = gen,
	.item_size = 8,
	.count = 1024,
	.impls = impls,
	.nimpls = sizeof(impls) / sizeof(impls[0])};
FLW_REGISTER(b);

int
main(int argc, char **argv)
{
	return flw_main(argc, argv);
}
