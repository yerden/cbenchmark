# SPDX-License-Identifier: BSD-3-Clause
"""Tests of bm_stats on synthetic data with known distributions and shifts."""

import os
import random
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bm_stats  # noqa: E402


def make_bench(center, rng, epochs=10, samples=30, epoch_sd=0.003, sample_sd=0.002,
               flags=None, name="X/throughput/a", **rec):
    """A bench whose epoch medians scatter around center by epoch_sd and
    whose samples scatter around their epoch by sample_sd (relative)."""
    rec = dict({"type": "bench", "name": name, "mode": "throughput", "unroll": 1,
                "ops_per_call": 1}, **rec)
    b = bm_stats.Bench(rec)
    iters = 1000
    for e in range(epochs):
        ec = center * (1 + rng.gauss(0, epoch_sd))
        for k in range(samples):
            v = ec * (1 + rng.gauss(0, sample_sd))
            f = flags(e, k) if flags else 0
            b.samples.append({"type": "sample", "bench": name, "epoch": e, "batch": k,
                              "iters": iters, "tsc": int(v * iters),
                              "cycles": int(v * iters), "flags": f})
    return b


class Basics(unittest.TestCase):
    def test_median_mad(self):
        self.assertEqual(bm_stats.median([3, 1, 2]), 2)
        self.assertEqual(bm_stats.median([4, 1, 2, 3]), 2.5)
        self.assertEqual(bm_stats.mad([1, 2, 3, 4, 100]), 1)

    def test_percentile(self):
        s = list(range(101))
        self.assertEqual(bm_stats.percentile(s, 50), 50)
        self.assertAlmostEqual(bm_stats.percentile(s, 2.5), 2.5)

    def test_bootstrap_is_seeded(self):
        v = [random.Random(3).random() for _ in range(10)]
        self.assertEqual(bm_stats.bootstrap_ci(v), bm_stats.bootstrap_ci(v))


class Summary(unittest.TestCase):
    def test_headline_and_ci(self):
        b = make_bench(100.0, random.Random(1))
        s = bm_stats.summarize(b, "cycles")
        self.assertAlmostEqual(s["per_call"], 100.0, delta=0.5)
        self.assertLess(s["ci"][0], 100.0)
        self.assertGreater(s["ci"][1], 100.0)
        self.assertEqual(s["kept"], 300)
        self.assertFalse(s["layout_sensitive"])

    def test_rejected_samples_are_left_out(self):
        # every 5th sample flagged and 50% slower: must not move the headline
        def flags(e, k):
            return bm_stats.REJ_RATIO if k % 5 == 0 else 0
        b = make_bench(100.0, random.Random(2), flags=flags)
        for s in b.samples:
            if s["flags"]:
                s["cycles"] = int(s["cycles"] * 1.5)
        s = bm_stats.summarize(b, "cycles")
        self.assertAlmostEqual(s["per_call"], 100.0, delta=0.5)
        self.assertEqual(s["kept"], 240)
        self.assertEqual(s["rejected"]["ratio"], 60)

    def test_per_op_last(self):
        # chained, R = 8, a = 2 subtracted per call: (per_call - a) / R
        b = make_bench(50.0, random.Random(4), mode="chained", ops_per_call=8,
                       subtract=True, a=2.0, epoch_sd=0, sample_sd=0)
        s = bm_stats.summarize(b, "cycles")
        self.assertAlmostEqual(s["raw_per_op"], 50.0 / 8)
        self.assertAlmostEqual(s["corrected_per_op"], 48.0 / 8)

    def test_layout_sensitive(self):
        b = make_bench(100.0, random.Random(5), epoch_sd=0.05, sample_sd=0.001)
        self.assertTrue(bm_stats.summarize(b, "cycles")["layout_sensitive"])


class Compare(unittest.TestCase):
    def test_planted_slowdown_is_significant(self):
        rng = random.Random(6)
        base, new = make_bench(100.0, rng), make_bench(105.0, rng)
        c = bm_stats.compare(base, new, "cycles")
        self.assertTrue(c["significant"])
        self.assertAlmostEqual(c["ratio"], 1.05, delta=0.01)
        self.assertGreater(c["ci"][0], 1.0)

    def test_small_shift_below_min_effect(self):
        rng = random.Random(7)
        base, new = make_bench(100.0, rng, epoch_sd=0.0005), make_bench(100.5, rng, epoch_sd=0.0005)
        c = bm_stats.compare(base, new, "cycles", min_effect=0.01)
        self.assertGreater(c["ci"][0], 1.0)      # detectable...
        self.assertFalse(c["significant"])       # ...but below the minimum effect

    def test_aa_coverage(self):
        # same distribution on both sides: the CI should contain 1 most of the time
        rng = random.Random(8)
        pairs, hits = 100, 0
        for _ in range(pairs):
            base, new = make_bench(100.0, rng), make_bench(100.0, rng)
            lo, hi = bm_stats.compare(base, new, "cycles")["ci"]
            hits += lo <= 1.0 <= hi
        print("\nA/A: CI contains 1.0 in %d of %d pairs" % (hits, pairs))
        self.assertGreaterEqual(hits, 85)


if __name__ == "__main__":
    unittest.main()
