// HLSL -> SPIR-V with vkd3d-shader. vkd3d's HLSL compiler is an FXC work-alike: it inlines, unrolls
// and folds constants before backend checks, which is why dynamic texture offsets and similar
// FXC-isms in the shipped shaders compile.
//
// Two steps, because vkd3d's direct HLSL -> SPIR-V path drops every name ("cb0_0", "t0", "V")
// while the DXBC it can also produce keeps them:
//   1. HLSL --> DXBC (TPF, SM5). Besides the code, the container has RDEF (names, registers, cbuffer
//      layouts, variable types) and ISGN/OSGN, i.e. what ID3D11ShaderReflection returned for the
//      FXC build; those are parsed into Reflection.
//   2. DXBC --> SPIR-V with explicit descriptor bindings derived from the registers (Internal.h:
//      binding = class base + register).
// This costs half as much as compiling the HLSL twice (once for names, once for SPIR-V), and the
// shaders pass in exactly the same numbers either way. vkd3d logs "Invalid swizzle ... for scalar
// value" for SampleCmp(...).x on both routes; the generated code is correct (the scalar is
// broadcast), so log lines never decide success.
#include "Internal.h"

extern "C" {
#include <vkd3d_shader.h>
}

#include <csetjmp>
#include <csignal>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

// libvkd3d-common's logging hook (private, but the library is linked statically).
extern "C" void vkd3d_dbg_set_log_callback(void (*callback)(const char* format, va_list args));

namespace shaderxlat
{
const char* frontend_name() { return "vkd3d-shader"; }
} // namespace shaderxlat

namespace shaderxlat::detail
{
namespace
{
struct IncludeCtx
{
    IncludeProvider* provider;
};

// vkd3d prints "err"/"fixme" diagnostics to stderr by default. Route them into the per-call log
// instead; some are harmless (e.g. "Invalid swizzle ... for scalar value" for SampleCmp(...).x,
// which still yields correct code), so they never decide success on their own.
thread_local std::string* g_logSink = nullptr;

void logCallback(const char* format, va_list args)
{
    if (!g_logSink)
        return;
    char buf[1024];
    vsnprintf(buf, sizeof buf, format, args);
    g_logSink->append(buf);
}

struct LogCapture
{
    explicit LogCapture(std::string& sink)
    {
        static std::once_flag once;
        std::call_once(once, [] { vkd3d_dbg_set_log_callback(logCallback); });
        g_logSink = &sink;
    }
    ~LogCapture() { g_logSink = nullptr; }
};

int openInclude(const char* filename, bool, const char*, void* context, vkd3d_shader_code* out)
{
    auto* ctx = static_cast<IncludeCtx*>(context);
    std::string resolved, contents;
    if (!ctx->provider || !ctx->provider->open(filename, "", resolved, contents))
        return VKD3D_ERROR;
    char* mem = (char*)std::malloc(contents.size() + 1);
    std::memcpy(mem, contents.data(), contents.size());
    mem[contents.size()] = 0;
    out->code = mem;
    out->size = contents.size();
    return VKD3D_OK;
}

void closeInclude(const vkd3d_shader_code* code, void*) { std::free(const_cast<void*>(code->code)); }

void takeMessages(char* messages, std::string& log)
{
    if (!messages)
        return;
    log += messages;
    vkd3d_shader_free_messages(messages);
}


// ---- crash guard ---------------------------------------------------------------------------------
//
// vkd3d-shader's HLSL front end can dereference NULL while recovering from some errors (observed:
// `add_implicit_conversion` after an undeclared identifier inside an expression, e.g. a missing
// quality #define in an addon's shader). A bad shader must not take the game down, so the three
// vkd3d entry points run under a SIGSEGV/SIGBUS guard that longjmps out. The abandoned vkd3d
// allocations leak (a few KB), the library keeps no global state that could be left inconsistent,
// and faults outside a guarded call are forwarded to whatever handler was installed before ours.
thread_local sigjmp_buf* g_jump = nullptr;
struct sigaction g_prevSegv, g_prevBus;
std::once_flag g_handlersOnce;

void faultHandler(int sig, siginfo_t* info, void* ctx)
{
    if (g_jump)
        siglongjmp(*g_jump, 1);
    const struct sigaction& prev = sig == SIGBUS ? g_prevBus : g_prevSegv;
    if ((prev.sa_flags & SA_SIGINFO) && prev.sa_sigaction)
        prev.sa_sigaction(sig, info, ctx);
    else if (!(prev.sa_flags & SA_SIGINFO) && prev.sa_handler && prev.sa_handler != SIG_IGN && prev.sa_handler != SIG_DFL)
        prev.sa_handler(sig);
    else
    {
        signal(sig, SIG_DFL); // returning re-executes the faulting instruction with the default action
    }
}

// Each thread needs its own alternate stack so a runaway recursion can also be caught.
//
//  * A thread that already has one (a crash reporter installed it) keeps it.
//  * Under a sanitizer the runtime gives every thread it starts an alternate stack and, at thread exit, unmaps whatever is installed
//    at that moment: a block of this guard's would be unmapped by someone else, so the guard installs nothing there.
//  * Otherwise the guard installs a stack of its own and removes and frees it with the thread. Darwin refuses to disable a stack
//    whose size is left at zero and keeps it installed, so the disable carries the real size; if it still fails the block is
//    leaked, because freeing an installed alternate stack is worse than losing 64 KB.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define XLAT_SANITIZER_OWNS_ALT_STACK 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define XLAT_SANITIZER_OWNS_ALT_STACK 1
#endif
#endif

struct AltStack
{
    static constexpr size_t kSize = 64 * 1024;
    void* mem = nullptr;
    AltStack()
    {
#ifdef XLAT_SANITIZER_OWNS_ALT_STACK
        return;
#else
        stack_t cur = {};
        if (sigaltstack(nullptr, &cur) == 0 && !(cur.ss_flags & SS_DISABLE))
            return;
        stack_t ss = {};
        ss.ss_size = kSize;
        ss.ss_sp = mem = std::malloc(ss.ss_size);
        if (ss.ss_sp && sigaltstack(&ss, nullptr) != 0)
        {
            std::free(mem);
            mem = nullptr;
        }
#endif
    }
    ~AltStack()
    {
        if (!mem)
            return;
        stack_t cur = {};
        if (sigaltstack(nullptr, &cur) == 0 && cur.ss_sp == mem && !(cur.ss_flags & SS_DISABLE))
        {
            stack_t ss = {};
            ss.ss_flags = SS_DISABLE;
            ss.ss_size = kSize;
            if (sigaltstack(&ss, nullptr) != 0)
                return; // still installed
        }
        std::free(mem);
    }
};

void armGuard()
{
    std::call_once(g_handlersOnce, [] {
        struct sigaction sa = {};
        sa.sa_sigaction = faultHandler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &g_prevSegv);
        sigaction(SIGBUS, &sa, &g_prevBus);
    });
    thread_local AltStack alt;
    (void)alt;
}

// Plain-C frames only between sigsetjmp and the fault: nothing here owns C++ resources.
int guardedCompile(const vkd3d_shader_compile_info* info, vkd3d_shader_code* out, char** messages, bool* crashed)
{
    armGuard();
    sigjmp_buf jb;
    *messages = nullptr;
    if (sigsetjmp(jb, 1) == 0)
    {
        g_jump = &jb;
        const int rc = vkd3d_shader_compile(info, out, messages);
        g_jump = nullptr;
        return rc;
    }
    g_jump = nullptr;
    *crashed = true;
    return VKD3D_ERROR;
}

int guardedScan(const vkd3d_shader_compile_info* info, char** messages, bool* crashed)
{
    armGuard();
    sigjmp_buf jb;
    *messages = nullptr;
    if (sigsetjmp(jb, 1) == 0)
    {
        g_jump = &jb;
        const int rc = vkd3d_shader_scan(info, messages);
        g_jump = nullptr;
        return rc;
    }
    g_jump = nullptr;
    *crashed = true;
    return VKD3D_ERROR;
}

ScalarType componentType(vkd3d_shader_component_type t)
{
    switch (t)
    {
    case VKD3D_SHADER_COMPONENT_UINT: return ScalarType::UInt;
    case VKD3D_SHADER_COMPONENT_INT: return ScalarType::Int;
    case VKD3D_SHADER_COMPONENT_BOOL: return ScalarType::Bool;
    case VKD3D_SHADER_COMPONENT_DOUBLE: return ScalarType::Double;
    default: return ScalarType::Float;
    }
}

uint32_t popcount4(uint32_t m) { return (m & 1) + ((m >> 1) & 1) + ((m >> 2) & 1) + ((m >> 3) & 1); }

void convertSignature(const vkd3d_shader_signature& sig, std::vector<StageVariable>& out)
{
    for (unsigned i = 0; i < sig.element_count; ++i)
    {
        const vkd3d_shader_signature_element& e = sig.elements[i];
        StageVariable v;
        v.semantic = e.semantic_name ? e.semantic_name : "";
        for (char& c : v.semantic)
            c = (char)std::toupper((unsigned char)c);
        v.semantic_index = e.semantic_index;
        v.reg = e.register_index;
        v.mask = e.mask;
        v.type = componentType(e.component_type);
        v.components = popcount4(e.mask);
        v.system_value = e.sysval_semantic != VKD3D_SHADER_SV_NONE && e.sysval_semantic != VKD3D_SHADER_SV_TARGET;
        out.push_back(std::move(v));
    }
}
} // namespace

bool frontendCompile(const FrontendInput& in, FrontendOutput& out)
{
    std::string diagnostics;
    LogCapture capture(diagnostics);
    struct Flush
    {
        std::string& d;
        std::string& log;
        ~Flush()
        {
            if (!d.empty())
                log += "\n[vkd3d] " + d;
        }
    } flush{diagnostics, out.log};

    std::vector<vkd3d_shader_macro> macros;
    for (const Macro& m : *in.macros)
        macros.push_back({m.name.c_str(), m.value.c_str()});
    IncludeCtx ctx{in.includes};

    vkd3d_shader_compile_option options[] = {
        {VKD3D_SHADER_COMPILE_OPTION_API_VERSION, VKD3D_SHADER_API_VERSION_1_16},
        {VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ORDER, in.options->row_major_matrices
                                                            ? VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ROW_MAJOR
                                                            : VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_COLUMN_MAJOR},
    };

    // ---- step 1: HLSL -> DXBC
    vkd3d_shader_preprocess_info pre = {};
    pre.type = VKD3D_SHADER_STRUCTURE_TYPE_PREPROCESS_INFO;
    pre.macros = macros.data();
    pre.macro_count = (unsigned)macros.size();
    pre.pfn_open_include = openInclude;
    pre.pfn_close_include = closeInclude;
    pre.include_context = &ctx;

    vkd3d_shader_hlsl_source_info hlsl = {};
    hlsl.type = VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO;
    hlsl.next = &pre;
    hlsl.entry_point = in.options->entry.c_str();
    hlsl.profile = in.profile.c_str();

    vkd3d_shader_compile_info info = {};
    info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    info.next = &hlsl;
    info.source.code = in.source->data();
    info.source.size = in.source->size();
    info.source_type = VKD3D_SHADER_SOURCE_HLSL;
    info.target_type = VKD3D_SHADER_TARGET_DXBC_TPF;
    info.options = options;
    info.option_count = sizeof(options) / sizeof(options[0]);
    info.log_level = VKD3D_SHADER_LOG_ERROR;
    info.source_name = in.name->c_str();

    vkd3d_shader_code dxbc = {};
    char* messages = nullptr;
    bool crashed = false;
    int rc = guardedCompile(&info, &dxbc, &messages, &crashed);
    takeMessages(messages, out.log);
    if (crashed)
        out.log += "\nvkd3d-shader crashed while compiling this shader (contained by the crash guard)";
    std::string rewritten; // keeps the retry's source alive
    if (rc != VKD3D_OK && !crashed && out.log.find("E5013") != std::string::npos)
    {
        // "Input semantic ... used multiple times with incompatible types": FXC allows it. Preprocess, alias, compile again.
        vkd3d_shader_code pre_out = {};
        char* pre_messages = nullptr;
        if (vkd3d_shader_preprocess(&info, &pre_out, &pre_messages) == VKD3D_OK)
        {
            rewritten.assign(static_cast<const char*>(pre_out.code), pre_out.size);
            if (aliasDuplicateInputSemantics(rewritten, in.options->entry))
            {
                vkd3d_shader_hlsl_source_info plain = {};
                plain.type = VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO;
                plain.entry_point = in.options->entry.c_str();
                plain.profile = in.profile.c_str();
                vkd3d_shader_compile_info again = info;
                again.next = &plain;
                again.source.code = rewritten.data();
                again.source.size = rewritten.size();
                vkd3d_shader_free_shader_code(&dxbc);
                dxbc = {};
                char* messages2 = nullptr;
                const int rc2 = guardedCompile(&again, &dxbc, &messages2, &crashed);
                std::string second;
                takeMessages(messages2, second);
                if (rc2 == VKD3D_OK)
                {
                    rc = VKD3D_OK;
                    out.aliasedSemantics = true;
                    out.log.clear();
                }
                else
                    out.log += "\n[after aliasing the duplicate input semantics] " + second;
            }
        }
        vkd3d_shader_free_shader_code(&pre_out);
        if (pre_messages)
            vkd3d_shader_free_messages(pre_messages);
    }
    if (rc != VKD3D_OK)
        return false;

    struct DxbcGuard
    {
        vkd3d_shader_code& c;
        ~DxbcGuard() { vkd3d_shader_free_shader_code(&c); }
    } dxbcGuard{dxbc};

    // ---- reflection from the DXBC
    vkd3d_shader_dxbc_desc desc = {};
    messages = nullptr;
    rc = vkd3d_shader_parse_dxbc(&dxbc, 0, &desc, &messages);
    takeMessages(messages, out.log);
    if (rc != VKD3D_OK)
        return false;
    bool haveRdef = false;
    std::string err;
    for (size_t i = 0; i < desc.section_count; ++i)
        if (desc.sections[i].tag == 0x46454452 /* 'RDEF' */)
        {
            haveRdef = parseRdef((const uint8_t*)desc.sections[i].data.code, desc.sections[i].data.size, out.d3d, err);
            break;
        }
    vkd3d_shader_free_dxbc(&desc);
    if (!haveRdef)
    {
        out.log += "no usable RDEF chunk: " + err;
        return false;
    }

    vkd3d_shader_scan_signature_info sigInfo = {};
    sigInfo.type = VKD3D_SHADER_STRUCTURE_TYPE_SCAN_SIGNATURE_INFO;
    vkd3d_shader_compile_info scan = {};
    scan.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    scan.next = &sigInfo;
    scan.source = dxbc;
    scan.source_type = VKD3D_SHADER_SOURCE_DXBC_TPF;
    scan.target_type = VKD3D_SHADER_TARGET_SPIRV_BINARY;
    scan.log_level = VKD3D_SHADER_LOG_ERROR;
    rc = guardedScan(&scan, &messages, &crashed);
    takeMessages(messages, out.log);
    if (rc != VKD3D_OK)
        return false;
    convertSignature(sigInfo.input, out.d3d.inputs);
    convertSignature(sigInfo.output, out.d3d.outputs);
    vkd3d_shader_free_scan_signature_info(&sigInfo);

    // ---- step 2: DXBC -> SPIR-V with bindings derived from the registers
    std::vector<vkd3d_shader_resource_binding> bindings;
    auto bind = [&](vkd3d_shader_descriptor_type type, uint32_t slot, uint32_t base, unsigned flags) {
        vkd3d_shader_resource_binding b = {};
        b.type = type;
        b.register_space = 0;
        b.register_index = slot;
        b.shader_visibility = VKD3D_SHADER_VISIBILITY_ALL;
        b.flags = flags;
        b.binding.set = 0;
        b.binding.binding = base + slot;
        b.binding.count = 1;
        bindings.push_back(b);
    };
    const unsigned bufOrImage = VKD3D_SHADER_BINDING_FLAG_BUFFER | VKD3D_SHADER_BINDING_FLAG_IMAGE;
    for (const ConstantBuffer& cb : out.d3d.constant_buffers)
        bind(VKD3D_SHADER_DESCRIPTOR_TYPE_CBV, cb.slot, kBindingCbv, VKD3D_SHADER_BINDING_FLAG_BUFFER);
    for (const TextureBinding& t : out.d3d.textures)
        bind(t.is_uav ? VKD3D_SHADER_DESCRIPTOR_TYPE_UAV : VKD3D_SHADER_DESCRIPTOR_TYPE_SRV, t.slot,
             t.is_uav ? kBindingUav : kBindingSrv, bufOrImage);
    for (const StorageBuffer& s : out.d3d.storage_buffers)
        bind(s.is_uav ? VKD3D_SHADER_DESCRIPTOR_TYPE_UAV : VKD3D_SHADER_DESCRIPTOR_TYPE_SRV, s.slot,
             s.is_uav ? kBindingUav : kBindingSrv, bufOrImage);
    for (const SamplerBinding& s : out.d3d.samplers)
        bind(VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER, s.slot, kBindingSampler, 0);

    vkd3d_shader_spirv_target_info spirv = {};
    spirv.type = VKD3D_SHADER_STRUCTURE_TYPE_SPIRV_TARGET_INFO;
    spirv.entry_point = "main";
    spirv.environment = VKD3D_SHADER_SPIRV_ENVIRONMENT_VULKAN_1_1;

    vkd3d_shader_interface_info iface = {};
    iface.type = VKD3D_SHADER_STRUCTURE_TYPE_INTERFACE_INFO;
    iface.next = &spirv;
    iface.bindings = bindings.data();
    iface.binding_count = (unsigned)bindings.size();

    vkd3d_shader_compile_info toSpirv = {};
    toSpirv.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    toSpirv.next = &iface;
    toSpirv.source = dxbc;
    toSpirv.source_type = VKD3D_SHADER_SOURCE_DXBC_TPF;
    toSpirv.target_type = VKD3D_SHADER_TARGET_SPIRV_BINARY;
    toSpirv.options = options;
    toSpirv.option_count = 1; // API version only; matrix order is already baked into the DXBC
    toSpirv.log_level = VKD3D_SHADER_LOG_ERROR;
    toSpirv.source_name = in.name->c_str();

    vkd3d_shader_code code = {};
    rc = guardedCompile(&toSpirv, &code, &messages, &crashed);
    takeMessages(messages, out.log);
    if (crashed)
        out.log += "\nvkd3d-shader crashed while generating SPIR-V (contained by the crash guard)";
    if (rc != VKD3D_OK)
        return false;
    out.spirv.resize(code.size / 4);
    std::memcpy(out.spirv.data(), code.code, code.size);
    vkd3d_shader_free_shader_code(&code);
    return true;
}
} // namespace shaderxlat::detail
