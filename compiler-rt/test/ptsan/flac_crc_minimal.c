// This was throwing an assert, so now test
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -S -emit-llvm %s

#include <stdint.h>

/* --- FLAC typedef aliases ------------------------------------------------- */
typedef uint8_t  FLAC__byte;
typedef uint16_t FLAC__uint16;

extern FLAC__uint16 FLAC__crc16_table[8][256];

/* --- Function under test -------------------------------------------------- */
FLAC__uint16 FLAC__crc16(const FLAC__byte *data, uint32_t len)
{
    FLAC__uint16 crc = 0;

    while (len >= 8) {
        crc = FLAC__crc16_table[7][crc >> 8] ^
              FLAC__crc16_table[5][data[2]];

        data += 8;
        len  -= 8;
    }

    return crc;
}
