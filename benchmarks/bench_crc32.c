/* SPDX-License-Identifier: BSD-3-Clause */

/* CRC-32C over 64-byte buffers: bitwise and table-driven. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"

#define CRC_ITEM_SIZE 64
#define CRC32C_POLY 0x82f63b78u /* reflected */

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

static uint64_t
crc_bitwise(const void *in, void *state)
{
	const uint8_t *p = in;
	uint32_t crc = ~0u;
	int i, k;

	(void)state;
	for (i = 0; i < CRC_ITEM_SIZE; i++) {
		crc ^= p[i];
		for (k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (CRC32C_POLY & -(crc & 1));
	}
	return ~crc;
}

static void *
crc_table_setup(const flw_corpus *c, void *arg)
{
	uint32_t *table;
	uint32_t i, crc;
	int k;

	(void)c;
	(void)arg;
	table = malloc(256 * sizeof(*table));
	if (table == NULL)
		return NULL;
	for (i = 0; i < 256; i++) {
		crc = i;
		for (k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (CRC32C_POLY & -(crc & 1));
		table[i] = crc;
	}
	return table;
}

static uint64_t
crc_table(const void *in, void *state)
{
	const uint8_t *p = in;
	const uint32_t *table = state;
	uint32_t crc = ~0u;
	int i;

	for (i = 0; i < CRC_ITEM_SIZE; i++)
		crc = (crc >> 8) ^ table[(crc ^ p[i]) & 0xff];
	return ~crc;
}

static const flw_impl crc_impls[] = {
	{.name = "bitwise", .fn = crc_bitwise},
	{
		.name = "table",
		.fn = crc_table,
		.setup = crc_table_setup,
		.teardown = free,
	},
};

static const flw_bench crc_bench = {
	.name = "Crc32",
	.mode = FLW_THROUGHPUT,
	.gen = gen_random_buffers,
	.item_size = CRC_ITEM_SIZE,
	.count = 1 << 14,
	.impls = crc_impls,
	.nimpls = sizeof(crc_impls) / sizeof(crc_impls[0]),
};
FLW_REGISTER(crc_bench);

int
main(int argc, char **argv)
{
	return flw_main(argc, argv);
}
