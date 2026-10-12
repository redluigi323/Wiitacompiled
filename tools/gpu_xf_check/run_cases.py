# Runs the GPU transform's emitted Cg vertex programs on a real GLSL implementation and compares
# every output with the C++ reference (gpu_xf_reference + gpu_lit_reference).
#
#   build the host tests, then:
#   build-host/tests/mkwv_gpu_xf_cases 200 > cases.json
#   python3 tools/gpu_xf_check/run_cases.py cases.json
#
# Needs NVIDIA's cgc (Cg Toolkit 3.1) on PATH, Python 3 with moderngl, and an EGL driver (Mesa's
# llvmpipe is enough, no GPU needed). Cg -> GLSL with cgc, then transform feedback of the outputs.
import json, re, struct, subprocess, sys, os, tempfile
import moderngl
TMP = tempfile.mkdtemp()
cases = json.load(open(sys.argv[1]))
ctx = moderngl.create_standalone_context(backend='egl')
ctx.simple_framebuffer((4, 4)).use()
bad = 0; checked = 0; worst = {}
def note(k, v):
    worst[k] = max(worst.get(k, 0), v)
def close(a, b):
    d = abs(a - b); return d <= 2e-3 or d <= 2e-3 * max(abs(a), abs(b))
for ci, c in enumerate(cases):
    open(os.path.join(TMP, 'c.cg'), 'w').write(c['vs'])
    r = subprocess.run(['cgc', '-profile', 'glslv', '-entry', 'main', os.path.join(TMP, 'c.cg'), '-o', os.path.join(TMP, 'c.glsl')], capture_output=True, text=True)
    if r.returncode != 0:
        print('cgc failed', ci, r.stdout[-500:]); bad += 1; continue
    src = open(os.path.join(TMP, 'c.glsl')).read()
    names = {}
    for m in re.finditer(r'^//var \S+ (\w+)(?:\[\d+\])? :[^:]*: (\w+)(?:\[\d+\])? : [-\d]+ : (\d)', src, re.M):
        names[m.group(1)] = m.group(2)
    body = '\n'.join(l for l in src.split('\n') if not l.startswith('//'))
    body = body.replace('#version 110', '#version 330').replace('\nattribute ', '\nin ')
    outs = ['xf_pos', 'xf_col0', 'xf_col1'] + ['xf_tc%d' % i for i in range(8)]
    decl = ''.join('out vec4 %s;\n' % o for o in outs)
    body = body.replace('#version 330', '#version 330\n' + decl, 1)
    body = re.sub(r'gl_TexCoord\[(\d)\]', r'xf_tc\1', body)
    body = body.replace('gl_FrontSecondaryColor', 'xf_col1').replace('gl_FrontColor', 'xf_col0').replace('gl_Position', 'xf_pos')
    init = ''.join('    %s = vec4(0.0);\n' % o for o in outs)
    body = body.replace('void main()\n{\n', 'void main()\n{\n' + init, 1)
    prog = ctx.program(vertex_shader=body, varyings=outs)
    for k, v in c['u'].items():
        g = names.get(k)
        if g is None or g not in prog: continue
        if len(v) > 4 or k in ('uPn','uNm','uProj'):
            full = prog[g].array_length * 4
            vv = list(v) + [0.0] * max(0, full - len(v))
            prog[g].write(struct.pack('%df' % full, *vv[:full]))
        else:
            prog[g].value = tuple(v)
    verts = c['v']; n = len(verts)
    def col(u): return [((u >> (8 * k)) & 255) / 255.0 for k in range(4)]
    data = {
        'aPosition': [x for v in verts for x in v['a'][0:3]],
        'aNormal': [x for v in verts for x in v['a'][3:6]],
        'aTc0': [x for v in verts for x in v['a'][6:8]],
        'aTc1': [x for v in verts for x in v['a'][8:10]],
        'aColor0': [x for v in verts for x in col(v['c0'])],
        'aColor1': [x for v in verts for x in col(v['c1'])],
        'aMtx': [x for v in verts for x in (float(v['m']), 0.0, 0.0, 0.0)],
    }
    fmt = {'aPosition': '3f', 'aNormal': '3f', 'aTc0': '2f', 'aTc1': '2f', 'aColor0': '4f', 'aColor1': '4f', 'aMtx': '4f'}
    content = []
    for a, vals in data.items():
        g = names.get(a)
        if g is None or g not in prog: continue
        buf = ctx.buffer(struct.pack('%df' % len(vals), *vals))
        content.append((buf, fmt[a], g))
    vao = ctx.vertex_array(prog, content)
    out = ctx.buffer(reserve=n * len(outs) * 16)
    vao.transform(out, mode=moderngl.POINTS, vertices=n)
    res = struct.unpack('%df' % (n * len(outs) * 4), out.read())
    for i, v in enumerate(verts):
        o = res[i * len(outs) * 4:(i + 1) * len(outs) * 4]
        get = lambda name: o[outs.index(name) * 4: outs.index(name) * 4 + 4]
        pos = get('xf_pos')
        checked += 1
        ok = all(close(pos[k], v['clip'][k]) for k in range(4))
        if not ok:
            bad += 1
            if bad < 6: print('case', ci, 'v', i, 'clip', pos, 'want', v['clip'], 'skinned', c['skinned'], 'm', v['m'])
            continue
        for t in range(8):
            u = c['used'][t]
            if not u: continue
            got = get('xf_tc%d' % t); want = v['tc'][t]
            nk = 3 if u == 2 else 2
            if not all(close(got[k], want[k]) for k in range(nk)):
                bad += 1
                if bad < 6: print('case', ci, 'v', i, 'tc', t, got[:nk], 'want', want[:nk])
                break
        for ch, key in ((0, 'xf_col0'), (1, 'xf_col1')):
            if not c['color%d' % ch]: continue
            got = get(key); want = col(v['col'][ch])
            d = max(abs(round(got[k] * 255) - round(want[k] * 255)) for k in range(4))
            note('col', d)
            if d > 1:
                bad += 1
                if bad < 6: print('case', ci, 'v', i, 'colour', ch, [round(x*255) for x in got], 'want', [round(x*255) for x in want])
print('shader check: %d cases, %d vertices, %d mismatches, worst colour step %d' % (len(cases), checked, bad, worst.get('col', 0)))
sys.exit(1 if bad else 0)
