// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt 1> out.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR
// RUN: cat out.txt | FileCheck %s

// ERR: Unreclaimed IDs at exit: 0

// Thanks ChatGPT!
#include <stdio.h>
#include <string.h>

int main(void) {
    char buf1[10] = {0};
    char buf2[10] = "UNCHANGED";
    char buf3[10] = {0};
    char buf4[ 5] = {0};
    char buf5[10] = {0};
    char buf6[10] = {0};
    char buf7[10] = {0};
    char fld1[16] = {0}, fld2[16] = {0}, fld3[ 5] = {0};
    char buf9[64] = {0};
    int r;

    // 1) positive scanset that includes ']' and 'a':
    r = sscanf("]a]b", "%[]a]", buf1);
    printf("r=%d, buf1=\"%s\"\n", r, buf1);
    //CHECK: r=1, buf1="]a]"

    // 2) negative scanset excluding ']' – reads up to the first ']'
    r = sscanf("abc]def", "%[^]]", buf2);
    printf("r=%d, buf2=\"%s\"\n", r, buf2);
    //CHECK: r=1, buf2="abc"

    // 3) if the very first char is excluded, r==0, buf unchanged
    r = sscanf("]start", "%[^]]", buf3);
    printf("r=%d, buf3=\"%s\"\n", r, buf3);
    //CHECK: r=0, buf3=""

    // 4) width‑limited scanset: read at most 4 digits
    r = sscanf("123456", "%4[0-9]", buf4);
    printf("r=%d, buf4=\"%s\"\n", r, buf4);
    //CHECK: r=1, buf4="1234"

    // 5) scanset with a literal '^' (not negation since '^' isn't first)
    r = sscanf("a^b~", "%[a^b]", buf5);
    printf("r=%d, buf5=\"%s\"\n", r, buf5);
    //CHECK: r=1, buf5="a^b"

    // 6) scanset with a literal '^' as last character (make sure we get the end)
    r = sscanf("a^~", "%[a^]", buf6);
    printf("r=%d, buf6=\"%s\"\n", r, buf6);
    //CHECK: r=1, buf6="a^"

    // 7) scanset with a literal '-' (not a range, since '-' is last)
    r = sscanf("a-b-c", "%[ab-]", buf6);
    printf("r=%d, buf7=\"%s\"\n", r, buf6);
    //CHECK: r=1, buf7="a-b-"

    // 8) from cjson “ADD %15[^,], %15[^,], %4s” example
    r = sscanf("ADD Ford, F150, 2010",
               "%15[^,], %15[^,], %4s",
               fld1, fld2, fld3);
    printf("r=%d, fld1=\"%s\", fld2=\"%s\", fld3=\"%s\"\n",
           r, fld1, fld2, fld3);
    //CHECK: r=3, fld1="ADD Ford", fld2="F150", fld3="2010"

    r = sscanf("SKIP_THIS,WAKE", "%*[^,],%31[^,]", buf9);
    printf("r=%d, buf9=\"%s\"\n", r, buf9);
    //CHECK: r=1, buf9="WAKE"

}
