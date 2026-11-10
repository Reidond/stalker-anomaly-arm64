#include "stdafx.h"

#include <array>
#ifdef XR_ARCH_ARM64
#include <arm_acle.h>
#endif
#ifdef XR_ARCH_WASM32
#include <string.h>
#endif

namespace
{
    constexpr u32 reflect(u32 ref, int ch) noexcept
    {
        u32 value = 0;
        for (int i = 1; i < (ch + 1); i++)
        {
            if (ref & 1)
                value |= 1 << (ch - i);
            ref >>= 1;
        }
        return value;
    }

    constexpr u32 ulPolynomial = 0x04c11db7;

    constexpr std::array<u32, 256> generate_crc32_lookup_table() noexcept
    {
        std::array<u32, 256> table{};

        for (int i = 0; i <= 0xFF; i++)
        {
            table[i] = reflect(i, 8) << 24;
            for (int j = 0; j < 8; j++)
                table[i] = (table[i] << 1) ^ ((table[i] & (1u << 31)) ? ulPolynomial : 0);
            table[i] = reflect(table[i], 32);
        }

        return table;
    }

    static constexpr auto crc32_table = generate_crc32_lookup_table();

#ifdef XR_ARCH_WASM32
    // WebAssembly has no CRC32C instruction. Software CRC32C (Castagnoli, reflected polynomial 0x82F63B78), slicing by 8, with
    // the same contract as _mm_crc32_* on x64: crc in, crc out, no final inversion. crc32() below must keep choosing this
    // variant (CPU::ID reports SSE4.2 on the web build) or its results would be the plain IEEE CRC32 and differ from every
    // other platform's.
    constexpr std::array<std::array<u32, 256>, 8> generate_crc32c_tables() noexcept
    {
        std::array<std::array<u32, 256>, 8> t{};
        for (u32 i = 0; i < 256; i++)
        {
            u32 c = i;
            for (int j = 0; j < 8; j++)
                c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : (c >> 1);
            t[0][i] = c;
        }
        for (int k = 1; k < 8; k++)
            for (u32 i = 0; i < 256; i++)
                t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 0xFF];
        return t;
    }

    static constexpr auto crc32c_tables = generate_crc32c_tables();

    u32 crc32_sse42(const void* P, u32 len, u32 starting_crc = ~0u) noexcept
    {
        const u8* buffer = static_cast<const u8*>(P);
        u32 crc = starting_crc;

        while (len >= 8)
        {
            u32 lo, hi;
            memcpy(&lo, buffer, 4);     // wasm is little endian
            memcpy(&hi, buffer + 4, 4);
            lo ^= crc;
            crc = crc32c_tables[7][lo & 0xFF] ^ crc32c_tables[6][(lo >> 8) & 0xFF] ^ crc32c_tables[5][(lo >> 16) & 0xFF] ^
                crc32c_tables[4][lo >> 24] ^ crc32c_tables[3][hi & 0xFF] ^ crc32c_tables[2][(hi >> 8) & 0xFF] ^
                crc32c_tables[1][(hi >> 16) & 0xFF] ^ crc32c_tables[0][hi >> 24];
            buffer += 8;
            len -= 8;
        }
        while (len--)
            crc = (crc >> 8) ^ crc32c_tables[0][(crc ^ *buffer++) & 0xFF];
        return crc;
    }
#elif defined(XR_ARCH_ARM64)
    __attribute__((target("crc")))
    u32 crc32_sse42(const void* P, u32 len, u32 starting_crc = ~0u) noexcept
    {
        // same CRC32C (Castagnoli) update, without the final inversion, as _mm_crc32_* on x64
        const u8* buffer = static_cast<const u8*>(P);
        u32 crc = starting_crc;

        while (len >= 8)
        {
            u64 v;
            memcpy(&v, buffer, 8);
            crc = __crc32cd(crc, v);
            buffer += 8;
            len -= 8;
        }
        if (len >= 4)
        {
            u32 v;
            memcpy(&v, buffer, 4);
            crc = __crc32cw(crc, v);
            buffer += 4;
            len -= 4;
        }
        if (len >= 2)
        {
            u16 v;
            memcpy(&v, buffer, 2);
            crc = __crc32ch(crc, v);
            buffer += 2;
            len -= 2;
        }
        if (len)
            crc = __crc32cb(crc, *buffer);
        return crc;
    }
#else
    u32 crc32_sse42(const void* P, u32 len, u32 starting_crc = ~0u) noexcept
    {
        const u8* buffer = static_cast<const u8*>(P);
        u32 crc = starting_crc;

        while (len >= 8)
        {
            crc = (u32)_mm_crc32_u64(crc, *reinterpret_cast<const u64*>(buffer));
            buffer += 8;
            len -= 8;
        }

        if (len >= 4)
        {
            crc = _mm_crc32_u32(crc, *reinterpret_cast<const u32*>(buffer));
            buffer += 4;
            len -= 4;
        }

        if (len >= 2)
        {
            crc = _mm_crc32_u16(crc, *reinterpret_cast<const u16*>(buffer));
            buffer += 2;
            len -= 2;
        }

        if (len)
        {
            crc = _mm_crc32_u8(crc, *buffer);
        }

        return crc;
    }
#endif
}

// arm64 always has the CRC32 extension (ARMv8.1+), and CPU::ID reports SSE4.2 for it.
u32 crc32(const void* P, u32 len)
{
    if (CPU::ID.feature & _CPU_FEATURE_SSE4_2)
    {
        return crc32_sse42(P, len);
    }

    u32 ulCRC = 0xffffffff;
    u8* buffer = (u8*)P;

    while (len--)
    {
        ulCRC = (ulCRC >> 8) ^ crc32_table[(ulCRC & 0xFF) ^ *buffer++];
    }

    return ulCRC ^ 0xffffffff;
}

u32 crc32(const void* P, u32 len, u32 starting_crc)
{
    if (CPU::ID.feature & _CPU_FEATURE_SSE4_2)
    {
        return crc32_sse42(P, len, ~starting_crc);
    }

    u32 ulCRC = 0xffffffff ^ starting_crc;
    u8* buffer = (u8*)P;

    while (len--)
    {
        ulCRC = (ulCRC >> 8) ^ crc32_table[(ulCRC & 0xFF) ^ *buffer++];
    }

    return ulCRC ^ 0xffffffff;
}

u32 path_crc32(const char* path, u32 len)
{
    u32 ulCRC = 0xffffffff;
    u8* buffer = (u8*)path;

    while (len--) {
        const u8 c = *buffer;
        if (c != '/' && c != '\\') {
            ulCRC = (ulCRC >> 8) ^ crc32_table[(ulCRC & 0xFF) ^ *buffer];
        }
        ++buffer;
    }

    return ~ulCRC;
}
