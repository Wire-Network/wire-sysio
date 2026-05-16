/* ===-- fixtfti.c - Implement __fixtfti -----------------------------------===
 *
 *                     The LLVM Compiler Infrastructure
 *
 * This file is dual licensed under the MIT and the University of Illinois Open
 * Source Licenses. See LICENSE.TXT for details.
 *
 * ===----------------------------------------------------------------------===
 */

#include "fp128.h"

__int128 ___fixtfti( float128_t a) {
    const __int128 fixint_max = (__int128)((~(unsigned __int128)0) / 2);
    const __int128 fixint_min = -fixint_max - 1;
    /* Unsigned magnitude of INT128_MIN (i.e. 2^127). A negative result is
     * representable iff its magnitude is <= this value (exactly INT128_MIN). */
    const unsigned __int128 fixint_min_absval =
        (unsigned __int128)1 << (sizeof(__int128) * CHAR_BIT - 1);

    // Break a into sign, exponent, significand
    const __int128 aRep = toRep(a);
    const __int128 aAbs = aRep & absMask;
    const int sign_negative = (aRep & signBit) ? 1 : 0;
    const int exponent = (aAbs >> significandBits) - exponentBias;
    const __int128 significand = (aAbs & significandMask) | implicitBit;

    // If exponent is negative, the result is zero.
    if (exponent < 0)
        return 0;

    // If the value is too large for the integer type, saturate.
    if ((unsigned)exponent >= sizeof(__int128) * CHAR_BIT)
        return sign_negative ? fixint_min : fixint_max;

    // If 0 <= exponent < significandBits, right shift to get the result.
    // significand is non-negative here (>= implicitBit, < 2 * implicitBit), so
    // a signed right shift is defined and the product fits in __int128.
    if (exponent < significandBits) {
        const __int128 mag = significand >> (significandBits - exponent);
        return sign_negative ? -mag : mag;
    }

    // Otherwise shift left. Perform in unsigned to avoid signed-overflow UB
    // when the result approaches 2^127 (e.g. |a| == 2^127 yields mag == 2^127,
    // which overflows __int128 on the positive side and equals INT128_MIN on
    // the negative side). Saturate explicitly for both signs.
    const unsigned __int128 mag =
        (unsigned __int128)significand << (exponent - significandBits);
    if (sign_negative)
        return mag >= fixint_min_absval ? fixint_min : -(__int128)mag;
    return mag > (unsigned __int128)fixint_max ? fixint_max : (__int128)mag;
}
