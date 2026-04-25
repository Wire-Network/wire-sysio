/* ===-- fixsfti.c - Implement __fixsfti -----------------------------------===
 *
 *                     The LLVM Compiler Infrastructure
 *
 * This file is dual licensed under the MIT and the University of Illinois Open
 * Source Licenses. See LICENSE.TXT for details.
 *
 * ===----------------------------------------------------------------------===
 */

#include "fp32.h"

typedef __int128 fixint_t;
typedef unsigned __int128 fixuint_t;

fixint_t ___fixsfti(uint32_t a) {
    const fixint_t fixint_max = (fixint_t)((~(fixuint_t)0) / 2);
    const fixint_t fixint_min = -fixint_max - 1;
    /* Unsigned magnitude of INT128_MIN (i.e. 2^127). Negative saturation
     * boundary -- only -2^127 is representable as a negative __int128. */
    const fixuint_t fixint_min_absval =
        (fixuint_t)1 << (sizeof(fixint_t) * CHAR_BIT - 1);

    // Break a into sign, exponent, significand
    const rep_t aRep = a;
    const rep_t aAbs = aRep & absMask;
    const int sign_negative = (aRep & signBit) ? 1 : 0;
    const int exponent = (aAbs >> significandBits) - exponentBias;
    const rep_t significand = (aAbs & significandMask) | implicitBit;

    // If exponent is negative, the result is zero.
    if (exponent < 0)
        return 0;

    // If the value is too large for the integer type, saturate.
    if ((unsigned)exponent >= sizeof(fixint_t) * CHAR_BIT)
        return sign_negative ? fixint_min : fixint_max;

    // If 0 <= exponent < significandBits, right shift to get the result.
    // significand fits in rep_t (<= 2^24); the shift result fits in fixint_t.
    if (exponent < significandBits) {
        const fixint_t mag = (fixint_t)(significand >> (significandBits - exponent));
        return sign_negative ? -mag : mag;
    }

    // Otherwise shift left in unsigned __int128 to avoid signed-overflow UB
    // when |a| == 2^127 (mag == 2^127 overflows signed __int128 for positive
    // inputs and equals INT128_MIN for negative inputs). Saturate explicitly.
    const fixuint_t mag = (fixuint_t)significand << (exponent - significandBits);
    if (sign_negative)
        return mag >= fixint_min_absval ? fixint_min : -(fixint_t)mag;
    return mag > (fixuint_t)fixint_max ? fixint_max : (fixint_t)mag;
}
