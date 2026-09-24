#pragma once
#include <string>
#include <algorithm>
#include <iterator>

#include <sysio/asset.hpp>

inline string_view trim(string_view sv) {
    sv.remove_prefix(std::min(sv.find_first_not_of(" "), sv.size())); // left trim
    sv.remove_suffix(std::min(sv.size()-sv.find_last_not_of(" ")-1, sv.size())); // right trim
    return sv;
}

inline vector<string_view> split(string_view str, string_view delims = " ")
{
    vector<string_view> res;
    std::size_t current, previous = 0;
    current = str.find_first_of(delims);
    while (current != std::string::npos) {
        res.push_back(trim(str.substr(previous, current - previous)));
        previous = current + 1;
        current = str.find_first_of(delims, previous);
    }
    res.push_back(trim(str.substr(previous, current - previous)));
    return res;
}

inline bool starts_with(string_view sv, string_view s) {
    return sv.size() >= s.size() && sv.compare(0, s.size(), s) == 0;
}

/// Largest decimal precision an amount string may carry: 10^18 is the last
/// power of ten inside int64.
inline constexpr uint8_t MAX_AMOUNT_PRECISION = 18;

/// Parse an unsigned decimal digit string into int64, aborting on any character
/// that is not a digit and on overflow. Signs are not accepted: every amount
/// read from a memo is a magnitude. An empty string is zero.
inline int64_t to_int(string_view sv) {
    int64_t res = 0;
    for (const char c : sv) {
        check( c >= '0' && c <= '9', "invalid character" );
        check( !__builtin_mul_overflow(res, int64_t(10), &res)
            && !__builtin_add_overflow(res, int64_t(c - '0'), &res), "amount too large" );
    }
    return res;
}

/// 10^decimals, the scale of an amount with that many decimal places.
inline int64_t precision_from_decimals(uint8_t decimals)
{
    check(decimals <= MAX_AMOUNT_PRECISION, "precision should be <= 18");
    int64_t p10 = 1;
    for (uint8_t i = 0; i < decimals; ++i) p10 *= 10;
    return p10;
}

/// Parse "<amount> <SYMBOL>" as it appears in an exchange memo, e.g.
/// "16.6570 VOICE": the symbol's precision is the number of decimals written.
/// Every arithmetic step is overflow-checked, so an amount string too large for
/// int64 aborts with "amount too large" rather than wrapping.
inline asset asset_from_string(string_view from)
{
    string_view s = trim(from);

    // Find space in order to split amount and symbol
    auto space_pos = s.find(' ');
    check(space_pos != string::npos, "Asset's amount and symbol should be separated with space");
    auto symbol_str = trim(s.substr(space_pos + 1));
    auto amount_str = s.substr(0, space_pos);

    // Ensure that if decimal point is used (.), decimal fraction is specified
    auto dot_pos = amount_str.find('.');
    if (dot_pos != string::npos) {
        check(dot_pos != amount_str.size() - 1, "Missing decimal fraction after decimal point");
    }

    // Parse symbol
    uint8_t precision_digit = 0;
    if (dot_pos != string::npos) {
        precision_digit = amount_str.size() - dot_pos - 1;
    }

    symbol sym = symbol(symbol_str, precision_digit);

    // Parse amount
    int64_t int_part = 0;
    int64_t fract_part = 0;
    if (dot_pos != string::npos) {
        int_part   = to_int(amount_str.substr(0, dot_pos));
        fract_part = to_int(amount_str.substr(dot_pos + 1));
    } else {
        int_part = to_int(amount_str);
    }

    int64_t amount = 0;
    check( !__builtin_mul_overflow(int_part, precision_from_decimals(sym.precision()), &amount)
        && !__builtin_add_overflow(amount, fract_part, &amount), "amount too large" );

    return asset(amount, sym);
}
