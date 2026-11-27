#!/usr/bin/env python3
"""compare.py [limit]: the sound Run-Ahead plays (rec1.bin, the frame from ahead) must equal the sound a normal run plays one frame later (rec0.bin)."""
import struct, sys
NF = 60
limit = int(sys.argv[1]) if len(sys.argv) > 1 else 8
def load(fn):
    d = open(fn, "rb").read()
    n = struct.unpack("<%di" % (2 * NF), d[:2 * NF * 4]); n = [n[:NF], n[NF:]]
    rec = d[2 * NF * 4:]
    out = []
    for m in range(2):
        fr = []
        for f in range(NF):
            off = (m * NF + f) * 2 * 2000 * 2
            fr.append(struct.unpack("<%dh" % (2 * n[m][f]), rec[off:off + n[m][f] * 4]))
        out.append(fr)
    return out, n
a0, n0 = load("rec0.bin"); a1, n1 = load("rec1.bin")
a0 = a0[0]; a1 = a1[1]; n0 = n0[0]; n1 = n1[1]
worst = 0; counts_ok = True
for k in range(NF - 1):
    if n1[k] != n0[k + 1]: counts_ok = False
    n = min(len(a1[k]), len(a0[k + 1]))
    worst = max(worst, max((abs(a1[k][i] - a0[k + 1][i]) for i in range(n)), default=0))
loud = max((abs(v) for fr in a0 for v in fr), default=0)
print("worst difference %d (the sound goes up to %d), sample counts %s" % (worst, loud, "equal" if counts_ok else "DIFFER"))
sys.exit(0 if worst <= limit and counts_ok and loud > 100 else 1)
