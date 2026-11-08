/* scratch: reference snippets through the full bench path */
#include <stdint.h>
#include <string.h>

#include "bench.h"

static int
gen(flw_corpus *c, uint64_t seed, size_t count, void *arg)
{
	uint64_t *w = c->items;
	(void)arg;
	for (size_t i = 0; i < count; i++)
		w[i] = seed += 0x9e3779b97f4a7c15ull;
	return 0;
}

static uint64_t
nop(const void *in, void *st)
{
	uint64_t x;
	(void)st;
	memcpy(&x, in, 8);
	return x;
}

#define INDEP(s)                                                                                  \
	static uint64_t indep_##s(const void *in, void *st)                                       \
	{                                                                                         \
		uint64_t x, y;                                                                    \
		(void)st;                                                                         \
		memcpy(&x, in, 8);                                                                \
		__asm__(".rept " #s "\n\timul $0x7f4a7c15, %1, %0\n\t.endr" : "=&r"(y) : "r"(x)); \
		return y;                                                                         \
	}
INDEP(4)
INDEP(16)
INDEP(32)

static const flw_impl impls[] = {
	{.name = "nop", .fn = nop},
	{.name = "imul-indep-4", .fn = indep_4},
	{.name = "imul-indep-16", .fn = indep_16},
	{.name = "imul-indep-32", .fn = indep_32},
};
static const flw_bench b = {.name = "Ref",
	.mode = FLW_THROUGHPUT,
	.gen = gen,
	.item_size = 8,
	.count = 1024,
	.impls = impls,
	.nimpls = 4};
FLW_REGISTER(b);

int
main(int argc, char **argv)
{
	return flw_main(argc, argv);
}
