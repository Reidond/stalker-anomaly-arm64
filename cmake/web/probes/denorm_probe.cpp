// Denormal behaviour probe: what the engine's per-thread setup (flush to zero) changes, and what wasm has instead (nothing).
// usage: denorm_probe [f]   (f = set flush-to-zero first, native arm64 only)
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <cfloat>
#if defined(__aarch64__)
static void set_ftz() { uint64_t v; asm volatile("mrs %0, fpcr" : "=r"(v)); v |= 1ull << 24; asm volatile("msr fpcr, %0" : : "r"(v)); }
#else
static void set_ftz() {}
#endif
static uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
volatile float vf[8];
int main(int argc, char** argv)
{
    const bool ftz = argc > 1 && argv[1][0] == 'f';
    if (ftz) set_ftz();
    printf("mode: %s\n", ftz ? "flush-to-zero on (the engine's native threads)" : "denormals kept (wasm)");
    vf[0] = FLT_MIN; vf[1] = 0.5f; vf[2] = 1.0000001f; vf[3] = 1.0f; vf[4] = 3e-39f; vf[5] = 4e-39f; vf[6] = 1e38f;
    float a = vf[0] * vf[1];                 // result below FLT_MIN
    float b = (vf[2] - vf[3]) * 1e-30f;      // 1.19e-37, normal
    float c = vf[4] + vf[5];                 // denormal + denormal
    float d = vf[4] * vf[6];                 // denormal operand scaled back into the normal range: 3e-39 * 1e38
    float e = vf[4] - vf[5];                 // exact denormal difference
    printf("FLT_MIN*0.5        = %.9g  bits %08x\n", a, bits(a));
    printf("(1+eps-1)*1e-30    = %.9g  bits %08x\n", b, bits(b));
    printf("3e-39 + 4e-39      = %.9g  bits %08x\n", c, bits(c));
    printf("3e-39 * 1e38       = %.9g  bits %08x\n", d, bits(d));
    printf("3e-39 - 4e-39      = %.9g  bits %08x\n", e, bits(e));
    // A decaying filter: y *= 0.99 for 12000 steps (1.0 -> 0.99^12000 = 1e-52, far below the denormal range).
    // With IEEE denormals y stops at the smallest denormal (1.4e-45 * 0.99 rounds back to 1.4e-45) and never reaches 0;
    // flushed, it reaches 0 at the first result below FLT_MIN.
    float y = 1.0f; long first_denormal = -1, first_zero = -1;
    const int reps = 2000;
    auto t0 = std::chrono::steady_clock::now();
    for (int rep = 0; rep < reps; ++rep)
    {
        y = 1.0f; first_denormal = first_zero = -1;
        for (long i = 0; i < 12000; ++i)
        {
            vf[7] = y; y = vf[7] * 0.99f;
            if (first_denormal < 0 && y != 0.0f && y < FLT_MIN) first_denormal = i;
            if (first_zero < 0 && y == 0.0f) first_zero = i;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    printf("y*=0.99, 12000 steps: first denormal at step %ld, first exact zero at step %ld, final y = %.9g (bits %08x)\n",
           first_denormal, first_zero, y, bits(y));
    printf("%d repeats: %.1f ms\n", reps, std::chrono::duration<double, std::milli>(t1 - t0).count());
    return 0;
}
