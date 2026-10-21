#!/usr/bin/env python3
"""Counts D3D11/DXGI API use in the R4 (USE_DX11) renderer build and prints markdown tables.

Usage: python3 -I tools/d3d11_surface_scan/scan.py <repo root> > tables.md

Scope: the .cpp files listed in xrRender_R4.vcxproj that live under xrRender, xrRenderDX10 or
xrRenderPC_R4, plus every header they reach through #include. A small preprocessor evaluates
#if/#ifdef with USE_DX11 and the flags from build_config_defines.h, so DX9/DX10-only and
disabled regions are not counted. Counts are call sites in source, not runtime frequencies.
"""
import collections, json, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pp as P

P.ROOT = sys.argv[1]
P.LAY = os.path.join(P.ROOT, 'src/Layers')
rel = lambda p: os.path.relpath(p, P.LAY)

rel_pp, cpp_files = P.run_config({'NDEBUG': '1'})
dbg_pp, _ = P.run_config({'DEBUG': '1', '_DEBUG': '1'})

# engine alias -> real D3D11 name, parsed from the USE_DX11 half of DXCommonTypes.h
alias = {}
src = open(os.path.join(P.LAY, 'xrRenderDX10/DXCommonTypes.h'), errors='replace').read()
sec = src[:src.find('#elif\tdefined(USE_DX10)')]
for m in re.finditer(r'typedef\s+(D3D11_\w+|ID3D11\w+)\s+(\w+);', sec):
    alias[m.group(2)] = m.group(1)
for m in re.finditer(r'#define\s+(D3D_\w+)\s+(D3D11_\w+)', sec):
    alias[m.group(1)] = m.group(2)
alias['IID_ID3DShaderReflection'] = 'IID_ID3D11ShaderReflection'
alias['ID3DBlob'] = 'ID3D10Blob'
alias['ID3DInclude'] = 'ID3D10Include'
alias['D3D_SHADER_MACRO'] = 'D3D10_SHADER_MACRO'

TOK = re.compile(r'\b(?:ID3D\w+|IDXGI\w+|D3D\w+|DXGI\w+|IID_\w*(?:D3D|DXGI)\w*|CreateDXGIFactory\w*|PIX_EVENT)\b')
STR = re.compile(r'"(?:[^"\\]|\\.)*"')


def scan(pp):
    cnt = collections.Counter()
    files = collections.defaultdict(collections.Counter)
    for path, lines in pp.live.items():
        if path.endswith('DXCommonTypes.h'):
            continue  # the alias table itself, not a use
        for ln, l in lines:
            if l.lstrip().startswith('#'):
                continue
            for m in TOK.finditer(STR.sub('""', l)):
                t = alias.get(m.group(0), m.group(0))
                cnt[t] += 1
                files[t][rel(path)] += 1
    return cnt, files


cnt, files = scan(rel_pp)
dcnt, _ = scan(dbg_pp)
short = lambda f: os.path.basename(f)


def fmt_files(fc, n=3):
    top = sorted(fc.items(), key=lambda x: -x[1])[:n]
    s = ', '.join(short(f) for f, _ in top)
    return s + (f' +{len(fc) - n}' if len(fc) > n else '')


def table(title, names, with_files=True):
    rows = sorted(((n, cnt[n], len(files[n])) for n in names if n in cnt), key=lambda r: (-r[1], r[0]))
    print(f'\n### {title} ({len(rows)} identifiers, {sum(r[1] for r in rows)} uses)\n')
    print('| Identifier | Uses | Files | Mostly in |')
    print('|---|--:|--:|---|')
    for n, c, f in rows:
        print(f'| `{n}` | {c} | {f} | {fmt_files(files[n]) if with_files else ""} |')


ifaces = [t for t in cnt if re.match(r'ID3D1[01]\w+$', t) or t.startswith('IDXGI') or t == 'ID3DUserDefinedAnnotation']
structs = [t for t in cnt if re.match(r'(D3D1[01]_\w*_DESC\d?|D3D1[01]_(VIEWPORT|SUBRESOURCE_DATA|MAPPED_SUBRESOURCE|SHADER_MACRO|INCLUDE_TYPE)|DXGI_\w*(DESC\d?|MODE_DESC|RATIONAL|GAMMA_CONTROL\w*|RGB))$', t)]
fam = ['BLEND_OP', 'BLEND', 'TEXTURE_ADDRESS', 'COMPARISON', 'STENCIL_OP', 'FILL', 'CULL', 'FILTER', 'USAGE',
       'BIND', 'CPU_ACCESS', 'MAP', 'RESOURCE_MISC', 'CLEAR', 'QUERY', 'RESOURCE_DIMENSION', 'SRV_DIMENSION',
       'DSV_DIMENSION', 'RTV_DIMENSION', 'UAV_DIMENSION', 'PRIMITIVE_TOPOLOGY', 'DEPTH_WRITE_MASK',
       'COLOR_WRITE_ENABLE', 'INPUT', 'CREATE_DEVICE', 'SIT', 'SVT', 'SVC', 'SHADER_PACK', 'FEATURE_LEVEL',
       'DRIVER_TYPE', 'COMMONSHADER', 'REQ', 'STANDARD_MULTISAMPLE', 'SDK_VERSION', 'UAV']
byfam = collections.defaultdict(list)
for t in cnt:
    m = re.match(r'D3D(?:1[01])?_(.*)$', t)
    if not m or t in structs or t in ifaces or t.endswith('_DESC') or t in ('D3D11_SHADER_RESOURCE_VIEW_DESC',):
        continue
    rest = m.group(1)
    for f in sorted(fam, key=len, reverse=True):
        if rest == f or rest.startswith(f + '_'):
            byfam[f].append(t)
            break
    else:
        byfam['(types and other)'].append(t)

print('## Generated tables\n')
print('Identifiers are shown under their real D3D11/D3D10/DXGI name; engine aliases (`ID3DTexture2D`, `D3D_USAGE_DEFAULT`, ...) are folded in.')
table('Interfaces', ifaces)
table('Descriptor and data structs', structs)
for f in sorted(byfam, key=lambda f: -sum(cnt[t] for t in byfam[f])):
    table(f'Enum family `{f}`', byfam[f], with_files=False)
fmts = [t for t in cnt if t.startswith('DXGI_FORMAT')]
table('DXGI formats', fmts, with_files=False)
other_dxgi = [t for t in cnt if t.startswith('DXGI_') and t not in fmts and t not in structs]
table('Other DXGI constants', other_dxgi)
fn = [t for t in cnt if t in ('D3D11CreateDevice', 'CreateDXGIFactory2', 'D3DReflect', 'D3DCompile', 'D3DGetInputSignatureBlob', 'D3DDisassemble') or t.startswith('D3DX11') or t.startswith('D3DX10')]
table('Free functions and D3DX11/D3DX10 helpers', fn)

# method calls
CALL = re.compile(r'((?:[A-Za-z_]\w*(?:\[[^\]]*\])?(?:\.|->))*[A-Za-z_]\w*(?:\[[^\]]*\])?)\s*(?:->|\.)\s*([A-Za-z_]\w*)\s*\(')
calls = collections.defaultdict(collections.Counter)
site_files = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))  # method -> receiver -> file
for path, lines in rel_pp.live.items():
    for ln, l in lines:
        for m in CALL.finditer(l):
            calls[m.group(2)][m.group(1)] += 1
            site_files[m.group(2)][m.group(1)][rel(path)] += 1
CTX = '''VSSetConstantBuffers PSSetShaderResources PSSetShader PSSetSamplers VSSetShader DrawIndexed Draw Map Unmap PSSetConstantBuffers
IASetInputLayout IASetVertexBuffers IASetIndexBuffer DrawIndexedInstanced DrawInstanced GSSetConstantBuffers GSSetShader
IASetPrimitiveTopology VSSetShaderResources VSSetSamplers Begin End GetData SetPredication GSSetShaderResources GSSetSamplers
OMSetRenderTargets OMSetRenderTargetsAndUnorderedAccessViews OMSetBlendState OMSetDepthStencilState SOSetTargets DrawAuto
Dispatch DispatchIndirect RSSetState RSSetViewports RSSetScissorRects CopySubresourceRegion CopyResource UpdateSubresource
CopyStructureCount ClearRenderTargetView ClearUnorderedAccessViewUint ClearUnorderedAccessViewFloat ClearDepthStencilView
GenerateMips ResolveSubresource HSSetShaderResources HSSetShader HSSetSamplers HSSetConstantBuffers DSSetShaderResources
DSSetShader DSSetSamplers DSSetConstantBuffers CSSetShaderResources CSSetUnorderedAccessViews CSSetShader CSSetSamplers
CSSetConstantBuffers OMGetRenderTargets ClearState Flush'''.split()
DEV = '''CreateBuffer CreateTexture1D CreateTexture2D CreateTexture3D CreateShaderResourceView CreateUnorderedAccessView
CreateRenderTargetView CreateDepthStencilView CreateInputLayout CreateVertexShader CreateGeometryShader CreatePixelShader
CreateHullShader CreateDomainShader CreateComputeShader CreateBlendState CreateDepthStencilState CreateRasterizerState
CreateSamplerState CreateQuery CheckFormatSupport'''.split()
SC = '''Present GetBuffer SetFullscreenState ResizeBuffers ResizeTarget GetContainingOutput SetColorSpace1 CheckColorSpaceSupport'''.split()
DXGI = '''EnumAdapters1 EnumOutputs CreateSwapChainForHwnd CheckFeatureSupport GetDisplayModeList GetGammaControlCapabilities SetGammaControl'''.split()
REFL = '''GetConstantBufferByIndex GetResourceBindingDesc GetVariableByIndex'''.split()
OTHER = '''GetBufferPointer GetBufferSize BeginEvent EndEvent GetResource GetType'''.split()

def method_table(title, names, recv_ok):
    rows = []
    for n in names:
        fs = collections.Counter()
        for recv, byfile in site_files.get(n, {}).items():
            if recv_ok(recv):
                fs.update(byfile)
        if fs:
            rows.append((n, sum(fs.values()), fs))
    rows.sort(key=lambda r: (-r[1], r[0]))
    print(f'\n### {title} ({len(rows)} methods, {sum(r[1] for r in rows)} call sites)\n')
    print('| Method | Calls | Files | Mostly in |')
    print('|---|--:|--:|---|')
    for n, c, fs in rows:
        print(f'| `{n}` | {c} | {len(fs)} | {fmt_files(fs)} |')

ctxr = lambda r: r.endswith('pContext') or r in ('pContext', 'context', 'dev')
devr = lambda r: r.endswith('pDevice') or r in ('pDevice', 'device')
scr = lambda r: 'wapChain' in r or 'wapchain' in r or r == 'swapchain3'
dxr = lambda r: any(k in r for k in ('Factory', 'factory', 'Adapter', 'adapter', 'Output', 'output'))
print('\n## Generated call tables\n')
print('Receiver-filtered: a name only counts when called on a D3D-looking receiver (`HW.pContext`, `HW.pDevice`, `m_pSwapChain`, ...).')
method_table('ID3D11DeviceContext', CTX, ctxr)
method_table('ID3D11Device', DEV, devr)
method_table('IDXGISwapChain1/3', SC, scr)
method_table('IDXGIFactory/Adapter/Output', DXGI, dxr)
method_table('ID3D11ShaderReflection*', REFL, lambda r: r.startswith('p') and ('Reflection' in r or 'Table' in r))
method_table('Blob / annotation / resource helpers', OTHER, lambda r: True)
print(f'\n<!-- {len(rel_pp.live)} files scanned, {len(cpp_files)} .cpp roots, debug-only identifiers: {sorted(t for t in dcnt if t not in cnt)} -->')
