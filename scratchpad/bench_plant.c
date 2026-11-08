/* scratch: a snippet whose work is set at build time, for a planted slowdown */
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

/* WORK dependent multiply-adds: cost proportional to WORK */
static uint64_t
work(const void *in, void *st)
{
	uint64_t x;
	(void)st;
	memcpy(&x, in, 8);
	for (int i = 0; i < WORK; i++)
		__asm__("imul %1, %0" : "+r"(x) : "r"(0x9e3779b97f4a7c15ull));
	return x;
}

static const flw_impl impls[] = {{.name = "work", .fn = work}};
static const flw_bench b = {.name = "Plant",
	.mode = FLW_THROUGHPUT,
	.gen = gen,
	.item_size = 8,
	.count = 1024,
	.impls = impls,
	.nimpls = 1};
FLW_REGISTER(b);

int
main(int argc, char **argv)
{
	return flw_main(argc, argv);
}
