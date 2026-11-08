import json, sys, statistics as st
from collections import defaultdict
ep = defaultdict(lambda: defaultdict(list)); floor=None; hb={}
for line in open(sys.argv[1]):
    r = json.loads(line)
    if r["type"] == "calibration" and r["mode"] == "throughput":
        floor = r["floor"]; ind = [(p["ref"], round(p["per_op"], 2)) for p in r["points"]]
    if r["type"] == "bench": hb[r["name"]] = r["harness_bound"]
    if r["type"] == "sample":
        v = r.get("cycles", r["tsc"]) / r["iters"]
        ep[r["bench"]][r["epoch"]].append(v)
print("floor", floor, "indep", ind)
for b, es in ep.items():
    meds = [st.median(v) for _, v in sorted(es.items())]
    mads = [st.median([abs(x - st.median(v)) for x in v]) / st.median(v) for v in es.values()]
    m = st.median(meds)
    print(f"{b}: median {m:.2f}  epoch medians {min(meds):.2f}..{max(meds):.2f} "
          f"spread {(max(meds)-min(meds))/m*100:.2f}%  max dev {max(abs(x-m) for x in meds)/m*100:.2f}%  "
          f"within-epoch rel MAD {st.median(mads)*100:.2f}%  harness_bound={hb[b]}")
