# SPDX-License-Identifier: BSD-3-Clause
"""Statistics over benchmark JSONL files. Python standard library only.

The epoch is the unit: per epoch the median of its kept samples, then the
median of the epoch medians with a bootstrap confidence interval over
epochs. Everything is computed per call first and converted to per op last.
"""

import json
import math
import random

BOOTSTRAP = 2000
SEED = 1
# between-epoch spread this many times the within-epoch spread: layout-sensitive
LAYOUT_FACTOR = 3.0

REJ_RATIO, REJ_CTX, REJ_MIGRATE = 1, 2, 4

# env fields that must match for two runs to be compared
ENV_MATCH = ("cpu", "core_type", "build_type", "c_args", "cf_protection",
             "profile", "page_size", "pmu_mode", "dpdk")
# bench fields that must match
BENCH_MATCH = ("mode", "unroll", "ops_per_call")


def median(v):
    s = sorted(v)
    n = len(s)
    if n == 0:
        return float("nan")
    return s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2


def mad(v):
    m = median(v)
    return median([abs(x - m) for x in v])


def percentile(s, q):
    """q-th percentile (0..100) of a sorted list, linear interpolation."""
    if not s:
        return float("nan")
    pos = (len(s) - 1) * q / 100
    lo = math.floor(pos)
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (pos - lo)


def bootstrap_ci(values, n=BOOTSTRAP, seed=SEED):
    """95% CI of the median of values, resampling values."""
    rng = random.Random(seed)
    k = len(values)
    meds = sorted(median([values[rng.randrange(k)] for _ in range(k)])
                  for _ in range(n))
    return percentile(meds, 2.5), percentile(meds, 97.5)


def bootstrap_ratio(base, new, n=BOOTSTRAP, seed=SEED):
    """Median ratio new/base of the medians, and its 95% CI, resampling
    each side independently; percentiles taken on the log scale."""
    rng = random.Random(seed)
    kb, kn = len(base), len(new)
    logs = []
    for _ in range(n):
        mb = median([base[rng.randrange(kb)] for _ in range(kb)])
        mn = median([new[rng.randrange(kn)] for _ in range(kn)])
        logs.append(math.log(mn / mb))
    logs.sort()
    return (math.exp(percentile(logs, 50)), math.exp(percentile(logs, 2.5)),
            math.exp(percentile(logs, 97.5)))


class Bench:
    """One implementation in one mode: its bench record, epochs and samples."""

    def __init__(self, rec):
        self.rec = rec
        self.name = rec["name"]
        self.epochs = {}    # epoch number -> epoch record
        self.samples = []   # sample records, in file order

    @property
    def ops_per_call(self):
        return self.rec.get("ops_per_call", 1)


class Run:
    """One JSONL file."""

    def __init__(self, path):
        self.path = path
        self.env = {}
        self.calibration = {}   # mode -> calibration record
        self.selftest = {}
        self.benches = {}       # name -> Bench
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line:
                    self._add(json.loads(line))

    def _add(self, r):
        t = r.get("type")
        if t == "env":
            self.env = r
        elif t == "calibration":
            self.calibration[r["mode"]] = r
        elif t == "selftest":
            self.selftest = r
        elif t == "bench":
            self.benches[r["name"]] = Bench(r)
        elif t == "epoch":
            self.benches[r["bench"]].epochs[r["epoch"]] = r
        elif t == "sample":
            self.benches[r["bench"]].samples.append(r)
        # unknown record types are skipped, so the schema can grow

    @property
    def unit(self):
        return "cycles" if self.env.get("pmu") else "tsc"

    @property
    def tsc_hz(self):
        return self.env.get("tsc_hz") or 0


def per_call(sample, unit):
    return sample.get(unit, sample["tsc"]) / sample["iters"]


def epoch_values(bench, unit, kept_only=True):
    """Per epoch, the per-call values of its (kept) samples."""
    by = {}
    for s in bench.samples:
        if kept_only and s["flags"]:
            continue
        by.setdefault(s["epoch"], []).append(per_call(s, unit))
    return [by[e] for e in sorted(by)]


def summarize(bench, unit):
    """Per-call and per-op statistics of one implementation."""
    eps = epoch_values(bench, unit)
    if not eps:
        eps = epoch_values(bench, unit, kept_only=False)
    meds = [median(v) for v in eps]
    head = median(meds)
    lo, hi = bootstrap_ci(meds) if len(meds) > 1 else (head, head)
    within = median([mad(v) for v in eps])
    between = mad(meds)
    rec = bench.rec
    r = bench.ops_per_call
    a = rec.get("a", 0.0) if rec.get("subtract") else 0.0
    rej = {"ratio": 0, "ctx": 0, "migrate": 0}
    for s in bench.samples:
        rej["ratio"] += bool(s["flags"] & REJ_RATIO)
        rej["ctx"] += bool(s["flags"] & REJ_CTX)
        rej["migrate"] += bool(s["flags"] & REJ_MIGRATE)
    return {
        "per_call": head, "ci": (lo, hi),
        "raw_per_op": head / r,
        "corrected_per_op": (head - a) / r if rec.get("subtract") else None,
        "epochs": len(meds), "epoch_medians": meds,
        "between": between, "within": within,
        "layout_sensitive": within > 0 and between > LAYOUT_FACTOR * within,
        "kept": sum(1 for s in bench.samples if not s["flags"]),
        "samples": len(bench.samples), "rejected": rej,
        "harness_bound": rec.get("harness_bound", False),
        "harness_bound_uncertain": rec.get("harness_bound_uncertain", False),
    }


def mismatches(base, new, bb, nb):
    """Fields that differ between two runs (env) or two benches."""
    out = []
    for k in ENV_MATCH:
        if base.env.get(k) != new.env.get(k):
            out.append("%s: %r vs %r" % (k, base.env.get(k), new.env.get(k)))
    if base.unit != new.unit:
        out.append("unit: %s vs %s" % (base.unit, new.unit))
    for k in BENCH_MATCH:
        if bb.rec.get(k) != nb.rec.get(k):
            out.append("%s: %r vs %r" % (k, bb.rec.get(k), nb.rec.get(k)))
    return out


def compare(bb, nb, unit, min_effect=0.01):
    """Ratio new/base of raw per-call medians of epoch medians, its CI, and
    whether it is significant: the CI excludes 1 and the effect is at least
    min_effect. Raw values: a is measured anew in each run, and subtracting
    it would add its noise to the comparison."""
    base = [median(v) for v in epoch_values(bb, unit)]
    new = [median(v) for v in epoch_values(nb, unit)]
    ratio, lo, hi = bootstrap_ratio(base, new)
    significant = (lo > 1 or hi < 1) and abs(ratio - 1) >= min_effect
    return {"ratio": ratio, "ci": (lo, hi), "significant": significant,
            "base": median(base), "new": median(new)}
