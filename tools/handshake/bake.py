# bake.py <fit .txt> <fit .log>: writes the fitted constants into $HS_SRC's src/anim/animator_impl.h
# (default: this repository). The .log is opt9's output for the same parameters (its "grip pose:" line).
import re, sys
import os
H = os.path.join(os.environ.get('HS_SRC', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')), 'src/anim/animator_impl.h')
p = {}
for l in open(sys.argv[1]):
    if l.startswith('#') or not l.strip(): continue
    k, v = l.split(); p[k] = float(v)
grip = [l for l in open(sys.argv[2]) if l.startswith('grip pose:')][-1]
fingers = re.findall(r'\{([^}]*)\}', grip)
g = [[float(x) for x in f.split(',')] for f in fingers]
s = open(H).read()
f3 = lambda x: ('%.3f' % x).rstrip('0').rstrip('.') if False else '%.3ff' % x
def fp(rows, indent):
    r = ['{' + ', '.join(f3(x) for x in row) + '}' for row in rows]
    return '{' + ', '.join(r[:3]) + ',\n' + indent + ', '.join(r[3:]) + '}'
# grip
s = re.sub(r'(inline const FingerPose& poseShakeGrip\(\) \{\n    static FingerPose p = fpMake\()\{.*?\}\}(\);)',
           lambda m: m.group(1) + fp(g, ' ' * 34) + m.group(2), s, flags=re.S)
# open thumb
s = re.sub(r'(inline const FingerPose& poseShakeOpen\(\) \{\n    static FingerPose p = fpMake\(\{)\{[^}]*\}',
           lambda m: m.group(1) + '{' + ', '.join(f3(p['oT%d' % j]) for j in range(4)) + '}', s)
s = re.sub(r'kShakePitch = [-0-9.]+f', 'kShakePitch = %.3ff' % p['phi'], s)
s = re.sub(r'kShakeYaw = [-0-9.]+f', 'kShakeYaw = %.3ff' % p['dyaw'], s)
s = re.sub(r'kShakeElbow = [-0-9.]+f', 'kShakeElbow = %.3ff' % p['lift'], s)
s = re.sub(r'kShakePump = [-0-9.]+f', 'kShakePump = %.4ff' % p['pump'], s)
s = re.sub(r'shakeAnchor\(\) \{ return vec3\([^)]*\)', 'shakeAnchor() { return vec3(%.4ff, %.4ff, %.4ff)' % (p['cx'], p['cy'], p['cz']), s)
s = re.sub(r'shakeSlide\(\) \{ return vec3\([^)]*\)', 'shakeSlide() { return vec3(%.4ff, %.4ff, 0.0f)' % (p['ox'], p['oy']), s)
open(H, 'w').write(s)
