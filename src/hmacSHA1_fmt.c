/*
 * This software is Copyright (c) 2012 magnum, and it is hereby released to the
 * general public under the following terms:  Redistribution and use in source
 * and binary forms, with or without modification, are permitted.
 *
 * Based on hmac-md5 by Bartavelle
 */

#include <string.h>

#include "arch.h"
#include "misc.h"
#include "common.h"
#include "formats.h"
#include "sha.h"
#include "johnswap.h"
#include "sse-intrinsics.h"

#define FORMAT_LABEL			"hmac-sha1"
#define FORMAT_NAME			"HMAC SHA-1"

#ifdef SHA1_SSE_PARA
#define SHA1_N				(SHA1_SSE_PARA*MMX_COEF)
#else
#define SHA1_N				MMX_COEF
#endif

#define ALGORITHM_NAME			SHA1_ALGORITHM_NAME

#define BENCHMARK_COMMENT		""
#define BENCHMARK_LENGTH		0

#define PLAINTEXT_LENGTH		125

#define PAD_SIZE			64
#define BINARY_SIZE			20
#define BINARY_ALIGN			sizeof(ARCH_WORD_32)
#define SALT_SIZE			PAD_SIZE
#define SALT_ALIGN			MEM_ALIGN_NONE
#define CIPHERTEXT_LENGTH		(SALT_SIZE + 1 + BINARY_SIZE * 2)

#ifdef MMX_COEF
#define MIN_KEYS_PER_CRYPT		SHA1_N
#define MAX_KEYS_PER_CRYPT		SHA1_N
#define GETPOS(i, index)		( (index&(MMX_COEF-1))*4 + ((i)&(0xffffffff-3) )*MMX_COEF + (3-((i)&3)) + (index>>(MMX_COEF>>1))*SHA_BUF_SIZ*4*MMX_COEF ) //for endianity conversion

#else
#define MIN_KEYS_PER_CRYPT		1
#define MAX_KEYS_PER_CRYPT		1
#endif

static struct fmt_tests tests[] = {
	{"The quick brown fox jumps over the lazy dog#de7c9b85b8b78aa6bc8a7a36f70a90701c9db4d9", "key"},
	{"#fbdb1d1b18aa6c08324b7d64b71fb76370690e1d", ""},
	{"Beppe#Grillo#DEBBDB4D549ABE59FAB67D0FB76B76FDBC4431F1", "Io credo nella reincarnazione e sono di Genova; per cui ho fatto testamento e mi sono lasciato tutto a me."},
	{"7oTwG04WUjJ0BTDFFIkTJlgl#293b75c1f28def530c17fc8ae389008179bf4091", "late*night"}, // from the test suite
	{NULL}
};

#ifdef MMX_COEF
/* Cygwin would not guarantee the alignment if these were declared static */
#define crypt_key hmacsha1_crypt_key
#define opad hmacsha1_opad
#define ipad hmacsha1_ipad
#define cursalt hmacsha1_cursalt
#define dump hmacsha1_dump

ALIGN(16) unsigned char crypt_key[SHA_BUF_SIZ*4*SHA1_N];
ALIGN(16) unsigned char opad[SHA_BUF_SIZ*4*SHA1_N];
ALIGN(16) unsigned char ipad[SHA_BUF_SIZ*4*SHA1_N];
ALIGN(16) unsigned char cursalt[SHA_BUF_SIZ*4*SHA1_N];
ALIGN(16) unsigned char dump[BINARY_SIZE*SHA1_N];
static char saved_plain[SHA1_N][PLAINTEXT_LENGTH + 1];
#else
static ARCH_WORD_32 crypt_key[BINARY_SIZE/4];
static unsigned char opad[PAD_SIZE];
static unsigned char ipad[PAD_SIZE];
static unsigned char cursalt[SALT_SIZE];
static char saved_plain[PLAINTEXT_LENGTH + 1];
#endif

#ifdef MMX_COEF
static void clear_keys(void)
{
	memset(ipad, 0x36, sizeof(ipad));
	memset(opad, 0x5C, sizeof(opad));
}
#endif

static void init(struct fmt_main *self)
{
#ifdef MMX_COEF
	int i;
	for (i = 0; i < SHA1_N; ++i) {
		crypt_key[GETPOS(BINARY_SIZE,i)] = 0x80;
		((unsigned int*)crypt_key)[15*MMX_COEF + (i&3) + (i>>2)*SHA_BUF_SIZ*MMX_COEF] = (BINARY_SIZE+64)<<3;
	}
	clear_keys();
#endif
}

static int valid(char *ciphertext, struct fmt_main *self)
{
	int pos, i;
	char *p;

	p = strrchr(ciphertext, '#'); // allow # in salt
	if (!p || p > &ciphertext[strlen(ciphertext)-1]) return 0;
	i = (int)(p - ciphertext);
#if MMX_COEF
	if(i > 55) return 0;
#else
	if(i > SALT_SIZE) return 0;
#endif
	pos = i+1;
	if (strlen(ciphertext+pos) != BINARY_SIZE*2) return 0;
	for (i = pos; i < BINARY_SIZE*2+pos; i++)
	{
		if (!(  (('0' <= ciphertext[i])&&(ciphertext[i] <= '9')) ||
		        (('a' <= ciphertext[i])&&(ciphertext[i] <= 'f'))
		        || (('A' <= ciphertext[i])&&(ciphertext[i] <= 'F'))))
			return 0;
	}
	return 1;
}

static char *split(char *ciphertext, int index, struct fmt_main *self)
{
	static char out[CIPHERTEXT_LENGTH + 1];

	strnzcpy(out, ciphertext, CIPHERTEXT_LENGTH + 1);
	strlwr(strrchr(out, '#'));

	return out;
}

static void set_salt(void *salt)
{
#ifdef MMX_COEF
	memcpy(cursalt, salt, SHA_BUF_SIZ*4*SHA1_N);
#else
	memcpy(cursalt, salt, SALT_SIZE);
#endif
}

static void set_key(char *key, int index)
{
	int len;
#ifdef MMX_COEF
	ARCH_WORD_32 *ipadp = (ARCH_WORD_32*)&ipad[GETPOS(3, index)];
	ARCH_WORD_32 *opadp = (ARCH_WORD_32*)&opad[GETPOS(3, index)];
	const ARCH_WORD_32 *keyp = (ARCH_WORD_32*)key;
	unsigned int temp;

	len = strlen(key);
	memcpy(saved_plain[index], key, len);
	saved_plain[index][len] = 0;

	if (len > PAD_SIZE) {
		unsigned char k0[BINARY_SIZE];
		SHA_CTX ctx;
		int i;

		SHA1_Init( &ctx );
		SHA1_Update( &ctx, key, len);
		SHA1_Final( k0, &ctx);

		keyp = (unsigned int*)k0;
		for(i = 0; i < BINARY_SIZE / 4; i++, ipadp += MMX_COEF, opadp += MMX_COEF)
		{
			temp = JOHNSWAP(*keyp++);
			*ipadp ^= temp;
			*opadp ^= temp;
		}
	}
	else
	while(((temp = JOHNSWAP(*keyp++)) & 0xff000000)) {
		if (!(temp & 0x00ff0000) || !(temp & 0x0000ff00))
		{
			((unsigned short*)ipadp)[1] ^=
				(unsigned short)(temp>>16);
			((unsigned short*)opadp)[1] ^=
				(unsigned short)(temp>>16);
			break;
		}
		*ipadp ^= temp;
		*opadp ^= temp;
		if (!(temp & 0x000000ff))
			break;
		ipadp += MMX_COEF;
		opadp += MMX_COEF;
	}
#else
	int i;

	len = strlen(key);
	memcpy(saved_plain, key, len);
	saved_plain[len] = 0;

	memset(ipad, 0x36, PAD_SIZE);
	memset(opad, 0x5C, PAD_SIZE);

	if (len > PAD_SIZE) {
		SHA_CTX ctx;
		unsigned char k0[BINARY_SIZE];

		SHA1_Init( &ctx );
		SHA1_Update( &ctx, key, len);
		SHA1_Final( k0, &ctx);

		len = BINARY_SIZE;

		for(i=0;i<len;i++)
		{
			ipad[i] ^= k0[i];
			opad[i] ^= k0[i];
		}
	}
	else
	for(i=0;i<len;i++)
	{
		ipad[i] ^= key[i];
		opad[i] ^= key[i];
	}
#endif
}

static char *get_key(int index)
{
#ifdef MMX_COEF
	return saved_plain[index];
#else
	return saved_plain;
#endif
}

static int cmp_all(void *binary, int count)
{
#ifdef MMX_COEF
	unsigned int x,y=0;

#if SHA1_SSE_PARA
	for(;y<SHA1_SSE_PARA;y++)
#endif
		for(x=0;x<MMX_COEF;x++)
		{
			// NOTE crypt_key is in input format (4*SHA_BUF_SIZ*MMX_COEF)
			if( ((ARCH_WORD_32*)binary)[0] == ((ARCH_WORD_32*)crypt_key)[x+y*MMX_COEF*SHA_BUF_SIZ] )
				return 1;
		}
	return 0;
#else
	return ((ARCH_WORD_32*)binary)[0] == crypt_key[0];
#endif
}

static int cmp_one(void *binary, int index)
{
#ifdef MMX_COEF
	int i = 0;
	for(i=0;i<(BINARY_SIZE/4);i++)
		// NOTE crypt_key is in input format (4*SHA_BUF_SIZ*MMX_COEF)
		if ( ((ARCH_WORD_32*)binary)[i] != ((ARCH_WORD_32*)crypt_key)[i*MMX_COEF+(index&3)+(index>>2)*SHA_BUF_SIZ*MMX_COEF] )
			return 0;
	return 1;
#else
	return !memcmp(binary, crypt_key, BINARY_SIZE);
#endif
}

static int cmp_exact(char *source, int count)
{
	return (1);
}

static int crypt_all(int *pcount, struct db_salt *salt)
{
	int count = *pcount;
#ifdef MMX_COEF
#ifdef SHA1_SSE_PARA
	SSESHA1body(ipad, (unsigned int*)dump, NULL, SSEi_MIXED_IN);
	SSESHA1body(cursalt, (unsigned int*)crypt_key, (unsigned int*)dump, SSEi_MIXED_IN|SSEi_RELOAD|SSEi_OUTPUT_AS_INP_FMT);

	SSESHA1body(opad, (unsigned int*)dump, NULL, SSEi_MIXED_IN);
	SSESHA1body(crypt_key, (unsigned int*)crypt_key, (unsigned int*)dump, SSEi_MIXED_IN|SSEi_RELOAD|SSEi_OUTPUT_AS_INP_FMT);
#else
	shammx_nosizeupdate_nofinalbyteswap(dump, ipad, 1);
	shammx_reloadinit_nosizeupdate_nofinalbyteswap(crypt_key, cursalt, dump);
	shammx_nosizeupdate_nofinalbyteswap(dump, opad, 1);
	shammx_reloadinit_nosizeupdate_nofinalbyteswap(crypt_key, crypt_key, dump);
#endif
#else
	SHA_CTX ctx;

	SHA1_Init( &ctx );
	SHA1_Update( &ctx, ipad, PAD_SIZE );
	SHA1_Update( &ctx, cursalt, strlen( (char*)cursalt) );
	SHA1_Final( (unsigned char*) crypt_key, &ctx);

	SHA1_Init( &ctx );
	SHA1_Update( &ctx, opad, PAD_SIZE );
	SHA1_Update( &ctx, crypt_key, BINARY_SIZE);
	SHA1_Final( (unsigned char*) crypt_key, &ctx);
#endif
	return count;
}

static void *binary(char *ciphertext)
{
	static union {
		unsigned char c[BINARY_SIZE];
		ARCH_WORD_32 dummy;
	} realcipher;
	int i,pos;

	for(i=strlen(ciphertext);ciphertext[i]!='#';i--); // allow # in salt
	pos=i+1;
	for(i=0;i<BINARY_SIZE;i++)
	{
		realcipher.c[i] = atoi16[ARCH_INDEX(ciphertext[i*2+pos])]*16 + atoi16[ARCH_INDEX(ciphertext[i*2+1+pos])];
	}
#ifdef MMX_COEF
	alter_endianity(realcipher.c, BINARY_SIZE);
#endif
	return (void*)realcipher.c;
}

static void *salt(char *ciphertext)
{
	static unsigned char salt[SALT_SIZE];
#ifdef MMX_COEF
	int i=0;
	int j;
	unsigned total_len=0;
#endif
	memset(salt, 0, sizeof(salt));
	// allow # in salt
	memcpy(salt, ciphertext, strrchr(ciphertext, '#') - ciphertext);
#ifdef MMX_COEF
	while(((unsigned char*)salt)[total_len])
	{
		for (i = 0; i < SHA1_N; ++i)
			cursalt[GETPOS(total_len,i)] = ((unsigned char*)salt)[total_len];
		++total_len;
	}
	for (i = 0; i < SHA1_N; ++i)
		cursalt[GETPOS(total_len, i)] = 0x80;
	for (j = total_len + 1; j < SALT_SIZE; ++j)
		for (i = 0; i < SHA1_N; ++i)
			cursalt[GETPOS(j, i)] = 0;
	for (i = 0; i < SHA1_N; ++i)
		((unsigned int*)cursalt)[15*MMX_COEF + (i&3) + (i>>2)*SHA_BUF_SIZ*MMX_COEF] = (total_len+64)<<3;
	return cursalt;
#else
	return salt;
#endif
}

static int binary_hash_0(void *binary) { return *(ARCH_WORD_32*)binary & 0xf; }
static int binary_hash_1(void *binary) { return *(ARCH_WORD_32*)binary & 0xff; }
static int binary_hash_2(void *binary) { return *(ARCH_WORD_32*)binary & 0xfff; }
static int binary_hash_3(void *binary) { return *(ARCH_WORD_32*)binary & 0xffff; }
static int binary_hash_4(void *binary) { return *(ARCH_WORD_32*)binary & 0xfffff; }
static int binary_hash_5(void *binary) { return *(ARCH_WORD_32*)binary & 0xffffff; }
static int binary_hash_6(void *binary) { return *(ARCH_WORD_32*)binary & 0x7ffffff; }

#ifdef MMX_COEF
// NOTE crypt_key is in input format (4*SHA_BUF_SIZ*MMX_COEF)
#define HASH_OFFSET (index&(MMX_COEF-1))+(index/MMX_COEF)*MMX_COEF*SHA_BUF_SIZ
static int get_hash_0(int index) { return ((ARCH_WORD_32*)crypt_key)[HASH_OFFSET] & 0xf; }
static int get_hash_1(int index) { return ((ARCH_WORD_32*)crypt_key)[HASH_OFFSET] & 0xff; }
static int get_hash_2(int index) { return ((ARCH_WORD_32*)crypt_key)[HASH_OFFSET] & 0xfff; }
static int get_hash_3(int index) { return ((ARCH_WORD_32*)crypt_key)[HASH_OFFSET] & 0xffff; }
static int get_hash_4(int index) { return ((ARCH_WORD_32*)crypt_key)[HASH_OFFSET] & 0xfffff; }
static int get_hash_5(int index) { return ((ARCH_WORD_32*)crypt_key)[HASH_OFFSET] & 0xffffff; }
static int get_hash_6(int index) { return ((ARCH_WORD_32*)crypt_key)[HASH_OFFSET] & 0x7ffffff; }
#else
static int get_hash_0(int index) { return crypt_key[0] & 0xf; }
static int get_hash_1(int index) { return crypt_key[0] & 0xff; }
static int get_hash_2(int index) { return crypt_key[0] & 0xfff; }
static int get_hash_3(int index) { return crypt_key[0] & 0xffff; }
static int get_hash_4(int index) { return crypt_key[0] & 0xfffff; }
static int get_hash_5(int index) { return crypt_key[0] & 0xffffff; }
static int get_hash_6(int index) { return crypt_key[0] & 0x7ffffff; }
#endif

struct fmt_main fmt_hmacSHA1 = {
	{
		FORMAT_LABEL,
		FORMAT_NAME,
		ALGORITHM_NAME,
		BENCHMARK_COMMENT,
		BENCHMARK_LENGTH,
		PLAINTEXT_LENGTH,
		BINARY_SIZE,
		BINARY_ALIGN,
#ifdef MMX_COEF
		SHA_BUF_SIZ*4*SHA1_N,
#else
		SALT_SIZE,
#endif
		SALT_ALIGN,
		MIN_KEYS_PER_CRYPT,
		MAX_KEYS_PER_CRYPT,
		FMT_CASE | FMT_8_BIT | FMT_SPLIT_UNIFIES_CASE,
		tests
	}, {
		init,
		fmt_default_done,
		fmt_default_reset,
		fmt_default_prepare,
		valid,
		split,
		binary,
		salt,
		fmt_default_source,
		{
			binary_hash_0,
			binary_hash_1,
			binary_hash_2,
			binary_hash_3,
			binary_hash_4,
			binary_hash_5,
			binary_hash_6
		},
		fmt_default_salt_hash,
		set_salt,
		set_key,
		get_key,
#ifdef MMX_COEF
		clear_keys,
#else
		fmt_default_clear_keys,
#endif
		crypt_all,
		{
			get_hash_0,
			get_hash_1,
			get_hash_2,
			get_hash_3,
			get_hash_4,
			get_hash_5,
			get_hash_6
		},
		cmp_all,
		cmp_one,
		cmp_exact
	}
};