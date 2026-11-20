// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 60 80 2> err.txt > out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=OUT
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR

// ERR-NOT: Found bad

//OUT: 4.22024E+12
//OUT: 2.19437E+06

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define ntimes_default 10000
#define digits_default 6
#define LEN 32000
#define LEN2 256
static int ntimes = ntimes_default;

#define TYPE double
#define ALIGNMENT 32
static int digits = digits_default;

TYPE array[LEN2*LEN2] __attribute__((aligned(ALIGNMENT)));

// We place all of the data into one single global structure so that we can
// control its exact layout. Otherwise, the performance of the code can become
// very dependent on the exact addresses assigned to arrays at compile time, and
// probing that behavior is not the purpose of this set of benchmarks.
//
// We insert prime-multiple padding in between each array to help ensure that
// the relative offsets of the arrays are unlikely to trigger unmodelled
// architecture specific problems w.r.t. cache behavior or other CPU
// features. For example, this ensures that no two of the arrays will be
// 4K-aliased with each other, which can be important for some Intel processors.
struct GlobalData {
  __attribute__((aligned(ALIGNMENT))) TYPE a[LEN];
  int pad1[3];
  __attribute__((aligned(ALIGNMENT))) TYPE b[LEN];
  int pad2[5];
  __attribute__((aligned(ALIGNMENT))) TYPE c[LEN];
  int pad3[7];
  __attribute__((aligned(ALIGNMENT))) TYPE d[LEN];
  int pad4[11];
  __attribute__((aligned(ALIGNMENT))) TYPE e[LEN];

  int pad5[13];
  __attribute__((aligned(ALIGNMENT))) TYPE aa[LEN2][LEN2];
  int pad6[17];
  __attribute__((aligned(ALIGNMENT))) TYPE bb[LEN2][LEN2];
  int pad7[19];
  __attribute__((aligned(ALIGNMENT))) TYPE cc[LEN2][LEN2];
  int pad8[23];
  __attribute__((aligned(ALIGNMENT))) TYPE tt[LEN2][LEN2];
} global_data;

__attribute__((aligned(ALIGNMENT))) TYPE * const a = global_data.a;
__attribute__((aligned(ALIGNMENT))) TYPE * const b = global_data.b;
__attribute__((aligned(ALIGNMENT))) TYPE * const c = global_data.c;
__attribute__((aligned(ALIGNMENT))) TYPE * const d = global_data.d;
__attribute__((aligned(ALIGNMENT))) TYPE * const e = global_data.e;
__attribute__((aligned(ALIGNMENT))) TYPE (* const aa)[LEN2] = global_data.aa;
__attribute__((aligned(ALIGNMENT))) TYPE (* const bb)[LEN2] = global_data.bb;
__attribute__((aligned(ALIGNMENT))) TYPE (* const cc)[LEN2] = global_data.cc;
__attribute__((aligned(ALIGNMENT))) TYPE (* const tt)[LEN2] = global_data.tt;


//TYPE* __restrict__ array;
TYPE array[LEN2*LEN2] __attribute__((aligned(ALIGNMENT)));

void check(int name){

  TYPE suma = 0;
  TYPE sumb = 0;
  TYPE sumc = 0;
  TYPE sumd = 0;
  TYPE sume = 0;
  for (int i = 0; i < LEN; i++){
    suma += a[i];
    sumb += b[i];
    sumc += c[i];
    sumd += d[i];
    sume += e[i];
  }
  TYPE sumaa = 0;
  TYPE sumbb = 0;
  TYPE sumcc = 0;
  for (int i = 0; i < LEN2; i++){
    for (int j = 0; j < LEN2; j++){
      sumaa += aa[i][j];
      sumbb += bb[i][j];
      sumcc += cc[i][j];

    }
  }
  TYPE sumarray = 0;
  for (int i = 0; i < LEN2*LEN2; i++){
    sumarray += array[i];
  }

  if (name == 1) printf("%.*G \n",digits,suma);
  if (name == 2) printf("%.*G \n",digits,sumb);
  if (name == 3) printf("%.*G \n",digits,sumc);
  if (name == 4) printf("%.*G \n",digits,sumd);
  if (name == 5) printf("%.*G \n",digits,sume);
  if (name == 11) printf("%.*G \n",digits,sumaa);
  if (name == 22) printf("%.*G \n",digits,sumbb);
  if (name == 33) printf("%.*G \n",digits,sumcc);
  if (name == 0) printf("%.*G \n",digits,sumarray);
  if (name == 12) printf("%.*G \n",digits,suma+sumb);
  if (name == 25) printf("%.*G \n",digits,sumb+sume);
  if (name == 13) printf("%.*G \n",digits,suma+sumc);
  if (name == 123) printf("%.*G \n",digits,suma+sumb+sumc);
  if (name == 1122) printf("%.*G \n",digits,sumaa+sumbb);
  if (name == 112233) printf("%.*G \n",digits,sumaa+sumbb+sumcc);
  if (name == 111) printf("%.*G \n",digits,sumaa+suma);

}

__attribute__((noinline))
int dummy(TYPE[LEN], TYPE[LEN], TYPE[LEN], TYPE[LEN], TYPE[LEN], TYPE[LEN2][LEN2], TYPE[LEN2][LEN2], TYPE[LEN2][LEN2], TYPE) {
  return 0;
}

__attribute__((noinline))
int set1d(TYPE arr[LEN], TYPE value, int stride)
{
  if (stride == -1) {
    for (int i = 0; i < LEN; i++) {
      arr[i] = 1. / (TYPE) (i+1);
    }
  } else if (stride == -2) {
    for (int i = 0; i < LEN; i++) {
      arr[i] = 1. / (TYPE) ((i+1) * (i+1));
    }
  } else {
    for (int i = 0; i < LEN; i += stride) {
      arr[i] = value;
    }
  }
  return 0;
}

__attribute__((noinline))
int set1ds(int _n, TYPE arr[LEN], TYPE value, int stride)
{
	if (stride == -1) {
		for (int i = 0; i < LEN; i++) {
			arr[i] = 1. / (TYPE) (i+1);
		}
	} else if (stride == -2) {
		for (int i = 0; i < LEN; i++) {
			arr[i] = 1. / (TYPE) ((i+1) * (i+1));
		}
	} else {
		for (int i = 0; i < LEN; i += stride) {
			arr[i] = value;
		}
	}
	return 0;
}


__attribute__((noinline))
int set2d(TYPE arr[LEN2][LEN2], TYPE value, int stride)
{

//  -- initialize two-dimensional arraysft

  if (stride == -1) {
    for (int i = 0; i < LEN2; i++) {
      for (int j = 0; j < LEN2; j++) {
        arr[i][j] = 1. / (TYPE) (i+1);
      }
    }
  } else if (stride == -2) {
    for (int i = 0; i < LEN2; i++) {
      for (int j = 0; j < LEN2; j++) {
        arr[i][j] = 1. / (TYPE) ((i+1) * (i+1));
      }
    }
  } else {
    for (int i = 0; i < LEN2; i++) {
      for (int j = 0; j < LEN2; j += stride) {
        arr[i][j] = value;
      }
    }
  }
  return 0;
}

__attribute__((noinline))
void init(char *name) {
  TYPE any=0.;
  TYPE zero=0.;
  TYPE half=.5;
  TYPE one=1.;
  TYPE two=2.;
  TYPE small = .000001;
  int unit =1;
  int frac=-1;
  int frac2=-2;

  if(!strcmp(name, "s235")) {
    set1d(a, one,unit);
    set1d(b, any,frac);
    set1d(c, any,frac);
    set2d(aa, one,unit);
    set2d(bb, any, frac2);
    return;
  }

  if (!strcmp(name, "s141")) {
		set1ds(LEN*LEN,array, one,unit);
		set2d(bb, any,frac2);
    return;
  }

  printf("Unknown name %s", name);
  exit(1);
}

__attribute__((noinline))
int s235()
{

//  loop interchanging
//  imperfectly nested loops


  init("s235");

  for (int nl = 0; nl < 200*(ntimes/LEN2); nl++) {
    for (int i = 0; i < LEN2; i++) {
      a[i] += b[i] * c[i];
      for (int j = 1; j < LEN2; j++) {
        aa[j][i] = aa[j-1][i] + bb[j][i] * a[i];
      }
    }
    dummy(a, b, c, d, e, aa, bb, cc, 0.);
  }
  check(111);
  return 0;
}

// Original
#if 0
int s141()
{

//	nonlinear dependence testing
//	walk a row in a symmetric packed array
//	element a(i,j) for (int j>i) stored in location j*(j-1)/2+i


	init( "s141");

	int k;
	for (int nl = 0; nl < 200*(ntimes/LEN2); nl++) {
		for (int i = 0; i < LEN2; i++) {
			k = (i+1) * ((i+1) - 1) / 2 + (i+1)-1;
			for (int j = i; j < LEN2; j++) {
				array[k] += bb[j][i];
				k += j+1;
			}
		}
		dummy(a, b, c, d, e, aa, bb, cc, 0.);
	}
	check(0);
	return 0;
}
#endif

// simplified
int s141()
{

//	nonlinear dependence testing
//	walk a row in a symmetric packed array
//	element a(i,j) for (int j>i) stored in location j*(j-1)/2+i


	init( "s141");

	int k;
	for (int nl = 0; nl < 200*(ntimes/LEN2); nl++) {
		for (int i = 0; i < LEN2; i++) {
			k = (i+1) * ((i+1) - 1) / 2 + (i+1)-1;
			for (int j = i; j < LEN2; j++) {
				array[k] += k;
				k += j+1;
			}
		}
		dummy(a, b, c, d, e, aa, bb, cc, 0.);
	}
	check(0);
	return 0;
}

int main() {
  s141();
  s235();
}
