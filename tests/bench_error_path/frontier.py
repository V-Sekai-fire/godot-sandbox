import csv, os, sys, statistics as st, collections
TOL = float(os.environ.get('NOFAULT_TOL', '1.05'))  # CI runners are noisy; the workflow loosens this
MIN_OK = int(os.environ.get('MIN_OK', '8'))
SLACK_NS = float(os.environ.get('SLACK_NS', '0'))  # absolute noise floor for 5-10 ns cells
rows = list(csv.DictReader(open('results.csv')))
D = collections.defaultdict(list)
for r in rows: D[(int(r['depth']), float(r['p']), r['variant'])].append(float(r['ns_per_call']))
print(f"{'level(depth,p)':<14} {'throw':>9} {'macro_v0':>9} {'macro_v1':>9} {'speedup v0':>10} {'speedup v1':>10}  success v0/v1 (n=8)  regret v0/v1")
tot = collections.defaultdict(int)
bad = []
for d in (2,6,12):
  for p in (1.0,0.05,0.0):
    t = st.median(D[(d,p,'throw')])
    out = {}
    for v in ('macro_v0','macro_v1_cold'):
        xs = D[(d,p,v)]
        if p > 0: s = sum(1 for x in xs if t/x >= 2.0)
        else:     s = sum(1 for x in xs if x <= t*TOL + SLACK_NS)
        out[v] = (st.median(xs), s, (1 if s>=1 else 0) - s/len(xs))
        tot[v] += s
        if s < MIN_OK: bad.append((d, p, v, s))
    a, b = out['macro_v0'], out['macro_v1_cold']
    print(f"d={d:<2} p={p:<5}   {t:9.1f} {a[0]:9.1f} {b[0]:9.1f} {t/a[0]:9.1f}x {t/b[0]:9.1f}x   {a[1]}/8  {b[1]}/8        {a[2]:.2f}/{b[2]:.2f}")
print({k:f"{v}/72" for k,v in tot.items()})
# control: throw must be slower than the macro on every faulting level, or the harness measures nothing
for d in (2,6,12):
    assert st.median(D[(d,1.0,'throw')]) > 5*st.median(D[(d,1.0,'macro_v0')]), 'control failed'
if bad:
    print('FAIL levels below', MIN_OK, '/8:', bad); sys.exit(1)
print('PASS')
