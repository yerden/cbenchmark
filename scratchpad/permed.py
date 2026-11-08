import json, sys, statistics as st
from collections import defaultdict
ep = defaultdict(lambda: defaultdict(list)); rat = defaultdict(lambda: defaultdict(list))
for line in open(sys.argv[1]):
    r = json.loads(line)
    if r["type"] == "sample":
        ep[r["bench"]][r["epoch"]].append(r["cycles"] / r["iters"])
        rat[r["bench"]][r["epoch"]].append(r["cycles"] / r["tsc"])
for b, es in ep.items():
    meds = [st.median(v) for _, v in sorted(es.items())]
    rs = [st.median(v) for _, v in sorted(rat[b].items())]
    allm = st.median(meds)
    ok = [m for m in meds if abs(m - allm) / allm <= 0.01]
    print(f"{b:32s} " + " ".join(f"{m:7.2f}" for m in meds), f" in1%={len(ok)}/10")
    print(f"{'  ratio':32s} " + " ".join(f"{x:7.3f}" for x in rs))
