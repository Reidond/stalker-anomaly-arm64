// LZO1X decompressor (bounds-checked). Decodes the raw block format produced by lzo1x_1_compress,
// which is what rtc_compress writes for compressed entries in .db archives.
#include "xrdb.h"

#include <cstring>

namespace xrdb
{
bool lzo1x_decompress(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len)
{
    const uint8_t* ip = in;
    const uint8_t* const ip_end = in + in_len;
    uint8_t* op = out;
    uint8_t* const op_end = out + out_len;
    const uint8_t* m_pos;
    size_t t;

#define NEED_IN(n)  do { if ((size_t)(ip_end - ip) < (size_t)(n)) return false; } while (0)
#define NEED_OUT(n) do { if ((size_t)(op_end - op) < (size_t)(n)) return false; } while (0)
#define COPY_LIT(n) do { NEED_IN(n); NEED_OUT(n); memcpy(op, ip, (n)); op += (n); ip += (n); } while (0)
// Overlap-safe back reference copy.
#define COPY_MATCH(n) do {                                   \
        if (m_pos < out || m_pos >= op) return false;         \
        NEED_OUT(n);                                          \
        for (size_t i_ = 0; i_ < (size_t)(n); ++i_) *op++ = *m_pos++; \
    } while (0)
#define EXT_LEN(base) do {                                    \
        NEED_IN(1);                                           \
        while (*ip == 0) { t += 255; ++ip; NEED_IN(1); }      \
        t += (base) + *ip++;                                  \
    } while (0)

    NEED_IN(1);
    if (*ip > 17)
    {
        t = *ip++ - 17;
        if (t < 4)
            goto match_next;
        COPY_LIT(t);
        goto first_literal_run;
    }

    for (;;)
    {
        NEED_IN(1);
        t = *ip++;
        if (t >= 16)
            goto match;
        if (t == 0)
            EXT_LEN(15);
        COPY_LIT(t + 3);

    first_literal_run:
        NEED_IN(2);
        t = *ip++;
        if (t >= 16)
            goto match;
        m_pos = op - (1 + 0x0800) - (t >> 2) - ((size_t)*ip++ << 2);
        COPY_MATCH(3);
        goto match_done;

        for (;;)
        {
        match:
            if (t >= 64)
            {
                NEED_IN(1);
                m_pos = op - 1 - ((t >> 2) & 7) - ((size_t)*ip++ << 3);
                t = (t >> 5) - 1;
                COPY_MATCH(t + 2);
                goto match_done;
            }
            else if (t >= 32)
            {
                t &= 31;
                if (t == 0)
                    EXT_LEN(31);
                NEED_IN(2);
                m_pos = op - 1 - (ip[0] >> 2) - ((size_t)ip[1] << 6);
                ip += 2;
            }
            else if (t >= 16)
            {
                m_pos = op - ((t & 8) << 11);
                t &= 7;
                if (t == 0)
                    EXT_LEN(7);
                NEED_IN(2);
                m_pos -= (ip[0] >> 2) + ((size_t)ip[1] << 6);
                ip += 2;
                if (m_pos == op)
                    return ip == ip_end && op == op_end; // end-of-stream marker
                m_pos -= 0x4000;
            }
            else
            {
                NEED_IN(1);
                m_pos = op - 1 - (t >> 2) - ((size_t)*ip++ << 2);
                COPY_MATCH(2);
                goto match_done;
            }
            COPY_MATCH(t + 2);

        match_done:
            NEED_IN(2);
            t = ip[-2] & 3;
            if (t == 0)
                break;
        match_next:
            COPY_LIT(t);
            NEED_IN(1);
            t = *ip++;
        }
    }
#undef NEED_IN
#undef NEED_OUT
#undef COPY_LIT
#undef COPY_MATCH
#undef EXT_LEN
}
} // namespace xrdb
