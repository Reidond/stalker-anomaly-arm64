// LZ-Huffman decoder (Haruhiko Okumura's LZHUF variant) used for compressed chunks in .db archives
// and .ogf/.xr files. Mirrors Decode() in src/xrCore/LzHuf.cpp but is self-contained and
// bounds-checked instead of using globals.
#include "xrdb.h"

#include <cstring>

namespace xrdb
{
namespace
{
constexpr int N = 4096;
constexpr int F = 60;
constexpr int THRESHOLD = 2;
constexpr int N_CHAR = 256 - THRESHOLD + F;
constexpr int T = N_CHAR * 2 - 1;
constexpr int R = T - 1;
constexpr unsigned MAX_FREQ = 0x4000;

const uint8_t p_len[64] = {
    0x03, 0x04, 0x04, 0x04, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x06, 0x06, 0x06, 0x06,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08};
const uint8_t p_code[64] = {
    0x00, 0x20, 0x30, 0x40, 0x50, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80, 0x88, 0x90, 0x94, 0x98, 0x9C,
    0xA0, 0xA4, 0xA8, 0xAC, 0xB0, 0xB4, 0xB8, 0xBC, 0xC0, 0xC2, 0xC4, 0xC6, 0xC8, 0xCA, 0xCC, 0xCE,
    0xD0, 0xD2, 0xD4, 0xD6, 0xD8, 0xDA, 0xDC, 0xDE, 0xE0, 0xE2, 0xE4, 0xE6, 0xE8, 0xEA, 0xEC, 0xEE,
    0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF};

struct Decoder
{
    const uint8_t* in;
    const uint8_t* in_end;
    uint8_t d_code[256];
    uint8_t d_len[256];
    unsigned getbuf = 0, getlen = 0;
    unsigned freq[T + 1];
    int prnt[T + N_CHAR + 1];
    int son[T];
    uint8_t text_buf[N + F];

    Decoder(const uint8_t* src, size_t n) : in(src), in_end(src + n)
    {
        // Decoding tables are the inverse of p_code/p_len: every byte that starts with the code
        // of position bucket i maps back to i.
        for (int i = 0; i < 64; ++i)
        {
            const int span = 1 << (8 - p_len[i]);
            for (int k = 0; k < span; ++k)
            {
                d_code[p_code[i] + k] = (uint8_t)i;
                d_len[p_code[i] + k] = p_len[i];
            }
        }
    }

    int getb() { return in == in_end ? 0 : *in++; }

    int get_bit()
    {
        while (getlen <= 8)
        {
            getbuf |= (unsigned)getb() << (8 - getlen);
            getlen += 8;
        }
        unsigned i = getbuf;
        getbuf <<= 1;
        --getlen;
        return (int)((i & 0x8000) >> 15);
    }

    int get_byte()
    {
        while (getlen <= 8)
        {
            getbuf |= (unsigned)getb() << (8 - getlen);
            getlen += 8;
        }
        unsigned i = getbuf;
        getbuf <<= 8;
        getlen -= 8;
        return (int)((i & 0xff00) >> 8);
    }

    void start_huff()
    {
        int i, j;
        for (i = 0; i < N_CHAR; ++i)
        {
            freq[i] = 1;
            son[i] = i + T;
            prnt[i + T] = i;
        }
        i = 0;
        j = N_CHAR;
        while (j <= R)
        {
            freq[j] = freq[i] + freq[i + 1];
            son[j] = i;
            prnt[i] = prnt[i + 1] = j;
            i += 2;
            ++j;
        }
        freq[T] = 0xffff;
        prnt[R] = 0;
    }

    void reconst()
    {
        int i, j, k;
        unsigned f, l;
        j = 0;
        for (i = 0; i < T; ++i)
        {
            if (son[i] >= T)
            {
                freq[j] = (freq[i] + 1) / 2;
                son[j] = son[i];
                ++j;
            }
        }
        for (i = 0, j = N_CHAR; j < T; i += 2, ++j)
        {
            k = i + 1;
            f = freq[j] = freq[i] + freq[k];
            for (k = j - 1; f < freq[k]; --k)
                ;
            ++k;
            l = (j - k) * sizeof(unsigned);
            memmove(&freq[k + 1], &freq[k], l);
            freq[k] = f;
            memmove(&son[k + 1], &son[k], l);
            son[k] = i;
        }
        for (i = 0; i < T; ++i)
        {
            if ((k = son[i]) >= T)
                prnt[k] = i;
            else
                prnt[k] = prnt[k + 1] = i;
        }
    }

    void update(int c)
    {
        int i, j, k, l;
        if (freq[R] == MAX_FREQ)
            reconst();
        c = prnt[c + T];
        do
        {
            k = ++freq[c];
            if ((unsigned)k > freq[l = c + 1])
            {
                while ((unsigned)k > freq[++l])
                    ;
                --l;
                freq[c] = freq[l];
                freq[l] = k;

                i = son[c];
                prnt[i] = l;
                if (i < T)
                    prnt[i + 1] = l;

                j = son[l];
                son[l] = i;

                prnt[j] = c;
                if (j < T)
                    prnt[j + 1] = c;
                son[c] = j;

                c = l;
            }
        } while ((c = prnt[c]) != 0);
    }

    int decode_char()
    {
        unsigned c = son[R];
        while (c < (unsigned)T)
        {
            c += get_bit();
            c = son[c];
        }
        c -= T;
        update((int)c);
        return (int)c;
    }

    int decode_position()
    {
        unsigned i = get_byte();
        unsigned c = (unsigned)d_code[i] << 6;
        unsigned j = d_len[i];
        j -= 2;
        while (j--)
            i = (i << 1) + get_bit();
        return (int)(c | (i & 0x3f));
    }
};
} // namespace

bool lzhuf_decompress(const uint8_t* src, size_t src_size, std::vector<uint8_t>& out)
{
    out.clear();
    if (src_size < 4)
        return false;
    const uint32_t text_size = src[0] | (src[1] << 8) | (src[2] << 16) | ((uint32_t)src[3] << 24);
    if (text_size == 0)
        return true;
    // Compression ratio is bounded; reject sizes that cannot come from the input.
    if (text_size > (src_size - 4) * 64ull + 1024)
        return false;

    out.resize(text_size);
    Decoder d(src + 4, src_size - 4);
    d.start_huff();
    memset(d.text_buf, 0x20, N - F);
    int r = N - F;
    size_t count = 0;
    while (count < text_size)
    {
        const int c = d.decode_char();
        if (c < 256)
        {
            out[count++] = (uint8_t)c;
            d.text_buf[r++] = (uint8_t)c;
            r &= (N - 1);
        }
        else
        {
            const int i = (r - d.decode_position() - 1) & (N - 1);
            const int j = c - 255 + THRESHOLD;
            for (int k = 0; k < j; ++k)
            {
                const uint8_t b = d.text_buf[(i + k) & (N - 1)];
                if (count >= text_size)
                    return true; // trailing match overshoot; mirrors the engine, which tolerates it
                out[count++] = b;
                d.text_buf[r++] = b;
                r &= (N - 1);
            }
        }
        // Ran out of input long before output was produced: corrupt stream.
        if (d.in == d.in_end && d.getlen == 0 && count < text_size)
            return false;
    }
    return true;
}
} // namespace xrdb
