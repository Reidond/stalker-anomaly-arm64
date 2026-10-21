# Mini preprocessor: evaluates #if/#ifdef chains for the R4 (USE_DX11) config, returns live code per file.
import re, os, sys, collections

ROOT = sys.argv[1] if len(sys.argv) > 1 else '.'
LAY = os.path.join(ROOT, 'src/Layers')
DIRS = ['xrRender', 'xrRenderDX10', 'xrRenderPC_R4']

def strip_comments(s):
    out = []; i = 0; n = len(s); st = 0
    while i < n:
        c = s[i]
        if st == 0:
            if c == '/' and i+1 < n and s[i+1] == '/':
                j = s.find('\n', i)
                if j < 0: j = n
                i = j; continue
            if c == '/' and i+1 < n and s[i+1] == '*':
                j = s.find('*/', i+2)
                if j < 0: j = n-2
                out.append(re.sub(r'[^\n]', ' ', s[i:j+2])); i = j+2; continue
            if c == '"':
                st = 1; out.append(c); i += 1; continue
            if c == "'":
                st = 2; out.append(c); i += 1; continue
            out.append(c); i += 1
        elif st == 1:
            if c == '\\' and i+1 < n:
                out.append(s[i:i+2]); i += 2; continue
            if c == '"': st = 0; out.append(c)
            elif c == '\n': st = 0; out.append(c)
            else: out.append(c)
            i += 1
        else:
            if c == '\\' and i+1 < n:
                out.append('  '); i += 2; continue
            if c == "'": st = 0; out.append(c)
            elif c == '\n': st = 0; out.append(c)
            else: out.append(' ')
            i += 1
    return ''.join(out)

def load_build_flags():
    d = {}
    p = os.path.join(ROOT, 'src/build_config_defines.h')
    for l in open(p, errors='replace'):
        m = re.match(r'\s*#\s*define\s+(\w+)\b\s*(.*)', l)
        if m and not l.lstrip().startswith('//'):
            d[m.group(1)] = '1'
    return d

class PP:
    def __init__(self, defines):
        self.defs = dict(defines)
        self.index = {}
        for dd in DIRS:
            for r, _, fs in os.walk(os.path.join(LAY, dd)):
                for f in fs:
                    if f.endswith(('.h', '.hpp', '.inl', '.cpp')):
                        self.index.setdefault(f.lower(), []).append(os.path.join(r, f))
        self.seen = set()
        self.live = {}          # path -> list[(lineno, text)]
        self.dead = {}          # path -> list[(lineno, text)] (inactive regions)
        self.errs = []

    def evalexpr(self, e, path, ln):
        e = e.strip()
        e = re.sub(r'defined\s*\(\s*(\w+)\s*\)', lambda m: '1' if m.group(1) in self.defs else '0', e)
        e = re.sub(r'defined\s+(\w+)', lambda m: '1' if m.group(1) in self.defs else '0', e)
        def rep(m):
            w = m.group(0)
            if re.match(r'\d', w): return w
            v = self.defs.get(w, '0')
            return v if re.fullmatch(r'-?\d+', v or '') else '0'
        e = re.sub(r'[A-Za-z_]\w*|\d\w*', rep, e)
        e = re.sub(r'(\d+)[uUlL]+', r'\1', e)
        e = e.replace('&&', ' and ').replace('||', ' or ')
        e = re.sub(r'!(?!=)', ' not ', e)
        try:
            return bool(eval(e, {}, {}))
        except Exception:
            self.errs.append((path, ln, e)); return False

    def run(self, path):
        path = os.path.normpath(path)
        if path in self.seen or not os.path.exists(path): return
        self.seen.add(path)
        txt = strip_comments(open(path, errors='replace').read())
        lines = txt.split('\n')
        # join continuations
        L = []; i = 0
        while i < len(lines):
            l = lines[i]; start = i
            while l.rstrip().endswith('\\') and i+1 < len(lines):
                i += 1; l = l.rstrip()[:-1] + ' ' + lines[i]
            L.append((start+1, l)); i += 1
        stack = []   # (active_parent, taken, active_now)
        active = True
        live = []; dead = []
        for ln, l in L:
            s = l.strip()
            if s.startswith('#'):
                m = re.match(r'#\s*(\w+)\s*(.*)', s)
                d = m.group(1) if m else ''; rest = m.group(2) if m else ''
                if d in ('if', 'ifdef', 'ifndef'):
                    if d == 'if': c = self.evalexpr(rest, path, ln)
                    elif d == 'ifdef': c = rest.split()[0] in self.defs if rest.split() else False
                    else: c = rest.split()[0] not in self.defs if rest.split() else True
                    stack.append((active, c and active, c)); active = active and c
                elif d == 'elif':
                    pa, taken, _ = stack.pop()
                    c = (not taken) and pa and self.evalexpr(rest, path, ln)
                    stack.append((pa, taken or c, c)); active = pa and c
                elif d == 'else':
                    pa, taken, _ = stack.pop()
                    c = pa and not taken
                    stack.append((pa, True, c)); active = c
                elif d == 'endif':
                    if stack: pa, _, _ = stack.pop(); active = pa
                elif active and d == 'define':
                    mm = re.match(r'(\w+)(\(?)\s*(.*)', rest)
                    if mm and not mm.group(2): self.defs[mm.group(1)] = mm.group(3).strip() or '1'
                elif active and d == 'undef':
                    self.defs.pop(rest.split()[0], None)
                elif active and d == 'include':
                    mm = re.match(r'["<]([^">]+)[">]', rest)
                    if mm:
                        base = os.path.basename(mm.group(1).replace('\\', '/')).lower()
                        for c in self.index.get(base, []):
                            self.run(c)
                continue
            (live if active else dead).append((ln, l))
        self.live[path] = live
        self.dead[path] = dead

def r4_files():
    out = []
    v = open(os.path.join(LAY, 'xrRenderPC_R4/xrRender_R4.vcxproj'), errors='replace').read()
    for m in re.finditer(r'ClCompile Include="([^"]+)"', v):
        p = m.group(1).replace('\\', '/')
        full = os.path.normpath(os.path.join(LAY, 'xrRenderPC_R4', p))
        if os.path.relpath(full, LAY).split('/')[0] in DIRS: out.append(full)
    return out

def run_config(extra):
    defs = load_build_flags()
    defs.update({'USE_DX11': '1', 'WIN32': '1', '_WINDOWS': '1', '_WIN32': '1', 'XRRENDER_R4_EXPORTS': '1', '_MSC_VER': '1930'})
    defs.update(extra)
    pp = PP(defs)
    files = r4_files()
    for f in files: pp.run(f)
    return pp, files
