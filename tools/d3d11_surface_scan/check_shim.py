#!/usr/bin/env python3
"""Checks that the Metal shim headers declare everything the survey found R4 using.

Usage: python3 -I tools/d3d11_surface_scan/check_shim.py <repo root>

1. Every D3D11/D3D10/DXGI/D3D_ identifier from scan.py compiles against the shim's compat headers
   (as a value, a type, or a pointer-to-type).
2. xrRenderDX10/DXCommonTypes.h (the engine's alias table) compiles with USE_DX11 against the shim.
3. Every interface method scan.py counted is declared in the shim headers.
"""
import os, re, subprocess, sys

root = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
shim = os.path.join(root, 'src/Platform/MetalD3D11')
tables = subprocess.run([sys.executable, '-I', os.path.join(here, 'scan.py'), root], capture_output=True, text=True, check=True).stdout
flags = ['clang++', '-std=c++17', '-fsyntax-only', '-x', 'c++', '-', f'-I{shim}/compat', f'-I{shim}/include', f'-I{root}']
hdr = '#include <d3d11.h>\n#include <d3d11_1.h>\n#include <d3d11_2.h>\n#include <dxgi1_4.h>\n#include <dxgi1_5.h>\n#include <D3DCompiler.h>\n'


def compiles(src):
    return subprocess.run(flags, input=src, capture_output=True, text=True).returncode == 0


ids = sorted(set(re.findall(r'^\| `([A-Za-z0-9_]+)` \|', tables, re.M)))
ids = [i for i in ids if re.match(r'(D3D11_|D3D10_|D3D_|DXGI_|ID3D1|IDXGI|ID3DUser|D3DCOMPILE|IID_)', i)]
missing = [i for i in ids if not any(compiles(hdr + f'void f() {{ {b} }}\n') for b in
           (f'auto v = {i}; (void)v;', f'enum {{ s = sizeof({i}) }};', f'{i}* p = nullptr; (void)p;'))]
print(f'identifiers: {len(ids)} checked, missing: {missing or "none"}')

alias = hdr + '#define USE_DX11 1\nclass dx10State;\n#include "src/Layers/xrRenderDX10/DXCommonTypes.h"\n'
print('DXCommonTypes.h (USE_DX11) against the shim:', 'ok' if compiles(alias) else 'FAILS')

calls = tables[tables.index('## Generated call tables'):]
names = set(re.findall(r'^\| `(\w+)` \| \d+', calls, re.M))
text = ''.join(open(os.path.join(shim, 'include/mtl_d3d11', f)).read() for f in ('d3d11_metal.h', 'dxgi_metal.h', 'd3d11shader_metal.h'))
gone = sorted(n for n in names if not re.search(r'\b' + n + r'\s*\(', text))
print(f'methods: {len(names)} checked, undeclared: {gone or "none"}')
sys.exit(1 if missing or gone else 0)
