// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe > out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=OUT

// OUT: Hit count = 7

#define _GNU_SOURCE       /* for tdestroy() */
#include <assert.h>
#include <search.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Basic types & helpers                                             */
/* ------------------------------------------------------------------ */
typedef unsigned char uchar;

/* xcalloc – small wrapper that aborts on OOM */
static __attribute__((noinline))
void *xcalloc(size_t n, size_t sz)
{
  void *p = calloc(n, sz);
  assert(p && "xcalloc failed");
  return p;
}

/* --- DNA 2-bit encoding table ------------------------------------- */
static uint8_t encoding[256];

static void init_encoding(void)
{
  for (int i = 0; i < 256; i++) encoding[i] = 4;       /* invalid */
  encoding['A'] = encoding['a'] = 0;
  encoding['C'] = encoding['c'] = 1;
  encoding['G'] = encoding['g'] = 2;
  encoding['T'] = encoding['t'] = 3;
}

/* ------------------------------------------------------------------ */
/*  Hash-node & environment structs                                   */
/* ------------------------------------------------------------------ */
typedef struct hash_node {
  unsigned int ecode;   /* 2-bit packed k-mer */
  int          pos;     /* position within pattern */
} hash_node_t, *hash_node_p_t;

typedef struct hash_env {
  unsigned int len;     /* pattern length                   */
  unsigned int W;       /* k-mer width (word size)          */
  unsigned int mask;    /* (1<<(2*(W-1)))-1                 */
  void       **hashtab; /* array of root pointers for tsearch */
  int         *next_pos;/* linked list of pattern positions */
} *hash_env_p_t;

typedef void *collec_p_t;   /* not used in this demo */

/* table size power-of-two so we can & with HASH_MASK */
#define HT_SIZE   1024
#define HASH_MASK (HT_SIZE - 1)

/* ------------------------------------------------------------------ */
/*  hash_node_compare  (from user snippet, unchanged)                 */
/* ------------------------------------------------------------------ */
static __attribute__((noinline))
int hash_node_compare(const void *a, const void *b)
{
  const hash_node_p_t ha = (hash_node_p_t)a, hb = (hash_node_p_t)b;
  if (ha->ecode < hb->ecode) return -1;
  if (ha->ecode > hb->ecode) return  1;
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Stub for extend_hit – just count calls                            */
/* ------------------------------------------------------------------ */
static int hit_counter = 0;

static __attribute__((noinline))
void extend_hit(int p, unsigned int i,
                hash_env_p_t he, uchar *s2, unsigned int len2,
                int K, collec_p_t mCol, int *diag_lev)
{
  (void)p; (void)i; (void)he; (void)s2; (void)len2;
  (void)K; (void)mCol; (void)diag_lev;
  hit_counter++;
}

/* ------------------------------------------------------------------ */
/*  The search() function (verbatim, needles ≤ 16 MiB)                */
/* ------------------------------------------------------------------ */
static void
search(hash_env_p_t he, uchar *s2, unsigned int len2, int K, collec_p_t mCol)
{
  uchar *t;
  unsigned int i = 0;
  int *allocated = xcalloc(he->len + len2 + 1, sizeof(int));
  int *diag_lev  = allocated + he->len;

  t = s2;
  while (i < len2) {
    unsigned int j;
    hash_node_t hn;
restart:
    hn.ecode = 0;
    for (j = 1; j < he->W && i < len2; j++) {
      unsigned int tmp = encoding[*t++];
      i += 1;
      if (tmp > 3) goto restart;
      hn.ecode = (hn.ecode << 2) + tmp;
    }
    while (i < len2) {
      unsigned int tmp = encoding[*t++];
      hash_node_p_t *key;
      i += 1;
      if (tmp > 3) goto restart;
      hn.ecode = ((hn.ecode & he->mask) << 2) + tmp;
      key = tfind(&hn,
                  he->hashtab + (hn.ecode & HASH_MASK),
                  hash_node_compare);
      if (key != NULL) {
        int p;
        for (p = (*key)->pos; p >= 0; p = he->next_pos[p])
          extend_hit(p, i, he, s2, len2, K, mCol, diag_lev);
      }
    }
  }
  free(allocated);
}

/* ------------------------------------------------------------------ */
/*  Build a tiny test pattern                                         */
/* ------------------------------------------------------------------ */
static hash_env_p_t
build_hash_env(const char *pattern, unsigned int W)
{
  const unsigned int len = strlen(pattern);
  hash_env_p_t he = xcalloc(1, sizeof(*he));

  he->len      = len;
  he->W        = W;
  he->mask     = (1u << (2 * (W - 1))) - 1u;
  he->hashtab  = xcalloc(HT_SIZE, sizeof(void *));
  he->next_pos = xcalloc(len, sizeof(int));

  /* pre-compute k-mers and insert into hash table */
  for (unsigned int pos = 0; pos + W <= len; pos++) {
    unsigned int ecode = 0;
    for (unsigned int k = 0; k < W; k++)
      ecode = (ecode << 2) + encoding[(uchar)pattern[pos + k]];

    hash_node_t *node = xcalloc(1, sizeof(*node));
    node->ecode = ecode;
    node->pos   = pos;
    /* link positions with identical ecode */
    node->pos = pos;
    he->next_pos[pos] = -1;

    hash_node_p_t *slot =
        (hash_node_p_t *)tsearch(node,
                                 &he->hashtab[ecode & HASH_MASK],
                                 hash_node_compare);

    if (*slot != node) {                /* duplicate ecode: prepend pos */
      he->next_pos[pos] = (*slot)->pos;
      (*slot)->pos      = pos;
      free(node);                       /* existing node keeps the list */
    }
  }
  return he;
}

/* free helper for tdestroy */
static void free_node(void *p) { free(p); }

size_t walked = 0;


void walk_action(const void *p, const VISIT v, const int d) {
  (void)v; (void)d; if (v == preorder || v == leaf) walked++;
}

/* ------------------------------------------------------------------ */
/*  Main test driver                                                  */
/* ------------------------------------------------------------------ */
int main(void)
{
  init_encoding();

  const char *pattern = "ACGTACGT";     /* length 8, k-mer width 3 */
  const char *text    = "TTACTACGTAAA"; /* will contain several hits */

  hash_env_p_t he = build_hash_env(pattern, 3 /*W*/);

  search(he, (uchar *)text, strlen(text), 0 /*K*/, NULL);

  /* we expect 7 matches */
  printf("Hit count = %d\n", hit_counter);

  /* walk the tree once for good measure */

  for (size_t i = 0; i < HT_SIZE; i++)
    if (he->hashtab[i])
      twalk(he->hashtab[i], walk_action);

  /* delete one key and verify lookup fails */
  hash_node_t probe = { .ecode = ((encoding['A']<<4)|(encoding['C']<<2)|encoding['G']) }; /* "ACG" */
  tdelete(&probe, &he->hashtab[probe.ecode & HASH_MASK], hash_node_compare);
  assert(tfind(&probe, &he->hashtab[probe.ecode & HASH_MASK], hash_node_compare) == NULL);

  /* clean up */
  for (size_t i = 0; i < HT_SIZE; i++)
    if (he->hashtab[i])
      tdestroy(he->hashtab[i], free_node);
  free(he->hashtab);
  free(he->next_pos);
  free(he);

  puts("all tests passed");
  return 0;
}
