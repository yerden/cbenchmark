/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Public API of the benchmark package. A benchmark file declares its corpus
 * generator and implementations, registers them with FLW_REGISTER() and
 * calls flw_main() from main(). The runner owns timing, scheduling and
 * output.
 */

#ifndef FLW_BENCH_H
#define FLW_BENCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { FLW_THROUGHPUT, FLW_CHAINED } flw_mode;

typedef struct flw_corpus {
	void *items; /* contiguous array of item_size elements */
	size_t item_size;
	size_t count;	    /* power of two, so index wrap is a mask */
	size_t bytes_total; /* working set, reported */
} flw_corpus;

typedef int (*flw_gen_fn)(flw_corpus *c, uint64_t seed, size_t count, void *arg);

/* throughput, or chained with an adapter */
typedef uint64_t (*flw_fn)(const void *in, void *state);
/* chained, word in/out */
typedef uint64_t (*flw_chain_fn)(uint64_t in, void *state);
/*
 * chained adapter: turns the previous output and a corpus item into the
 * input of the next fn call. It must return a valid input for any out.
 */
typedef const void *(*flw_next_fn)(uint64_t out, const void *item);

typedef struct {
	const char *name;				/* "table", "pclmul", ... */
	flw_fn fn;					/* throughput, or chained with next */
	flw_chain_fn chain;				/* chained word in/out, may be NULL */
	flw_next_fn next;				/* chained adapter, may be NULL */
	void *(*setup)(const flw_corpus *c, void *arg); /* may be NULL */
	void (*teardown)(void *state);			/* may be NULL */
	unsigned int ops_per_call;			/* operations per call, R; 0 means 1 */
} flw_impl;

typedef struct {
	const char *name; /* "Crc32" */
	flw_mode mode;	  /* default, overridable by flag */
	flw_gen_fn gen;	  /* corpus generator */
	void *gen_arg;
	size_t item_size;
	size_t count;	       /* corpus items, power of two */
	const flw_impl *impls; /* same semantics, 1..N */
	size_t nimpls;
} flw_bench;

#define FLW_REGISTER(b)                            \
	static const flw_bench * const flw_reg_##b \
		__attribute__((used, section("flw_benches"))) = &(b)

/* sink: the compiler must compute x, and cannot move memory access across */
static inline void
flw_use(uint64_t x)
{
	__asm__ volatile("" : : "g"(x) : "memory");
}

/* compiler memory barrier */
static inline void
flw_clobber(void)
{
	__asm__ volatile("" ::: "memory");
}

/* zero the compiler cannot see through; the CPU breaks the dependency */
static inline uint64_t
flw_zero(void)
{
	uint64_t z;

	__asm__ volatile("xor %k0, %k0" : "=r"(z));
	return z;
}

/* in, unchanged, but made to depend on out (one AND and one OR on the chain) */
static inline uint64_t
flw_chain_dep(uint64_t in, uint64_t out)
{
	return in | (out & flw_zero());
}

/* rte_eal_init(), then the whole runner; returns the process exit code */
int flw_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* FLW_BENCH_H */
