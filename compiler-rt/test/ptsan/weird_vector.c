// REQUIRES: x86_64-target-arch
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -mavx2 -o %exe %s
// RUN: ./%exe 2> err.txt
// RUN: ./%exe > out.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR
// RUN: cat out.txt | FileCheck %s --check-prefix=OUT

// ERR-NOT: Found bad
// OUT: 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22

#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

/* ------------------------------------------------------------------ */
/* Minimal FLAC aliases so the original prototype compiles.           */
typedef int32_t  FLAC__int32;
#define FLAC__ASSERT(x)  assert(x)

void __attribute__((noinline))
FLAC__lpc_compute_residual_from_qlp_coefficients_16_intrin_avx2(const FLAC__int32 *data, uint32_t data_len, const FLAC__int32 qlp_coeff[], uint32_t order, int lp_quantization, FLAC__int32 residual[])
{
	int i;
	FLAC__int32 sum;
	const __m128i cnt = _mm_cvtsi32_si128(lp_quantization);

	if(order <= 12) {
  if(order > 4) {
			if(order > 6) {
				if(order == 8) {
					__m256i q0, q1, q2, q3, q4, q5, q6, q7;
					q0  = _mm256_set1_epi32(0xffff & qlp_coeff[0 ]);
					q1  = _mm256_set1_epi32(0xffff & qlp_coeff[1 ]);
					q2  = _mm256_set1_epi32(0xffff & qlp_coeff[2 ]);
					q3  = _mm256_set1_epi32(0xffff & qlp_coeff[3 ]);
					q4  = _mm256_set1_epi32(0xffff & qlp_coeff[4 ]);
					q5  = _mm256_set1_epi32(0xffff & qlp_coeff[5 ]);
					q6  = _mm256_set1_epi32(0xffff & qlp_coeff[6 ]);
					q7  = _mm256_set1_epi32(0xffff & qlp_coeff[7 ]);

					for(i = 0; i < (int)data_len-7; i+=8) {
						__m256i summ, mull;
						summ = _mm256_madd_epi16(q7,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-8 )));
						mull = _mm256_madd_epi16(q6,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-7 ))); summ = _mm256_add_epi32(summ, mull);
						mull = _mm256_madd_epi16(q5,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-6 ))); summ = _mm256_add_epi32(summ, mull);
						mull = _mm256_madd_epi16(q4,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-5 ))); summ = _mm256_add_epi32(summ, mull);
						mull = _mm256_madd_epi16(q3,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-4 ))); summ = _mm256_add_epi32(summ, mull);
						mull = _mm256_madd_epi16(q2,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-3 ))); summ = _mm256_add_epi32(summ, mull);
						mull = _mm256_madd_epi16(q1,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-2 ))); summ = _mm256_add_epi32(summ, mull);
						mull = _mm256_madd_epi16(q0,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-1 ))); summ = _mm256_add_epi32(summ, mull);
						summ = _mm256_sra_epi32(summ, cnt);
						_mm256_storeu_si256((__m256i*)(void*)(residual+i), _mm256_sub_epi32(_mm256_loadu_si256((const __m256i*)(const void*)(data+i)), summ));
					}
				}
				else { /* order == 7 */
					__m256i q0, q1, q2, q3, q4, q5, q6;
					q0  = _mm256_set1_epi32(0xffff & qlp_coeff[0 ]);
					q1  = _mm256_set1_epi32(0xffff & qlp_coeff[1 ]);
					q2  = _mm256_set1_epi32(0xffff & qlp_coeff[2 ]);
					q3  = _mm256_set1_epi32(0xffff & qlp_coeff[3 ]);
					q4  = _mm256_set1_epi32(0xffff & qlp_coeff[4 ]);
					q5  = _mm256_set1_epi32(0xffff & qlp_coeff[5 ]);
					q6  = _mm256_set1_epi32(0xffff & qlp_coeff[6 ]);

					for(i = 0; i < (int)data_len-7; i+=8) {
						__m256i summ, mull;
						summ = _mm256_madd_epi16(q6,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-7 )));
						mull = _mm256_madd_epi16(q5,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-6 ))); summ = _mm256_add_epi32(summ, mull);
						mull = _mm256_madd_epi16(q4,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-5 ))); summ = _mm256_add_epi32(summ, mull);
						mull = _mm256_madd_epi16(q3,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-4 ))); summ = _mm256_add_epi32(summ, mull);
						mull = _mm256_madd_epi16(q2,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-3 ))); summ = _mm256_add_epi32(summ, mull);
						mull = _mm256_madd_epi16(q1,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-2 ))); summ = _mm256_add_epi32(summ, mull);
						mull = _mm256_madd_epi16(q0,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-1 ))); summ = _mm256_add_epi32(summ, mull);
						summ = _mm256_sra_epi32(summ, cnt);
						_mm256_storeu_si256((__m256i*)(void*)(residual+i), _mm256_sub_epi32(_mm256_loadu_si256((const __m256i*)(const void*)(data+i)), summ));
					}
				}
			}
			else {
				if(order == 6) {
					__m256i q0, q1, q2, q3, q4, q5;
					q0  = _mm256_set1_epi32(0xffff & qlp_coeff[0 ]);
					q1  = _mm256_set1_epi32(0xffff & qlp_coeff[1 ]);
					q2  = _mm256_set1_epi32(0xffff & qlp_coeff[2 ]);
					q3  = _mm256_set1_epi32(0xffff & qlp_coeff[3 ]);
					q4  = _mm256_set1_epi32(0xffff & qlp_coeff[4 ]);
					q5  = _mm256_set1_epi32(0xffff & qlp_coeff[5 ]);

					for(i = 0; i < (int)data_len-7; i+=8) {
						__m256i summ, mull;
						summ = _mm256_madd_epi16(q5,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-6 )));
						mull = _mm256_madd_epi16(q4,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-5 ))); summ = _mm256_add_epi32(summ, mull);
						mull = _mm256_madd_epi16(q3,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-4 ))); summ = _mm256_add_epi32(summ, mull);
						mull = _mm256_madd_epi16(q2,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-3 ))); summ = _mm256_add_epi32(summ, mull);
						mull = _mm256_madd_epi16(q1,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-2 ))); summ = _mm256_add_epi32(summ, mull);
						mull = _mm256_madd_epi16(q0,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-1 ))); summ = _mm256_add_epi32(summ, mull);
						summ = _mm256_sra_epi32(summ, cnt);
						_mm256_storeu_si256((__m256i*)(void*)(residual+i), _mm256_sub_epi32(_mm256_loadu_si256((const __m256i*)(const void*)(data+i)), summ));
					}
				}
				else { /* order == 5 */
					__m256i q0, q1, q2, q3, q4;
					q0  = _mm256_set1_epi32(0xffff & qlp_coeff[0 ]);
					q1  = _mm256_set1_epi32(0xffff & qlp_coeff[1 ]);
					q2  = _mm256_set1_epi32(0xffff & qlp_coeff[2 ]);
					q3  = _mm256_set1_epi32(0xffff & qlp_coeff[3 ]);
					q4  = _mm256_set1_epi32(0xffff & qlp_coeff[4 ]);

					for(i = 0; i < (int)data_len-7; i+=8) {
						__m256i summ, mull;
						summ = _mm256_madd_epi16(q4,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-5 )));
						mull = _mm256_madd_epi16(q3,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-4 ))); summ = _mm256_add_epi32(summ, mull);
						mull = _mm256_madd_epi16(q2,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-3 ))); summ = _mm256_add_epi32(summ, mull);
						mull = _mm256_madd_epi16(q1,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-2 ))); summ = _mm256_add_epi32(summ, mull);
						mull = _mm256_madd_epi16(q0,  _mm256_loadu_si256((const __m256i*)(const void*)(data+i-1 ))); summ = _mm256_add_epi32(summ, mull);
						summ = _mm256_sra_epi32(summ, cnt);
						_mm256_storeu_si256((__m256i*)(void*)(residual+i), _mm256_sub_epi32(_mm256_loadu_si256((const __m256i*)(const void*)(data+i)), summ));
					}
				}
			}
		}
		for(; i < (int)data_len; i++) {
			sum = 0;
			switch(order) {
				case 12: sum += qlp_coeff[11] * data[i-12]; /* Falls through. */
				case 11: sum += qlp_coeff[10] * data[i-11]; /* Falls through. */
				case 10: sum += qlp_coeff[ 9] * data[i-10]; /* Falls through. */
				case 9:  sum += qlp_coeff[ 8] * data[i- 9]; /* Falls through. */
				case 8:  sum += qlp_coeff[ 7] * data[i- 8]; /* Falls through. */
				case 7:  sum += qlp_coeff[ 6] * data[i- 7]; /* Falls through. */
				case 6:  sum += qlp_coeff[ 5] * data[i- 6]; /* Falls through. */
				case 5:  sum += qlp_coeff[ 4] * data[i- 5]; /* Falls through. */
				case 4:  sum += qlp_coeff[ 3] * data[i- 4]; /* Falls through. */
				case 3:  sum += qlp_coeff[ 2] * data[i- 3]; /* Falls through. */
				case 2:  sum += qlp_coeff[ 1] * data[i- 2]; /* Falls through. */
				case 1:  sum += qlp_coeff[ 0] * data[i- 1];
			}
			residual[i] = data[i] - (sum >> lp_quantization);
		}
	}
	else { /* order > 12 */
		for(i = 0; i < (int)data_len; i++) {
			sum = 0;
			switch(order) {
				case 32: sum += qlp_coeff[31] * data[i-32]; /* Falls through. */
				case 31: sum += qlp_coeff[30] * data[i-31]; /* Falls through. */
				case 30: sum += qlp_coeff[29] * data[i-30]; /* Falls through. */
				case 29: sum += qlp_coeff[28] * data[i-29]; /* Falls through. */
				case 28: sum += qlp_coeff[27] * data[i-28]; /* Falls through. */
				case 27: sum += qlp_coeff[26] * data[i-27]; /* Falls through. */
				case 26: sum += qlp_coeff[25] * data[i-26]; /* Falls through. */
				case 25: sum += qlp_coeff[24] * data[i-25]; /* Falls through. */
				case 24: sum += qlp_coeff[23] * data[i-24]; /* Falls through. */
				case 23: sum += qlp_coeff[22] * data[i-23]; /* Falls through. */
				case 22: sum += qlp_coeff[21] * data[i-22]; /* Falls through. */
				case 21: sum += qlp_coeff[20] * data[i-21]; /* Falls through. */
				case 20: sum += qlp_coeff[19] * data[i-20]; /* Falls through. */
				case 19: sum += qlp_coeff[18] * data[i-19]; /* Falls through. */
				case 18: sum += qlp_coeff[17] * data[i-18]; /* Falls through. */
				case 17: sum += qlp_coeff[16] * data[i-17]; /* Falls through. */
				case 16: sum += qlp_coeff[15] * data[i-16]; /* Falls through. */
				case 15: sum += qlp_coeff[14] * data[i-15]; /* Falls through. */
				case 14: sum += qlp_coeff[13] * data[i-14]; /* Falls through. */
				case 13: sum += qlp_coeff[12] * data[i-13];
				         sum += qlp_coeff[11] * data[i-12];
				         sum += qlp_coeff[10] * data[i-11];
				         sum += qlp_coeff[ 9] * data[i-10];
				         sum += qlp_coeff[ 8] * data[i- 9];
				         sum += qlp_coeff[ 7] * data[i- 8];
				         sum += qlp_coeff[ 6] * data[i- 7];
				         sum += qlp_coeff[ 5] * data[i- 6];
				         sum += qlp_coeff[ 4] * data[i- 5];
				         sum += qlp_coeff[ 3] * data[i- 4];
				         sum += qlp_coeff[ 2] * data[i- 3];
				         sum += qlp_coeff[ 1] * data[i- 2];
				         sum += qlp_coeff[ 0] * data[i- 1];
			}
			residual[i] = data[i] - (sum >> lp_quantization);
		}
	}
	_mm256_zeroupper();
}

/* ------------------------------------------------------------------ */
int main(void)
{
    const uint32_t order = 7;                 /* must be ≤ 12 for AVX2 path */
    const uint32_t data_len = 32;             /* any size ≥ 8 works        */
    const int      lp_quant = 12;

    /* Predictor coefficients: a simple ramp 1…8 */
    FLAC__int32 qlp_coeff[order];
    for (unsigned i = 0; i < order; ++i)
        qlp_coeff[i] = i + 1;

		printf("Finished predictor gen\n");

    /* Input signal: need extra `order` samples *before* the pointer because
       the algorithm accesses data[i‑order] … data[i‑1]. */
    const unsigned PAD   = order;
    FLAC__int32   *alloc = calloc(data_len + PAD, sizeof(*alloc));
    FLAC__int32   *data  = alloc + PAD;       /* point past the pad area   */

    /* Initialise the “history” (negative indices) and the actual samples.   */
    for (unsigned i = 0; i < data_len + PAD; ++i)
        alloc[i] = (FLAC__int32)i;            /* 0,1,2,3,… for visibility  */

		printf("Finished history\n");

    FLAC__int32 residual[data_len];

    FLAC__lpc_compute_residual_from_qlp_coefficients_16_intrin_avx2(
        data, data_len, qlp_coeff, order, lp_quant, residual);

    puts("first 16 residuals:");
    for (unsigned i = 0; i < 16 && i < data_len; ++i)
        printf("%d ", residual[i]);
    putchar('\n');

    free(alloc);
    return 0;
}