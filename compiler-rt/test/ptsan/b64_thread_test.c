// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -I%crt_src/include -I%crt_src/lib -o %exe %s
// RUN: ./%exe 2> err.txt 1> out.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR

// ERR: Unreclaimed IDs at exit: 0

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>

#define NUM_THREADS  200

// String of base64 chars and their inverses for quick lookup for encoding and decoding
static const char *b64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Calculate the length of the final base64 encoded string
size_t b64_encoded_size(size_t inlen)
{
	size_t ret;

	ret = inlen;
	if (inlen % 3 != 0)
		ret += 3 - (inlen % 3);
	ret /= 3;
	ret *= 4;

	return ret;
}

// Unsafely base64 encode a string given a string and its length with no NULL check.
// If a NULL ptr is passed, it will be dereferenced causing a crash
char *b64_encode(char *in, size_t len)
{
	char   *out;
	size_t  elen;
	size_t  i;
	size_t  j;
	size_t  v;

	elen = b64_encoded_size(len);
	out  = malloc(elen+1);
	out[elen] = '\0';

	for (i=0, j=0; i<len; i+=3, j+=4) {
		v = in[i];
		v = i+1 < len ? v << 8 | in[i+1] : v << 8;
		v = i+2 < len ? v << 8 | in[i+2] : v << 8;

		out[j]   = b64chars[(v >> 18) & 0x3F];
		out[j+1] = b64chars[(v >> 12) & 0x3F];
		if (i+1 < len) {
			out[j+2] = b64chars[(v >> 6) & 0x3F];
		} else {
			out[j+2] = '=';
		}
		if (i+2 < len) {
			out[j+3] = b64chars[v & 0x3F];
		} else {
			out[j+3] = '=';
		}
	}

	return out;
}

/* Thread entry function */
static void *thread_func(void *arg) {
    char *input = (char *)arg;

    char *encoded = b64_encode(input, strlen(input));

    printf("Encoded in thread %lu: %s\n", pthread_self(), encoded);
    free(encoded);

    return NULL;
}

int main(void) {
    pthread_t threads[NUM_THREADS];
    char *s = "Hello hello kitty, konichiwa!\n";

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, thread_func, s);
    }

    /* Wait for all threads to finish (or crash). */
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    return 0;
}