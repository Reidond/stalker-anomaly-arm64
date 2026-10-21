// Shader container (de)serialisation, D3DCompiler entry points and the ID3D11ShaderReflection facade.
#import "mtl_internal.h"

namespace mtl_d3d11
{
// ---------------------------------------------------------------------------------------------------------
// Stage interface of translated MSL: SPIRV-Cross declares the interpolants as members of main0_out / main0_in with
// [[user(locnN)]], e.g. "    float4 v7 [[user(locn7)]];"
// ---------------------------------------------------------------------------------------------------------
namespace
{
bool structBlock(const std::string& msl, const char* structName, size_t* open, size_t* close)
{
    const std::string head = std::string("struct ") + structName;
    size_t p = msl.find(head);
    if (p == std::string::npos)
        return false;
    *open = msl.find('{', p);
    if (*open == std::string::npos)
        return false;
    *close = msl.find("};", *open);
    return *close != std::string::npos;
}

bool parseMember(const std::string& line, Interpolant* out)
{
    const size_t attr = line.find("[[user(locn");
    if (attr == std::string::npos)
        return false;
    out->location = uint32_t(strtoul(line.c_str() + attr + 11, nullptr, 10));
    // "<type> <name> " before the attribute
    size_t end = attr;
    while (end > 0 && isspace(uint8_t(line[end - 1])))
        --end;
    size_t nameBegin = end;
    while (nameBegin > 0 && (isalnum(uint8_t(line[nameBegin - 1])) || line[nameBegin - 1] == '_'))
        --nameBegin;
    size_t typeBegin = 0;
    while (typeBegin < nameBegin && isspace(uint8_t(line[typeBegin])))
        ++typeBegin;
    size_t typeEnd = nameBegin;
    while (typeEnd > typeBegin && isspace(uint8_t(line[typeEnd - 1])))
        --typeEnd;
    if (nameBegin == end || typeEnd == typeBegin)
        return false;
    out->name = line.substr(nameBegin, end - nameBegin);
    out->type = line.substr(typeBegin, typeEnd - typeBegin);
    return true;
}
} // namespace

std::vector<Interpolant> ParseInterpolants(const std::string& msl, const char* structName)
{
    std::vector<Interpolant> out;
    size_t open, close;
    if (!structBlock(msl, structName, &open, &close))
        return out;
    size_t pos = open + 1;
    while (pos < close)
    {
        size_t eol = msl.find('\n', pos);
        if (eol == std::string::npos || eol > close)
            eol = close;
        Interpolant v;
        if (parseMember(msl.substr(pos, eol - pos), &v))
            out.push_back(std::move(v));
        pos = eol + 1;
    }
    return out;
}

std::string PatchFragmentInputs(const std::string& msl, const std::vector<Interpolant>& remove)
{
    size_t open, close;
    if (remove.empty() || !structBlock(msl, "main0_in", &open, &close))
        return msl;
    auto removed = [&](const Interpolant& v) {
        for (const Interpolant& r : remove)
            if (r.location == v.location && r.name == v.name)
                return true;
        return false;
    };
    // 1. drop the members from the input struct
    std::string structText = "{\n";
    size_t kept = 0;
    size_t pos = open + 1;
    while (pos < close)
    {
        size_t eol = msl.find('\n', pos);
        if (eol == std::string::npos || eol > close)
            eol = close;
        const std::string line = msl.substr(pos, eol - pos);
        Interpolant v;
        if (!(parseMember(line, &v) && removed(v)))
        {
            if (line.find_first_not_of(" \t\r") != std::string::npos)
            {
                structText += line + "\n";
                ++kept;
            }
        }
        pos = eol + 1;
    }
    std::string out = msl.substr(0, open) + structText + msl.substr(close);
    // 2. find the entry point body and declare the dropped inputs as zero locals
    size_t fn = out.find("main0(", open);
    size_t body = fn == std::string::npos ? std::string::npos : out.find('{', fn);
    if (body == std::string::npos)
        return msl;
    if (kept == 0)
    {
        // an empty stage_in struct is not allowed as a parameter
        size_t param = out.find("main0_in in [[stage_in]]", fn);
        if (param != std::string::npos && param < body)
        {
            size_t end = param + std::strlen("main0_in in [[stage_in]]");
            if (out.compare(end, 2, ", ") == 0)
                end += 2;
            out.erase(param, end - param);
            body = out.find('{', fn);
        }
    }
    std::string locals;
    for (const Interpolant& r : remove)
        locals += "\n    " + r.type + " " + r.name + "_unwritten = " + r.type + "(0);";
    std::string head = out.substr(0, body + 1) + locals;
    std::string tail = out.substr(body + 1);
    // 3. in.<member> -> <member>_unwritten
    for (const Interpolant& r : remove)
    {
        const std::string needle = "in." + r.name;
        size_t at = 0;
        while ((at = tail.find(needle, at)) != std::string::npos)
        {
            const size_t after = at + needle.size();
            const bool boundaryBefore = at == 0 || !(isalnum(uint8_t(tail[at - 1])) || tail[at - 1] == '_');
            const bool boundaryAfter = after >= tail.size() || !(isalnum(uint8_t(tail[after])) || tail[after] == '_');
            if (boundaryBefore && boundaryAfter)
            {
                tail.replace(at, needle.size(), r.name + "_unwritten");
                at += r.name.size() + 10;
            }
            else
                at = after;
        }
    }
    return head + tail;
}

namespace
{
constexpr uint32_t kMagic = 'XMSL';
constexpr uint32_t kVersion = 1;

class Writer
{
public:
    void u8(uint8_t v) { buf.push_back(v); }
    void u32(uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            buf.push_back(uint8_t(v >> (8 * i)));
    }
    void str(const std::string& s)
    {
        u32(uint32_t(s.size()));
        buf.insert(buf.end(), s.begin(), s.end());
    }
    void bytes(const void* p, size_t n)
    {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        buf.insert(buf.end(), b, b + n);
    }
    std::vector<uint8_t> buf;
};

class Reader
{
public:
    Reader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}
    bool u8(uint8_t* v)
    {
        if (p_ + 1 > end_)
            return fail();
        *v = *p_++;
        return true;
    }
    bool u32(uint32_t* v)
    {
        if (p_ + 4 > end_)
            return fail();
        *v = uint32_t(p_[0]) | uint32_t(p_[1]) << 8 | uint32_t(p_[2]) << 16 | uint32_t(p_[3]) << 24;
        p_ += 4;
        return true;
    }
    bool str(std::string* s)
    {
        uint32_t n;
        if (!u32(&n) || size_t(end_ - p_) < n)
            return fail();
        s->assign(reinterpret_cast<const char*>(p_), n);
        p_ += n;
        return true;
    }
    bool bytes(std::vector<uint8_t>* out, size_t n)
    {
        if (size_t(end_ - p_) < n)
            return fail();
        out->assign(p_, p_ + n);
        p_ += n;
        return true;
    }
    bool skip(size_t n)
    {
        if (size_t(end_ - p_) < n)
            return fail();
        p_ += n;
        return true;
    }
    const uint8_t* cur() const { return p_; }
    size_t left() const { return size_t(end_ - p_); }
    bool ok = true;

private:
    bool fail()
    {
        ok = false;
        return false;
    }
    const uint8_t* p_;
    const uint8_t* end_;
};

void writeReflection(Writer& w, const ShaderReflection& r)
{
    w.u8(uint8_t(r.stage));
    w.str(r.entryPoint);
    w.u32(r.colorOutputs);
    for (int i = 0; i < 3; ++i)
        w.u32(r.threadgroupSize[i]);
    w.u32(uint32_t(r.cbuffers.size()));
    for (const CBuffer& cb : r.cbuffers)
    {
        w.str(cb.name);
        w.u32(cb.d3dSlot);
        w.u32(cb.metalIndex);
        w.u32(cb.sizeBytes);
        w.u32(uint32_t(cb.variables.size()));
        for (const CBufferVariable& v : cb.variables)
        {
            w.str(v.name);
            w.u32(v.offset);
            w.u32(v.size);
            w.u32(uint32_t(v.cls));
            w.u32(uint32_t(v.type));
            w.u32(v.rows);
            w.u32(v.columns);
            w.u32(v.elements);
        }
    }
    w.u32(uint32_t(r.resources.size()));
    for (const ResourceBinding& b : r.resources)
    {
        w.str(b.name);
        w.u32(uint32_t(b.type));
        w.u32(b.d3dSlot);
        w.u32(b.count);
        w.u32(b.metalIndex);
        w.u32(uint32_t(b.dimension));
        w.u32(uint32_t(b.returnType));
        w.u32(b.numSamples);
        w.u8(b.depthTexture ? 1 : 0);
        w.u8(b.comparisonSampler ? 1 : 0);
    }
    w.u32(uint32_t(r.inputs.size()));
    for (const VertexInput& in : r.inputs)
    {
        w.str(in.semantic);
        w.u32(in.semanticIndex);
        w.u32(in.attribute);
        w.u32(in.components);
        w.u32(uint32_t(in.componentType));
    }
}

bool readReflection(Reader& r, ShaderReflection* out)
{
    uint8_t stage = 0;
    uint32_t n = 0;
    if (!r.u8(&stage) || stage >= kShaderStageCount)
        return false;
    out->stage = ShaderStage(stage);
    if (!r.str(&out->entryPoint) || !r.u32(&out->colorOutputs))
        return false;
    for (int i = 0; i < 3; ++i)
        if (!r.u32(&out->threadgroupSize[i]))
            return false;
    if (!r.u32(&n) || n > 64)
        return false;
    out->cbuffers.resize(n);
    for (CBuffer& cb : out->cbuffers)
    {
        uint32_t nv;
        if (!r.str(&cb.name) || !r.u32(&cb.d3dSlot) || !r.u32(&cb.metalIndex) || !r.u32(&cb.sizeBytes) || !r.u32(&nv) ||
            nv > 4096)
            return false;
        cb.variables.resize(nv);
        for (CBufferVariable& v : cb.variables)
        {
            uint32_t cls, type;
            if (!r.str(&v.name) || !r.u32(&v.offset) || !r.u32(&v.size) || !r.u32(&cls) || !r.u32(&type) ||
                !r.u32(&v.rows) || !r.u32(&v.columns) || !r.u32(&v.elements))
                return false;
            v.cls = D3D_SHADER_VARIABLE_CLASS(cls);
            v.type = D3D_SHADER_VARIABLE_TYPE(type);
        }
    }
    if (!r.u32(&n) || n > 1024)
        return false;
    out->resources.resize(n);
    for (ResourceBinding& b : out->resources)
    {
        uint32_t type, dim, ret;
        uint8_t depth, cmp;
        if (!r.str(&b.name) || !r.u32(&type) || !r.u32(&b.d3dSlot) || !r.u32(&b.count) || !r.u32(&b.metalIndex) ||
            !r.u32(&dim) || !r.u32(&ret) || !r.u32(&b.numSamples) || !r.u8(&depth) || !r.u8(&cmp))
            return false;
        b.type = D3D_SHADER_INPUT_TYPE(type);
        b.dimension = D3D_SRV_DIMENSION(dim);
        b.returnType = D3D_RESOURCE_RETURN_TYPE(ret);
        b.depthTexture = depth != 0;
        b.comparisonSampler = cmp != 0;
    }
    if (!r.u32(&n) || n > 64)
        return false;
    out->inputs.resize(n);
    for (VertexInput& in : out->inputs)
    {
        uint32_t ct;
        if (!r.str(&in.semantic) || !r.u32(&in.semanticIndex) || !r.u32(&in.attribute) || !r.u32(&in.components) ||
            !r.u32(&ct))
            return false;
        in.componentType = D3D_REGISTER_COMPONENT_TYPE(ct);
    }
    return true;
}

struct CompilerHook
{
    CompileFn fn = nullptr;
    void* user = nullptr;
};
CompilerHook& hook()
{
    static CompilerHook h;
    return h;
}
} // namespace

std::vector<uint8_t> Serialize(const ShaderBlob& blob)
{
    Writer refl;
    writeReflection(refl, blob.reflection);
    Writer w;
    w.u32(kMagic);
    w.u32(kVersion);
    w.u8(uint8_t(blob.reflection.stage));
    w.u8(uint8_t(blob.kind));
    w.u8(0);
    w.u8(0);
    w.u32(uint32_t(refl.buf.size()));
    w.bytes(refl.buf.data(), refl.buf.size());
    w.u32(uint32_t(blob.payload.size()));
    w.bytes(blob.payload.data(), blob.payload.size());
    return std::move(w.buf);
}

bool IsShaderContainer(const void* data, size_t size)
{
    if (!data || size < 8)
        return false;
    Reader r(static_cast<const uint8_t*>(data), size);
    uint32_t magic = 0, ver = 0;
    return r.u32(&magic) && r.u32(&ver) && magic == kMagic && ver == kVersion;
}

static bool parse(const void* data, size_t size, ShaderBlob* blob, bool wantPayload)
{
    Reader r(static_cast<const uint8_t*>(data), size);
    uint32_t magic = 0, ver = 0, rsize = 0, psize = 0;
    uint8_t stage, kind, pad;
    if (!r.u32(&magic) || !r.u32(&ver) || magic != kMagic || ver != kVersion)
        return false;
    if (!r.u8(&stage) || !r.u8(&kind) || !r.u8(&pad) || !r.u8(&pad) || kind > 1)
        return false;
    if (!r.u32(&rsize) || r.left() < rsize)
        return false;
    Reader rr(r.cur(), rsize);
    if (!readReflection(rr, &blob->reflection))
        return false;
    r.skip(rsize);
    blob->kind = PayloadKind(kind);
    if (!r.u32(&psize) || r.left() < psize)
        return false;
    if (wantPayload)
        return r.bytes(&blob->payload, psize);
    return true;
}

bool Deserialize(const void* data, size_t size, ShaderBlob* out) { return parse(data, size, out, true); }

bool ParseReflection(const void* data, size_t size, ShaderReflection* out)
{
    ShaderBlob b;
    if (!parse(data, size, &b, false))
        return false;
    *out = std::move(b.reflection);
    return true;
}

void SetShaderCompiler(CompileFn fn, void* user)
{
    hook().fn = fn;
    hook().user = user;
}

// ---------------------------------------------------------------------------------------------------------
// Blob
// ---------------------------------------------------------------------------------------------------------
namespace
{
class BlobImpl final : public ComObject<ID3D10Blob>
{
public:
    explicit BlobImpl(std::vector<uint8_t> d) : data(std::move(d)) {}
    void* GetBufferPointer() override { return data.data(); }
    SIZE_T GetBufferSize() override { return data.size(); }
    std::vector<uint8_t> data;
};

// -------------------------------------------------------------------------------------------------------
// Reflection facade
// -------------------------------------------------------------------------------------------------------
const char* typeNameFor(const CBufferVariable& v)
{
    static thread_local char buf[32];
    const char* base = v.type == D3D_SVT_INT ? "int" : v.type == D3D_SVT_UINT ? "uint" : v.type == D3D_SVT_BOOL ? "bool" : "float";
    if (v.cls == D3D_SVC_SCALAR)
        snprintf(buf, sizeof(buf), "%s", base);
    else if (v.cls == D3D_SVC_VECTOR)
        snprintf(buf, sizeof(buf), "%s%u", base, v.columns);
    else
        snprintf(buf, sizeof(buf), "%s%ux%u", base, v.rows, v.columns);
    return buf;
}

struct TypeFacade final : ID3D11ShaderReflectionType
{
    D3D11_SHADER_TYPE_DESC desc{};
    std::string name;
    HRESULT GetDesc(D3D11_SHADER_TYPE_DESC* p) override
    {
        *p = desc;
        return S_OK;
    }
    ID3D11ShaderReflectionType* GetMemberTypeByIndex(UINT) override { return nullptr; }
    LPCSTR GetMemberTypeName(UINT) override { return nullptr; }
};

struct VariableFacade final : ID3D11ShaderReflectionVariable
{
    D3D11_SHADER_VARIABLE_DESC desc{};
    std::string name;
    TypeFacade type;
    HRESULT GetDesc(D3D11_SHADER_VARIABLE_DESC* p) override
    {
        *p = desc;
        return S_OK;
    }
    ID3D11ShaderReflectionType* GetType() override { return &type; }
};

struct CBufferFacade final : ID3D11ShaderReflectionConstantBuffer
{
    D3D11_SHADER_BUFFER_DESC desc{};
    std::string name;
    std::vector<VariableFacade> vars;
    HRESULT GetDesc(D3D11_SHADER_BUFFER_DESC* p) override
    {
        *p = desc;
        return S_OK;
    }
    ID3D11ShaderReflectionVariable* GetVariableByIndex(UINT i) override { return i < vars.size() ? &vars[i] : nullptr; }
    ID3D11ShaderReflectionVariable* GetVariableByName(LPCSTR n) override
    {
        for (VariableFacade& v : vars)
            if (v.name == n)
                return &v;
        return nullptr;
    }
};

class ReflectionImpl final : public ComObject<ID3D11ShaderReflection>
{
public:
    explicit ReflectionImpl(ShaderReflection r) : refl(std::move(r))
    {
        cbs.resize(refl.cbuffers.size());
        for (size_t i = 0; i < cbs.size(); ++i)
        {
            const CBuffer& src = refl.cbuffers[i];
            CBufferFacade& cb = cbs[i];
            cb.name = src.name;
            cb.desc.Name = cb.name.c_str();
            cb.desc.Type = D3D_CT_CBUFFER;
            cb.desc.Variables = UINT(src.variables.size());
            cb.desc.Size = src.sizeBytes;
            cb.vars.resize(src.variables.size());
            for (size_t j = 0; j < src.variables.size(); ++j)
            {
                const CBufferVariable& sv = src.variables[j];
                VariableFacade& v = cb.vars[j];
                v.name = sv.name;
                v.desc.Name = v.name.c_str();
                v.desc.StartOffset = sv.offset;
                v.desc.Size = sv.size;
                v.type.name = typeNameFor(sv);
                v.type.desc.Class = sv.cls;
                v.type.desc.Type = sv.type;
                v.type.desc.Rows = sv.rows;
                v.type.desc.Columns = sv.columns;
                v.type.desc.Elements = sv.elements;
                v.type.desc.Name = v.type.name.c_str();
            }
        }
        // D3D lists cbuffers among the bound resources too.
        for (const CBuffer& cb : refl.cbuffers)
        {
            D3D11_SHADER_INPUT_BIND_DESC d{};
            d.Name = cb.name.c_str();
            d.Type = D3D_SIT_CBUFFER;
            d.BindPoint = cb.d3dSlot;
            d.BindCount = 1;
            bound.push_back(d);
        }
        for (const ResourceBinding& rb : refl.resources)
        {
            D3D11_SHADER_INPUT_BIND_DESC d{};
            d.Name = rb.name.c_str();
            d.Type = rb.type;
            d.BindPoint = rb.d3dSlot;
            d.BindCount = rb.count;
            d.ReturnType = rb.returnType;
            d.Dimension = rb.dimension;
            d.NumSamples = rb.numSamples;
            bound.push_back(d);
        }
    }
    HRESULT GetDesc(D3D11_SHADER_DESC* p) override
    {
        std::memset(p, 0, sizeof(*p));
        p->Version = 0x50;
        p->Creator = "MetalD3D11";
        p->ConstantBuffers = UINT(cbs.size());
        p->BoundResources = UINT(bound.size());
        p->InputParameters = UINT(refl.inputs.size());
        p->OutputParameters = refl.colorOutputs;
        return S_OK;
    }
    ID3D11ShaderReflectionConstantBuffer* GetConstantBufferByIndex(UINT i) override
    {
        return i < cbs.size() ? &cbs[i] : nullptr;
    }
    ID3D11ShaderReflectionConstantBuffer* GetConstantBufferByName(LPCSTR n) override
    {
        for (CBufferFacade& c : cbs)
            if (c.name == n)
                return &c;
        return nullptr;
    }
    HRESULT GetResourceBindingDesc(UINT i, D3D11_SHADER_INPUT_BIND_DESC* p) override
    {
        if (i >= bound.size())
            return E_INVALIDARG;
        *p = bound[i];
        return S_OK;
    }
    HRESULT GetInputParameterDesc(UINT i, D3D11_SIGNATURE_PARAMETER_DESC* p) override
    {
        if (i >= refl.inputs.size())
            return E_INVALIDARG;
        const VertexInput& in = refl.inputs[i];
        std::memset(p, 0, sizeof(*p));
        p->SemanticName = in.semantic.c_str();
        p->SemanticIndex = in.semanticIndex;
        p->Register = in.attribute;
        p->ComponentType = in.componentType;
        p->Mask = UINT8((1u << in.components) - 1);
        p->ReadWriteMask = p->Mask;
        return S_OK;
    }
    HRESULT GetOutputParameterDesc(UINT, D3D11_SIGNATURE_PARAMETER_DESC*) override { return E_NOTIMPL; }
    HRESULT GetThreadGroupSize(UINT* x, UINT* y, UINT* z) override
    {
        if (x) *x = refl.threadgroupSize[0];
        if (y) *y = refl.threadgroupSize[1];
        if (z) *z = refl.threadgroupSize[2];
        return S_OK;
    }

private:
    ShaderReflection refl;
    std::vector<CBufferFacade> cbs;
    std::vector<D3D11_SHADER_INPUT_BIND_DESC> bound;
};
} // namespace
} // namespace mtl_d3d11

using namespace mtl_d3d11;

HRESULT D3DCompile(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName, const D3D10_SHADER_MACRO* pDefines,
                   ID3D10Include* pInclude, LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags1, UINT Flags2,
                   ID3D10Blob** ppCode, ID3D10Blob** ppErrorMsgs)
{
    if (ppCode)
        *ppCode = nullptr;
    if (ppErrorMsgs)
        *ppErrorMsgs = nullptr;
    if (!hook().fn)
        return E_NOTIMPL;
    CompileRequest req = {pSrcData, SrcDataSize, pSourceName, pDefines, pInclude, pEntrypoint, pTarget, Flags1, Flags2};
    std::vector<uint8_t> container;
    std::string errors;
    HRESULT hr = hook().fn(req, &container, &errors, hook().user);
    if (!errors.empty() && ppErrorMsgs)
        *ppErrorMsgs = new BlobImpl(std::vector<uint8_t>(errors.begin(), errors.end() + 0));
    if (FAILED(hr))
        return hr;
    if (ppCode)
        *ppCode = new BlobImpl(std::move(container));
    return S_OK;
}

HRESULT D3DReflect(LPCVOID pSrcData, SIZE_T SrcDataSize, MtlREFIID pInterface, void** ppReflector)
{
    if (!ppReflector)
        return E_POINTER;
    *ppReflector = nullptr;
    if (pInterface != mtl_uuidof<ID3D11ShaderReflection>())
        return E_NOINTERFACE;
    ShaderReflection r;
    if (!ParseReflection(pSrcData, SrcDataSize, &r))
        return E_FAIL;
    *ppReflector = static_cast<ID3D11ShaderReflection*>(new ReflectionImpl(std::move(r)));
    return S_OK;
}

HRESULT D3DGetInputSignatureBlob(LPCVOID pSrcData, SIZE_T SrcDataSize, ID3D10Blob** ppSignatureBlob)
{
    if (!ppSignatureBlob)
        return E_POINTER;
    *ppSignatureBlob = nullptr;
    ShaderBlob b;
    if (!Deserialize(pSrcData, SrcDataSize, &b))
        return E_FAIL;
    b.payload.clear();
    *ppSignatureBlob = new BlobImpl(Serialize(b));
    return S_OK;
}

HRESULT D3DDisassemble(LPCVOID pSrcData, SIZE_T SrcDataSize, UINT, LPCSTR, ID3D10Blob** ppDisassembly)
{
    if (!ppDisassembly)
        return E_POINTER;
    ShaderBlob b;
    if (!Deserialize(pSrcData, SrcDataSize, &b))
        return E_FAIL;
    std::vector<uint8_t> text;
    if (b.kind == PayloadKind::MslSource)
        text = b.payload;
    else
    {
        static const char note[] = "// precompiled metallib payload\n";
        text.assign(note, note + sizeof(note) - 1);
    }
    *ppDisassembly = new BlobImpl(std::move(text));
    return S_OK;
}
