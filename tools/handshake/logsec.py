#!/usr/bin/env python3
# logsec.py <log>: armtrace lines inside the handshake parts of the anim viewer self-test log.
import sys
lines = open(sys.argv[1]).read().split('\n')
def idx(s):
    return next(i for i, l in enumerate(lines) if s in l)
a, b = idx('anim viewer: handshake at t=0.508'), idx('White e2-e4')
c, d = idx('selftest mirror:'), idx('selftest handshake with a left-handed')
for name, (i, j) in (('timeline handshake', (a, b)), ('pen handshake selftest', (c, d))):
    tr = [l for l in lines[i:j] if 'armtrace' in l]
    print('%s (log lines %d..%d): %d armtrace lines' % (name, i + 1, j + 1, len(tr)))
    for l in tr[:5]:
        print('   ', l)
pops = [l for l in lines if 'selftest pop' in l]
print('pops:', len(pops))
for l in pops:
    print('   ', l)
