// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2017-2019 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Utilities for converting data from/to strings.
 */
#ifndef BITCOIN_UTILSTRENCODINGS_H
#define BITCOIN_UTILSTRENCODINGS_H

#include "support/allocators/secure.h"
#include "span.h"

#include <algorithm>
#include <iterator>
#include <stdint.h>
#include <string>
#include <vector>

#define BEGIN(a) ((char*)&(a))
#define END(a) ((char*)&((&(a))[1]))
#define UBEGIN(a) ((unsigned char*)&(a))
#define UEND(a) ((unsigned char*)&((&(a))[1]))
#define ARRAYLEN(array) (sizeof(array) / sizeof((array)[0]))

/** Used by SanitizeString() */
enum SafeChars
{
    SAFE_CHARS_DEFAULT, //!< The full set of allowed chars
    SAFE_CHARS_UA_COMMENT, //!< BIP-0014 subset
    SAFE_CHARS_FILENAME, //!< Chars allowed in filenames
};

/**
* Remove unsafe chars. Safe chars chosen to allow simple messages/URLs/email
* addresses, but avoid anything even possibly remotely dangerous like & or >
* @param[in] str    The string to sanitize
* @param[in] rule   The set of safe chars to choose (default: least restrictive)
* @return           A new string without unsafe chars
*/
std::string SanitizeString(const std::string& str, int rule = SAFE_CHARS_DEFAULT);

/**
* Check URL format for conformance for validity to a defined pattern
* @param[in] strURL   The string to be processed for validity
* @param[in] stdErr   A string that will be loaded with any validation error message
* @param[in] maxSize  An unsigned int, defaulted to 64, to restrict the length
* @return             A bool, true if valid, false if not (reason in stdErr)
*/
bool validateURL(std::string strURL, std::string& strErr, unsigned int maxSize = 64);

std::vector<unsigned char> ParseHex(const char* psz);
std::vector<unsigned char> ParseHex(const std::string& str);
signed char HexDigit(char c);
bool IsHex(const std::string& str);
std::vector<unsigned char> DecodeBase64(const char* p, bool* pfInvalid = NULL);
std::string DecodeBase64(const std::string& str);
std::string EncodeBase64(Span<const unsigned char> input);
std::string EncodeBase64(const std::string& str);
std::vector<unsigned char> DecodeBase32(const char* p, bool* pfInvalid = NULL);
std::string DecodeBase32(const std::string& str);
std::string EncodeBase32(const unsigned char* pch, size_t len);
std::string EncodeBase32(const std::string& str);

std::string i64tostr(int64_t n);
std::string itostr(int n);
int64_t atoi64(const char* psz);
int64_t atoi64(const std::string& str);
int atoi(const std::string& str);

/**
 * Tests if the given character is a decimal digit.
 * @param[in] c     character to test
 * @return          true if the argument is a decimal digit; otherwise false.
 */
constexpr bool IsDigit(char c)
{
    return c >= '0' && c <= '9';
}

/**
 * Convert string to signed 32-bit integer with strict parse error feedback.
 * @returns true if the entire string could be parsed as valid integer,
 *   false if not the entire string could be parsed or when overflow or underflow occurred.
 */
bool ParseInt32(const std::string& str, int32_t *out);

/**
 * Convert string to signed 64-bit integer with strict parse error feedback.
 * @returns true if the entire string could be parsed as valid integer,
 *   false if not the entire string could be parsed or when overflow or underflow occurred.
 */
bool ParseInt64(const std::string& str, int64_t *out);

/**
 * Convert string to double with strict parse error feedback.
 * @returns true if the entire string could be parsed as valid double,
 *   false if not the entire string could be parsed or when overflow or underflow occurred.
 */
bool ParseDouble(const std::string& str, double *out);

/* Return iterator to first non zero element, or itend */
template <typename T>
T FindFirstNonZero(T itbegin, T itend)
{
    return std::find_if(itbegin, itend, [](unsigned char v) { return v != 0; });
}

/**
 * Convert a span of bytes to a lower-case hexadecimal string.
 */
std::string HexStr(const Span<const uint8_t> s);
inline std::string HexStr(const Span<const char> s) { return HexStr(MakeUCharSpan(s)); }

/** Reverse the endianess of a string */
inline std::string ReverseEndianString(std::string in)
{
    std::string out = "";
    unsigned int s = in.size();
    for (unsigned int i = 0; i < s; i += 2) {
        out += in.substr(s - i - 2, 2);
    }

    return out;
}

/**
 * Format a paragraph of text to a fixed width, adding spaces for
 * indentation to any added line.
 */
std::string FormatParagraph(const std::string in, size_t width = 79, size_t indent = 0);

/**
 * Timing-attack-resistant comparison.
 * Takes time proportional to length
 * of first argument.
 */
template <typename T>
bool TimingResistantEqual(const T& a, const T& b)
{
    if (b.size() == 0) return a.size() == 0;
    size_t accumulator = a.size() ^ b.size();
    for (size_t i = 0; i < a.size(); i++)
        accumulator |= a[i] ^ b[i % b.size()];
    return accumulator == 0;
}

/**
  * Convert from one power-of-2 number base to another.
  *
  * Examples using ConvertBits<8, 5, true>():
  * 000000 -> 0000000000
  * 202020 -> 0400100200
  * 757575 -> 0e151a170a
  * abcdef -> 150f061e1e
  * ffffff -> 1f1f1f1f1e
  */
template<int frombits, int tobits, bool pad, typename O, typename I>
bool ConvertBits(const O& outfn, I it, I end) {
    size_t acc = 0;
    size_t bits = 0;
    constexpr size_t maxv = (1 << tobits) - 1;
    constexpr size_t max_acc = (1 << (frombits + tobits - 1)) - 1;
    while (it != end) {
        acc = ((acc << frombits) | *it) & max_acc;
        bits += frombits;
        while (bits >= tobits) {
            bits -= tobits;
            outfn((acc >> bits) & maxv);
        }
        ++it;
    }
    if (pad) {
        if (bits) outfn((acc << (tobits - bits)) & maxv);
    } else if (bits >= frombits || ((acc << (tobits - bits)) & maxv)) {
        return false;
    }
    return true;
}


/** Parse number as fixed point according to JSON number syntax.
  * See http://json.org/number.gif
  * @returns true on success, false on error.
  * @note The result must be in the range (-10^18,10^18), otherwise an overflow error will trigger.
  */
bool ParseFixedPoint(const std::string &val, int decimals, int64_t *amount_out);

/**
 * Converts the given character to its lowercase equivalent.
 * This function is locale independent. It only converts uppercase
 * characters in the standard 7-bit ASCII range.
 * @param[in] c     the character to convert to lowercase.
 * @return          the lowercase equivalent of c; or the argument
 *                  if no conversion is possible.
 */
constexpr unsigned char ToLower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z' ? (c - 'A') + 'a' : c);
}

/**
 * Returns the lowercase equivalent of the given string.
 * This function is locale independent. It only converts uppercase
 * characters in the standard 7-bit ASCII range.
 * This is a feature, not a limitation.
 *
 * @param[in] str   the string to convert to lowercase.
 * @returns         lowercased equivalent of str
 */
std::string ToLower(const std::string& str);

/**
 * Converts the given string to its lowercase equivalent.
 * This function is locale independent. It only converts uppercase
 * characters in the standard 7-bit ASCII range.
 * @param[in,out] str   the string to convert to lowercase.
 */
void Downcase(std::string& str);

/**
 * Converts the given character to its uppercase equivalent.
 * This function is locale independent. It only converts lowercase
 * characters in the standard 7-bit ASCII range.
 * @param[in] c     the character to convert to uppercase.
 * @return          the uppercase equivalent of c; or the argument
 *                  if no conversion is possible.
 */
constexpr unsigned char ToUpper(unsigned char c)
{
    return (c >= 'a' && c <= 'z' ? (c - 'a') + 'A' : c);
}

/**
 * Returns the uppercase equivalent of the given string.
 * This function is locale independent. It only converts lowercase
 * characters in the standard 7-bit ASCII range.
 * This is a feature, not a limitation.
 *
 * @param[in] str   the string to convert to uppercase.
 * @returns         UPPERCASED EQUIVALENT OF str
 */
std::string ToUpper(const std::string& str);

/**
 * Capitalizes the first character of the given string.
 * This function is locale independent. It only capitalizes the
 * first character of the argument if it has an uppercase equivalent
 * in the standard 7-bit ASCII range.
 * @param[in] str   the string to capitalize.
 * @return          string with the first letter capitalized.
 */
std::string Capitalize(std::string str);

/**
 * Checks for valid 4-byte UTF-8 encoding in a string
 * @param[in] str   the string to check.
 * @return          boolean. true for valid UTF-8 encoding.
 */
bool IsValidUTF8(const std::string& str);

#endif // BITCOIN_UTILSTRENCODINGS_H
