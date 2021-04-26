/*
 * Copyright (C) 2021 Denys Vlasenko
 *
 * Licensed under GPLv2, see file LICENSE in this source tree.
 */
#include "tls.h"

#define SP_DEBUG          0
#define FIXED_SECRET      0
#define FIXED_PEER_PUBKEY 0

#if SP_DEBUG
# define dbg(...) fprintf(stderr, __VA_ARGS__)
static void dump_hex(const char *fmt, const void *vp, int len)
{
	char hexbuf[32 * 1024 + 4];
	const uint8_t *p = vp;

	bin2hex(hexbuf, (void*)p, len)[0] = '\0';
	dbg(fmt, hexbuf);
}
#else
# define dbg(...) ((void)0)
# define dump_hex(...) ((void)0)
#endif

#undef DIGIT_BIT
#define DIGIT_BIT  32
typedef int32_t sp_digit;

/* The code below is taken from parts of
 *  wolfssl-3.15.3/wolfcrypt/src/sp_c32.c
 * and heavily modified.
 * Header comment is kept intact:
 */

/* sp.c
 *
 * Copyright (C) 2006-2018 wolfSSL Inc.
 *
 * This file is part of wolfSSL.
 *
 * wolfSSL is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * wolfSSL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335, USA
 */

/* Implementation by Sean Parkinson. */

typedef struct sp_point {
	sp_digit x[2 * 10];
	sp_digit y[2 * 10];
	sp_digit z[2 * 10];
	int infinity;
} sp_point;

/* The modulus (prime) of the curve P256. */
static const sp_digit p256_mod[10] = {
	0x3ffffff,0x3ffffff,0x3ffffff,0x003ffff,0x0000000,
	0x0000000,0x0000000,0x0000400,0x3ff0000,0x03fffff,
};

#define p256_mp_mod ((sp_digit)0x000001)

/* The base point of curve P256. */
static const sp_point p256_base = {
	/* X ordinate */
	{ 0x098c296,0x04e5176,0x33a0f4a,0x204b7ac,0x277037d,0x0e9103c,0x3ce6e56,0x1091fe2,0x1f2e12c,0x01ac5f4 },
	/* Y ordinate */
	{ 0x3bf51f5,0x1901a0d,0x1ececbb,0x15dacc5,0x22bce33,0x303e785,0x27eb4a7,0x1fe6e3b,0x2e2fe1a,0x013f8d0 },
	/* Z ordinate */
	{ 0x0000001,0x0000000,0x0000000,0x0000000,0x0000000,0x0000000,0x0000000,0x0000000,0x0000000,0x0000000 },
	/* infinity */
	0
};

/* Write r as big endian to byte aray.
 * Fixed length number of bytes written: 32
 *
 * r  A single precision integer.
 * a  Byte array.
 */
static void sp_256_to_bin(sp_digit* r, uint8_t* a)
{
    int i, j, s = 0, b;

    for (i = 0; i < 9; i++) {
        r[i+1] += r[i] >> 26;
        r[i] &= 0x3ffffff;
    }
    j = 256 / 8 - 1;
    a[j] = 0;
    for (i=0; i<10 && j>=0; i++) {
        b = 0;
        a[j--] |= r[i] << s; b += 8 - s;
        if (j < 0)
            break;
        while (b < 26) {
            a[j--] = r[i] >> b; b += 8;
            if (j < 0)
                break;
        }
        s = 8 - (b - 26);
        if (j >= 0)
            a[j] = 0;
        if (s != 0)
            j++;
    }
}

/* Read big endian unsigned byte aray into r.
 *
 * r  A single precision integer.
 * a  Byte array.
 * n  Number of bytes in array to read.
 */
static void sp_256_from_bin(sp_digit* r, int max, const uint8_t* a, int n)
{
    int i, j = 0, s = 0;

    r[0] = 0;
    for (i = n-1; i >= 0; i--) {
        r[j] |= ((sp_digit)a[i]) << s;
        if (s >= 18) {
            r[j] &= 0x3ffffff;
            s = 26 - s;
            if (j + 1 >= max)
                break;
            r[++j] = a[i] >> s;
            s = 8 - s;
        }
        else
            s += 8;
    }

    for (j++; j < max; j++)
        r[j] = 0;
}

/* Convert a point of big-endian 32-byte x,y pair to type sp_point. */
static void sp_256_point_from_bin2x32(sp_point* p, const uint8_t *bin2x32)
{
    memset(p, 0, sizeof(*p));
    /*p->infinity = 0;*/
    sp_256_from_bin(p->x, 2 * 10, bin2x32, 32);
    sp_256_from_bin(p->y, 2 * 10, bin2x32 + 32, 32);
    //static const uint8_t one[1] = { 1 };
    //sp_256_from_bin(p->z, 2 * 10, one, 1);
    p->z[0] = 1;
}

/* Compare a with b in constant time.
 *
 * return -ve, 0 or +ve if a is less than, equal to or greater than b
 * respectively.
 */
static sp_digit sp_256_cmp_10(const sp_digit* a, const sp_digit* b)
{
    sp_digit r = 0;
    int i;
    for (i = 9; i >= 0; i--)
        r |= (a[i] - b[i]) & (0 - !r);
    return r;
}

/* Compare two numbers to determine if they are equal.
 *
 * return 1 when equal and 0 otherwise.
 */
static int sp_256_cmp_equal_10(const sp_digit* a, const sp_digit* b)
{
#if 1
    sp_digit r = 0;
    int i;
    for (i = 0; i < 10; i++)
        r |= (a[i] ^ b[i]);
    return r == 0;
#else
    return sp_256_cmp_10(a, b) == 0;
#endif
}

/* Normalize the values in each word to 26 bits. */
static void sp_256_norm_10(sp_digit* a)
{
    int i;
    for (i = 0; i < 9; i++) {
        a[i+1] += a[i] >> 26;
        a[i] &= 0x3ffffff;
    }
}

/* Add b to a into r. (r = a + b) */
static void sp_256_add_10(sp_digit* r, const sp_digit* a, const sp_digit* b)
{
    int i;
    for (i = 0; i < 10; i++)
        r[i] = a[i] + b[i];
}

/* Conditionally add a and b using the mask m.
 * m is -1 to add and 0 when not.
 */
static void sp_256_cond_add_10(sp_digit* r, const sp_digit* a,
        const sp_digit* b, const sp_digit m)
{
    int i;
    for (i = 0; i < 10; i++)
        r[i] = a[i] + (b[i] & m);
}

/* Conditionally subtract b from a using the mask m.
 * m is -1 to subtract and 0 when not.
 */
static void sp_256_cond_sub_10(sp_digit* r, const sp_digit* a,
        const sp_digit* b, const sp_digit m)
{
    int i;
    for (i = 0; i < 10; i++)
        r[i] = a[i] - (b[i] & m);
}

/* Shift number left one bit. Bottom bit is lost. */
static void sp_256_rshift1_10(sp_digit* r, sp_digit* a)
{
    int i;
    for (i = 0; i < 9; i++)
        r[i] = ((a[i] >> 1) | (a[i + 1] << 25)) & 0x3ffffff;
    r[9] = a[9] >> 1;
}

/* Multiply a number by Montogmery normalizer mod modulus (prime).
 *
 * r  The resulting Montgomery form number.
 * a  The number to convert.
 */
static void sp_256_mod_mul_norm_10(sp_digit* r, const sp_digit* a)
{
    int64_t t[8];
    int64_t a32[8];
    int64_t o;

    a32[0] = a[0];
    a32[0] |= a[1] << 26;
    a32[0] &= 0xffffffff;
    a32[1] = (sp_digit)(a[1] >> 6);
    a32[1] |= a[2] << 20;
    a32[1] &= 0xffffffff;
    a32[2] = (sp_digit)(a[2] >> 12);
    a32[2] |= a[3] << 14;
    a32[2] &= 0xffffffff;
    a32[3] = (sp_digit)(a[3] >> 18);
    a32[3] |= a[4] << 8;
    a32[3] &= 0xffffffff;
    a32[4] = (sp_digit)(a[4] >> 24);
    a32[4] |= a[5] << 2;
    a32[4] |= a[6] << 28;
    a32[4] &= 0xffffffff;
    a32[5] = (sp_digit)(a[6] >> 4);
    a32[5] |= a[7] << 22;
    a32[5] &= 0xffffffff;
    a32[6] = (sp_digit)(a[7] >> 10);
    a32[6] |= a[8] << 16;
    a32[6] &= 0xffffffff;
    a32[7] = (sp_digit)(a[8] >> 16);
    a32[7] |= a[9] << 10;
    a32[7] &= 0xffffffff;

    /*  1  1  0 -1 -1 -1 -1  0 */
    t[0] = 0 + a32[0] + a32[1] - a32[3] - a32[4] - a32[5] - a32[6];
    /*  0  1  1  0 -1 -1 -1 -1 */
    t[1] = 0 + a32[1] + a32[2] - a32[4] - a32[5] - a32[6] - a32[7];
    /*  0  0  1  1  0 -1 -1 -1 */
    t[2] = 0 + a32[2] + a32[3] - a32[5] - a32[6] - a32[7];
    /* -1 -1  0  2  2  1  0 -1 */
    t[3] = 0 - a32[0] - a32[1] + 2 * a32[3] + 2 * a32[4] + a32[5] - a32[7];
    /*  0 -1 -1  0  2  2  1  0 */
    t[4] = 0 - a32[1] - a32[2] + 2 * a32[4] + 2 * a32[5] + a32[6];
    /*  0  0 -1 -1  0  2  2  1 */
    t[5] = 0 - a32[2] - a32[3] + 2 * a32[5] + 2 * a32[6] + a32[7];
    /* -1 -1  0  0  0  1  3  2 */
    t[6] = 0 - a32[0] - a32[1] + a32[5] + 3 * a32[6] + 2 * a32[7];
    /*  1  0 -1 -1 -1 -1  0  3 */
    t[7] = 0 + a32[0] - a32[2] - a32[3] - a32[4] - a32[5] + 3 * a32[7];

    t[1] += t[0] >> 32; t[0] &= 0xffffffff;
    t[2] += t[1] >> 32; t[1] &= 0xffffffff;
    t[3] += t[2] >> 32; t[2] &= 0xffffffff;
    t[4] += t[3] >> 32; t[3] &= 0xffffffff;
    t[5] += t[4] >> 32; t[4] &= 0xffffffff;
    t[6] += t[5] >> 32; t[5] &= 0xffffffff;
    t[7] += t[6] >> 32; t[6] &= 0xffffffff;
    o     = t[7] >> 32; t[7] &= 0xffffffff;
    t[0] += o;
    t[3] -= o;
    t[6] -= o;
    t[7] += o;
    t[1] += t[0] >> 32; t[0] &= 0xffffffff;
    t[2] += t[1] >> 32; t[1] &= 0xffffffff;
    t[3] += t[2] >> 32; t[2] &= 0xffffffff;
    t[4] += t[3] >> 32; t[3] &= 0xffffffff;
    t[5] += t[4] >> 32; t[4] &= 0xffffffff;
    t[6] += t[5] >> 32; t[5] &= 0xffffffff;
    t[7] += t[6] >> 32; t[6] &= 0xffffffff;

    r[0] = (sp_digit)(t[0]) & 0x3ffffff;
    r[1] = (sp_digit)(t[0] >> 26);
    r[1] |= t[1] << 6;
    r[1] &= 0x3ffffff;
    r[2] = (sp_digit)(t[1] >> 20);
    r[2] |= t[2] << 12;
    r[2] &= 0x3ffffff;
    r[3] = (sp_digit)(t[2] >> 14);
    r[3] |= t[3] << 18;
    r[3] &= 0x3ffffff;
    r[4] = (sp_digit)(t[3] >> 8);
    r[4] |= t[4] << 24;
    r[4] &= 0x3ffffff;
    r[5] = (sp_digit)(t[4] >> 2) & 0x3ffffff;
    r[6] = (sp_digit)(t[4] >> 28);
    r[6] |= t[5] << 4;
    r[6] &= 0x3ffffff;
    r[7] = (sp_digit)(t[5] >> 22);
    r[7] |= t[6] << 10;
    r[7] &= 0x3ffffff;
    r[8] = (sp_digit)(t[6] >> 16);
    r[8] |= t[7] << 16;
    r[8] &= 0x3ffffff;
    r[9] = (sp_digit)(t[7] >> 10);
}

/* Mul a by scalar b and add into r. (r += a * b) */
static void sp_256_mul_add_10(sp_digit* r, const sp_digit* a, sp_digit b)
{
    int64_t tb = b;
    int64_t t = 0;
    int i;

    for (i = 0; i < 10; i++) {
        t += (tb * a[i]) + r[i];
        r[i] = t & 0x3ffffff;
        t >>= 26;
    }
    r[10] += t;
}

/* Divide the number by 2 mod the modulus (prime). (r = a / 2 % m) */
static void sp_256_div2_10(sp_digit* r, const sp_digit* a, const sp_digit* m)
{
    sp_256_cond_add_10(r, a, m, 0 - (a[0] & 1));
    sp_256_norm_10(r);
    sp_256_rshift1_10(r, r);
}

/* Shift the result in the high 256 bits down to the bottom. */
static void sp_256_mont_shift_10(sp_digit* r, const sp_digit* a)
{
    int i;
    sp_digit n, s;

    s = a[10];
    n = a[9] >> 22;
    for (i = 0; i < 9; i++) {
        n += (s & 0x3ffffff) << 4;
        r[i] = n & 0x3ffffff;
        n >>= 26;
        s = a[11 + i] + (s >> 26);
    }
    n += s << 4;
    r[9] = n;
    memset(&r[10], 0, sizeof(*r) * 10);
}

/* Add two Montgomery form numbers (r = a + b % m) */
static void sp_256_mont_add_10(sp_digit* r, const sp_digit* a, const sp_digit* b,
        const sp_digit* m)
{
    sp_256_add_10(r, a, b);
    sp_256_norm_10(r);
    sp_256_cond_sub_10(r, r, m, 0 - ((r[9] >> 22) > 0));
    sp_256_norm_10(r);
}

/* Double a Montgomery form number (r = a + a % m) */
static void sp_256_mont_dbl_10(sp_digit* r, const sp_digit* a, const sp_digit* m)
{
    sp_256_add_10(r, a, a);
    sp_256_norm_10(r);
    sp_256_cond_sub_10(r, r, m, 0 - ((r[9] >> 22) > 0));
    sp_256_norm_10(r);
}

/* Triple a Montgomery form number (r = a + a + a % m) */
static void sp_256_mont_tpl_10(sp_digit* r, const sp_digit* a, const sp_digit* m)
{
    sp_256_add_10(r, a, a);
    sp_256_norm_10(r);
    sp_256_cond_sub_10(r, r, m, 0 - ((r[9] >> 22) > 0));
    sp_256_norm_10(r);
    sp_256_add_10(r, r, a);
    sp_256_norm_10(r);
    sp_256_cond_sub_10(r, r, m, 0 - ((r[9] >> 22) > 0));
    sp_256_norm_10(r);
}

/* Sub b from a into r. (r = a - b) */
static void sp_256_sub_10(sp_digit* r, const sp_digit* a, const sp_digit* b)
{
    int i;
    for (i = 0; i < 10; i++)
        r[i] = a[i] - b[i];
}

/* Subtract two Montgomery form numbers (r = a - b % m) */
static void sp_256_mont_sub_10(sp_digit* r, const sp_digit* a, const sp_digit* b,
        const sp_digit* m)
{
    sp_256_sub_10(r, a, b);
    sp_256_cond_add_10(r, r, m, r[9] >> 22);
    sp_256_norm_10(r);
}

/* Reduce the number back to 256 bits using Montgomery reduction.
 *
 * a   A single precision number to reduce in place.
 * m   The single precision number representing the modulus.
 * mp  The digit representing the negative inverse of m mod 2^n.
 */
static void sp_256_mont_reduce_10(sp_digit* a, const sp_digit* m, sp_digit mp)
{
    int i;
    sp_digit mu;

    if (mp != 1) {
        for (i = 0; i < 9; i++) {
            mu = (a[i] * mp) & 0x3ffffff;
            sp_256_mul_add_10(a+i, m, mu);
            a[i+1] += a[i] >> 26;
        }
        mu = (a[i] * mp) & 0x3fffffl;
        sp_256_mul_add_10(a+i, m, mu);
        a[i+1] += a[i] >> 26;
        a[i] &= 0x3ffffff;
    }
    else {
        for (i = 0; i < 9; i++) {
            mu = a[i] & 0x3ffffff;
            sp_256_mul_add_10(a+i, p256_mod, mu);
            a[i+1] += a[i] >> 26;
        }
        mu = a[i] & 0x3fffffl;
        sp_256_mul_add_10(a+i, p256_mod, mu);
        a[i+1] += a[i] >> 26;
        a[i] &= 0x3ffffff;
    }

    sp_256_mont_shift_10(a, a);
    sp_256_cond_sub_10(a, a, m, 0 - ((a[9] >> 22) > 0));
    sp_256_norm_10(a);
}

/* Multiply a and b into r. (r = a * b) */
static void sp_256_mul_10(sp_digit* r, const sp_digit* a, const sp_digit* b)
{
    int i, j, k;
    int64_t c;

    c = ((int64_t)a[9]) * b[9];
    r[19] = (sp_digit)(c >> 26);
    c = (c & 0x3ffffff) << 26;
    for (k = 17; k >= 0; k--) {
        for (i = 9; i >= 0; i--) {
            j = k - i;
            if (j >= 10)
                break;
            if (j < 0)
                continue;
            c += ((int64_t)a[i]) * b[j];
        }
        r[k + 2] += c >> 52;
        r[k + 1] = (c >> 26) & 0x3ffffff;
        c = (c & 0x3ffffff) << 26;
    }
    r[0] = (sp_digit)(c >> 26);
}

/* Multiply two Montogmery form numbers mod the modulus (prime).
 * (r = a * b mod m)
 *
 * r   Result of multiplication.
 * a   First number to multiply in Montogmery form.
 * b   Second number to multiply in Montogmery form.
 * m   Modulus (prime).
 * mp  Montogmery mulitplier.
 */
static void sp_256_mont_mul_10(sp_digit* r, const sp_digit* a, const sp_digit* b,
        const sp_digit* m, sp_digit mp)
{
    sp_256_mul_10(r, a, b);
    sp_256_mont_reduce_10(r, m, mp);
}

/* Square a and put result in r. (r = a * a) */
static void sp_256_sqr_10(sp_digit* r, const sp_digit* a)
{
    int i, j, k;
    int64_t c;

    c = ((int64_t)a[9]) * a[9];
    r[19] = (sp_digit)(c >> 26);
    c = (c & 0x3ffffff) << 26;
    for (k = 17; k >= 0; k--) {
        for (i = 9; i >= 0; i--) {
            j = k - i;
            if (j >= 10 || i <= j)
                break;
            if (j < 0)
                continue;

            c += ((int64_t)a[i]) * a[j] * 2;
        }
        if (i == j)
           c += ((int64_t)a[i]) * a[i];

        r[k + 2] += c >> 52;
        r[k + 1] = (c >> 26) & 0x3ffffff;
        c = (c & 0x3ffffff) << 26;
    }
    r[0] = (sp_digit)(c >> 26);
}

/* Square the Montgomery form number. (r = a * a mod m)
 *
 * r   Result of squaring.
 * a   Number to square in Montogmery form.
 * m   Modulus (prime).
 * mp  Montogmery mulitplier.
 */
static void sp_256_mont_sqr_10(sp_digit* r, const sp_digit* a, const sp_digit* m,
        sp_digit mp)
{
    sp_256_sqr_10(r, a);
    sp_256_mont_reduce_10(r, m, mp);
}

/* Invert the number, in Montgomery form, modulo the modulus (prime) of the
 * P256 curve. (r = 1 / a mod m)
 *
 * r   Inverse result.
 * a   Number to invert.
 * td  Temporary data.
 */
/* Mod-2 for the P256 curve. */
static const uint32_t p256_mod_2[8] = {
	0xfffffffd,0xffffffff,0xffffffff,0x00000000,
	0x00000000,0x00000000,0x00000001,0xffffffff,
};
static void sp_256_mont_inv_10(sp_digit* r, sp_digit* a, sp_digit* td)
{
    sp_digit* t = td;
    int i;

    memcpy(t, a, sizeof(sp_digit) * 10);
    for (i = 254; i >= 0; i--) {
        sp_256_mont_sqr_10(t, t, p256_mod, p256_mp_mod);
        if (p256_mod_2[i / 32] & ((sp_digit)1 << (i % 32)))
            sp_256_mont_mul_10(t, t, a, p256_mod, p256_mp_mod);
    }
    memcpy(r, t, sizeof(sp_digit) * 10);
}

/* Map the Montgomery form projective co-ordinate point to an affine point.
 *
 * r  Resulting affine co-ordinate point.
 * p  Montgomery form projective co-ordinate point.
 * t  Temporary ordinate data.
 */
static void sp_256_map_10(sp_point* r, sp_point* p, sp_digit* t)
{
    sp_digit* t1 = t;
    sp_digit* t2 = t + 2*10;
    int32_t n;

    sp_256_mont_inv_10(t1, p->z, t + 2*10);

    sp_256_mont_sqr_10(t2, t1, p256_mod, p256_mp_mod);
    sp_256_mont_mul_10(t1, t2, t1, p256_mod, p256_mp_mod);

    /* x /= z^2 */
    sp_256_mont_mul_10(r->x, p->x, t2, p256_mod, p256_mp_mod);
    memset(r->x + 10, 0, sizeof(r->x) / 2);
    sp_256_mont_reduce_10(r->x, p256_mod, p256_mp_mod);
    /* Reduce x to less than modulus */
    n = sp_256_cmp_10(r->x, p256_mod);
    sp_256_cond_sub_10(r->x, r->x, p256_mod, 0 - (n >= 0));
    sp_256_norm_10(r->x);

    /* y /= z^3 */
    sp_256_mont_mul_10(r->y, p->y, t1, p256_mod, p256_mp_mod);
    memset(r->y + 10, 0, sizeof(r->y) / 2);
    sp_256_mont_reduce_10(r->y, p256_mod, p256_mp_mod);
    /* Reduce y to less than modulus */
    n = sp_256_cmp_10(r->y, p256_mod);
    sp_256_cond_sub_10(r->y, r->y, p256_mod, 0 - (n >= 0));
    sp_256_norm_10(r->y);

    memset(r->z, 0, sizeof(r->z));
    r->z[0] = 1;
}

/* Double the Montgomery form projective point p.
 *
 * r  Result of doubling point.
 * p  Point to double.
 * t  Temporary ordinate data.
 */
static void sp_256_proj_point_dbl_10(sp_point* r, sp_point* p, sp_digit* t)
{
    sp_point *rp[2];
    sp_point tp;
    sp_digit* t1 = t;
    sp_digit* t2 = t + 2*10;
    sp_digit* x;
    sp_digit* y;
    sp_digit* z;
    int i;

    /* When infinity don't double point passed in - constant time. */
    rp[0] = r;
    rp[1] = &tp;
    x = rp[p->infinity]->x;
    y = rp[p->infinity]->y;
    z = rp[p->infinity]->z;
    /* Put point to double into result - good for infinity. */
    if (r != p) {
        for (i = 0; i < 10; i++)
            r->x[i] = p->x[i];
        for (i = 0; i < 10; i++)
            r->y[i] = p->y[i];
        for (i = 0; i < 10; i++)
            r->z[i] = p->z[i];
        r->infinity = p->infinity;
    }

    /* T1 = Z * Z */
    sp_256_mont_sqr_10(t1, z, p256_mod, p256_mp_mod);
    /* Z = Y * Z */
    sp_256_mont_mul_10(z, y, z, p256_mod, p256_mp_mod);
    /* Z = 2Z */
    sp_256_mont_dbl_10(z, z, p256_mod);
    /* T2 = X - T1 */
    sp_256_mont_sub_10(t2, x, t1, p256_mod);
    /* T1 = X + T1 */
    sp_256_mont_add_10(t1, x, t1, p256_mod);
    /* T2 = T1 * T2 */
    sp_256_mont_mul_10(t2, t1, t2, p256_mod, p256_mp_mod);
    /* T1 = 3T2 */
    sp_256_mont_tpl_10(t1, t2, p256_mod);
    /* Y = 2Y */
    sp_256_mont_dbl_10(y, y, p256_mod);
    /* Y = Y * Y */
    sp_256_mont_sqr_10(y, y, p256_mod, p256_mp_mod);
    /* T2 = Y * Y */
    sp_256_mont_sqr_10(t2, y, p256_mod, p256_mp_mod);
    /* T2 = T2/2 */
    sp_256_div2_10(t2, t2, p256_mod);
    /* Y = Y * X */
    sp_256_mont_mul_10(y, y, x, p256_mod, p256_mp_mod);
    /* X = T1 * T1 */
    sp_256_mont_mul_10(x, t1, t1, p256_mod, p256_mp_mod);
    /* X = X - Y */
    sp_256_mont_sub_10(x, x, y, p256_mod);
    /* X = X - Y */
    sp_256_mont_sub_10(x, x, y, p256_mod);
    /* Y = Y - X */
    sp_256_mont_sub_10(y, y, x, p256_mod);
    /* Y = Y * T1 */
    sp_256_mont_mul_10(y, y, t1, p256_mod, p256_mp_mod);
    /* Y = Y - T2 */
    sp_256_mont_sub_10(y, y, t2, p256_mod);
}

/* Add two Montgomery form projective points.
 *
 * r  Result of addition.
 * p  Frist point to add.
 * q  Second point to add.
 * t  Temporary ordinate data.
 */
static void sp_256_proj_point_add_10(sp_point* r, sp_point* p, sp_point* q,
        sp_digit* t)
{
    sp_point *ap[2];
    sp_point *rp[2];
    sp_point tp;
    sp_digit* t1 = t;
    sp_digit* t2 = t + 2*10;
    sp_digit* t3 = t + 4*10;
    sp_digit* t4 = t + 6*10;
    sp_digit* t5 = t + 8*10;
    sp_digit* x;
    sp_digit* y;
    sp_digit* z;
    int i;

    /* Ensure only the first point is the same as the result. */
    if (q == r) {
        sp_point* a = p;
        p = q;
        q = a;
    }

    /* Check double */
    sp_256_sub_10(t1, p256_mod, q->y);
    sp_256_norm_10(t1);
    if (sp_256_cmp_equal_10(p->x, q->x)
     & sp_256_cmp_equal_10(p->z, q->z)
     & (sp_256_cmp_equal_10(p->y, q->y) | sp_256_cmp_equal_10(p->y, t1))
    ) {
        sp_256_proj_point_dbl_10(r, p, t);
    }
    else {
        rp[0] = r;
        rp[1] = &tp;
        memset(&tp, 0, sizeof(tp));
        x = rp[p->infinity | q->infinity]->x;
        y = rp[p->infinity | q->infinity]->y;
        z = rp[p->infinity | q->infinity]->z;

        ap[0] = p;
        ap[1] = q;
        for (i=0; i<10; i++)
            r->x[i] = ap[p->infinity]->x[i];
        for (i=0; i<10; i++)
            r->y[i] = ap[p->infinity]->y[i];
        for (i=0; i<10; i++)
            r->z[i] = ap[p->infinity]->z[i];
        r->infinity = ap[p->infinity]->infinity;

        /* U1 = X1*Z2^2 */
        sp_256_mont_sqr_10(t1, q->z, p256_mod, p256_mp_mod);
        sp_256_mont_mul_10(t3, t1, q->z, p256_mod, p256_mp_mod);
        sp_256_mont_mul_10(t1, t1, x, p256_mod, p256_mp_mod);
        /* U2 = X2*Z1^2 */
        sp_256_mont_sqr_10(t2, z, p256_mod, p256_mp_mod);
        sp_256_mont_mul_10(t4, t2, z, p256_mod, p256_mp_mod);
        sp_256_mont_mul_10(t2, t2, q->x, p256_mod, p256_mp_mod);
        /* S1 = Y1*Z2^3 */
        sp_256_mont_mul_10(t3, t3, y, p256_mod, p256_mp_mod);
        /* S2 = Y2*Z1^3 */
        sp_256_mont_mul_10(t4, t4, q->y, p256_mod, p256_mp_mod);
        /* H = U2 - U1 */
        sp_256_mont_sub_10(t2, t2, t1, p256_mod);
        /* R = S2 - S1 */
        sp_256_mont_sub_10(t4, t4, t3, p256_mod);
        /* Z3 = H*Z1*Z2 */
        sp_256_mont_mul_10(z, z, q->z, p256_mod, p256_mp_mod);
        sp_256_mont_mul_10(z, z, t2, p256_mod, p256_mp_mod);
        /* X3 = R^2 - H^3 - 2*U1*H^2 */
        sp_256_mont_sqr_10(x, t4, p256_mod, p256_mp_mod);
        sp_256_mont_sqr_10(t5, t2, p256_mod, p256_mp_mod);
        sp_256_mont_mul_10(y, t1, t5, p256_mod, p256_mp_mod);
        sp_256_mont_mul_10(t5, t5, t2, p256_mod, p256_mp_mod);
        sp_256_mont_sub_10(x, x, t5, p256_mod);
        sp_256_mont_dbl_10(t1, y, p256_mod);
        sp_256_mont_sub_10(x, x, t1, p256_mod);
        /* Y3 = R*(U1*H^2 - X3) - S1*H^3 */
        sp_256_mont_sub_10(y, y, x, p256_mod);
        sp_256_mont_mul_10(y, y, t4, p256_mod, p256_mp_mod);
        sp_256_mont_mul_10(t5, t5, t3, p256_mod, p256_mp_mod);
        sp_256_mont_sub_10(y, y, t5, p256_mod);
    }
}

/* Multiply the point by the scalar and return the result.
 * If map is true then convert result to affine co-ordinates.
 *
 * r     Resulting point.
 * g     Point to multiply.
 * k     Scalar to multiply by.
 */
static void sp_256_ecc_mulmod_10(sp_point* r, const sp_point* g, const sp_digit* k /*, int map*/)
{
    enum { map = 1 }; /* we always convert result to affine coordinates */
    sp_point td[3];
    sp_point* t[3];
    sp_digit tmp[2 * 10 * 5];
    sp_digit n;
    int i;
    int c, y;

    memset(td, 0, sizeof(td));

    t[0] = &td[0];
    t[1] = &td[1];
    t[2] = &td[2];

    /* t[0] = {0, 0, 1} * norm */
    t[0]->infinity = 1;
    /* t[1] = {g->x, g->y, g->z} * norm */
    sp_256_mod_mul_norm_10(t[1]->x, g->x);
    sp_256_mod_mul_norm_10(t[1]->y, g->y);
    sp_256_mod_mul_norm_10(t[1]->z, g->z);

    i = 9;
    c = 22;
    n = k[i--] << (26 - c);
    for (; ; c--) {
        if (c == 0) {
            if (i == -1)
                break;

            n = k[i--];
            c = 26;
        }

        y = (n >> 25) & 1;
        n <<= 1;

        sp_256_proj_point_add_10(t[y^1], t[0], t[1], tmp);
        memcpy(t[2], t[y], sizeof(sp_point));
        sp_256_proj_point_dbl_10(t[2], t[2], tmp);
        memcpy(t[y], t[2], sizeof(sp_point));
    }

    if (map)
        sp_256_map_10(r, t[0], tmp);
    else
        memcpy(r, t[0], sizeof(sp_point));

    memset(tmp, 0, sizeof(tmp)); //paranoia
    memset(td, 0, sizeof(td)); //paranoia
}

/* Multiply the base point of P256 by the scalar and return the result.
 * If map is true then convert result to affine co-ordinates.
 *
 * r     Resulting point.
 * k     Scalar to multiply by.
 */
static void sp_256_ecc_mulmod_base_10(sp_point* r, sp_digit* k /*, int map*/)
{
	sp_256_ecc_mulmod_10(r, &p256_base, k /*, map*/);
}

/* Multiply the point by the scalar and serialize the X ordinate.
 * The number is 0 padded to maximum size on output.
 *
 * priv    Scalar to multiply the point by.
 * pub2x32 Point to multiply.
 * out32   Buffer to hold X ordinate.
 */
static void sp_ecc_secret_gen_256(sp_digit priv[10], const uint8_t *pub2x32, uint8_t* out32)
{
    sp_point point[1];

#if FIXED_PEER_PUBKEY
    memset((void*)pub2x32, 0x55, 64);
#endif
    dump_hex("peerkey %s\n", pub2x32, 32); /* in TLS, this is peer's public key */
    dump_hex("        %s\n", pub2x32 + 32, 32);

    sp_256_point_from_bin2x32(point, pub2x32);
    dump_hex("point->x %s\n", point->x, sizeof(point->x));
    dump_hex("point->y %s\n", point->y, sizeof(point->y));

    sp_256_ecc_mulmod_10(point, point, priv);

    sp_256_to_bin(point->x, out32);
    dump_hex("out32: %s\n", out32, 32);
}

/* Generates a scalar that is in the range 1..order-1. */
#define SIMPLIFY 1
/* Add 1 to a. (a = a + 1) */
#if !SIMPLIFY
static void sp_256_add_one_10(sp_digit* a)
{
    a[0]++;
    sp_256_norm_10(a);
}
#endif
static void sp_256_ecc_gen_k_10(sp_digit k[10])
{
#if !SIMPLIFY
	/* The order of the curve P256 minus 2. */
	static const sp_digit p256_order2[10] = {
		0x063254f,0x272b0bf,0x1e84f3b,0x2b69c5e,0x3bce6fa,
		0x3ffffff,0x3ffffff,0x00003ff,0x3ff0000,0x03fffff,
	};
#endif
	uint8_t buf[32];

	for (;;) {
		tls_get_random(buf, sizeof(buf));
#if FIXED_SECRET
		memset(buf, 0x77, sizeof(buf));
#endif
		sp_256_from_bin(k, 10, buf, sizeof(buf));
#if !SIMPLIFY
		if (sp_256_cmp_10(k, p256_order2) < 0)
			break;
#else
		/* non-loopy version (and not needing p256_order2[]):
		 * if most-significant word seems that k can be larger
		 * than p256_order2, fix it up:
		 */
		if (k[9] >= 0x03fffff)
			k[9] = 0x03ffffe;
		break;
#endif
	}
#if !SIMPLIFY
	sp_256_add_one_10(k);
#else
	if (k[0] == 0)
		k[0] = 1;
#endif
#undef SIMPLIFY
}

/* Makes a random EC key pair. */
static void sp_ecc_make_key_256(sp_digit privkey[10], uint8_t *pubkey)
{
	sp_point point[1];

	sp_256_ecc_gen_k_10(privkey);
	sp_256_ecc_mulmod_base_10(point, privkey);
	sp_256_to_bin(point->x, pubkey);
	sp_256_to_bin(point->y, pubkey + 32);

	memset(point, 0, sizeof(point)); //paranoia
}

void FAST_FUNC curve_P256_compute_pubkey_and_premaster(
		uint8_t *pubkey2x32, uint8_t *premaster32,
		const uint8_t *peerkey2x32)
{
	sp_digit privkey[10];

	sp_ecc_make_key_256(privkey, pubkey2x32);
	dump_hex("pubkey: %s\n", pubkey2x32, 32);
	dump_hex("        %s\n", pubkey2x32 + 32, 32);

	/* Combine our privkey and peer's public key to generate premaster */
	sp_ecc_secret_gen_256(privkey, /*x,y:*/peerkey2x32, premaster32);
	dump_hex("premaster: %s\n", premaster32, 32);
}