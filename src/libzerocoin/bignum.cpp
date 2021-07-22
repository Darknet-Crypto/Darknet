// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2017-2019 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "bignum.h"

/** C++ wrapper for BIGNUM (Gmp bignum) */
CBigNum::CBigNum()
{
    mpz_init(bn);
}

CBigNum::CBigNum(const CBigNum& b)
{
    mpz_init(bn);
    mpz_set(bn, b.bn);
}

CBigNum& CBigNum::operator=(const CBigNum& b)
{
    mpz_set(bn, b.bn);
    return (*this);
}

CBigNum::~CBigNum()
{
    mpz_clear(bn);
}

//CBigNum(char n) is not portable.  Use 'signed char' or 'unsigned char'.
CBigNum::CBigNum(signed char n)      { mpz_init(bn); if (n >= 0) mpz_set_ui(bn, n); else mpz_set_si(bn, n); }
CBigNum::CBigNum(short n)            { mpz_init(bn); if (n >= 0) mpz_set_ui(bn, n); else mpz_set_si(bn, n); }
CBigNum::CBigNum(int n)              { mpz_init(bn); if (n >= 0) mpz_set_ui(bn, n); else mpz_set_si(bn, n); }
CBigNum::CBigNum(long n)             { mpz_init(bn); if (n >= 0) mpz_set_ui(bn, n); else mpz_set_si(bn, n); }
CBigNum::CBigNum(long long n)        { mpz_init(bn); mpz_set_si(bn, n); }
CBigNum::CBigNum(unsigned char n)    { mpz_init(bn); mpz_set_ui(bn, n); }
CBigNum::CBigNum(unsigned short n)   { mpz_init(bn); mpz_set_ui(bn, n); }
CBigNum::CBigNum(unsigned int n)     { mpz_init(bn); mpz_set_ui(bn, n); }
CBigNum::CBigNum(unsigned long n)    { mpz_init(bn); mpz_set_ui(bn, n); }

CBigNum::CBigNum(uint256 n) { mpz_init(bn); setuint256(n); }

CBigNum::CBigNum(const std::vector<unsigned char>& vch)
{
    mpz_init(bn);
    setvch(vch);
}

/** PRNGs use GMP for consistency with seed initialization **/

/** Generates a cryptographically secure random number between zero and range-1 (inclusive)
* i.e. 0 <= returned number < range
* @param range The upper bound on the number.
* @return
*/
CBigNum CBigNum::randBignum(const CBigNum& range)
{
    if (range < 2)
        return 0;

    size_t size = (mpz_sizeinbase (range.bn, 2) + CHAR_BIT-1) / CHAR_BIT;
    std::vector<unsigned char> buf(size);

    GetStrongRandBytes(buf.data(), size);

    CBigNum ret(buf);
    if (ret < 0)
        mpz_neg(ret.bn, ret.bn);
    return (ret % range);
}

/**Returns the size in bits of the underlying bignum.
 *
 * @return the size
 */
int CBigNum::bitSize() const
{
    return  mpz_sizeinbase(bn, 2);
}

void CBigNum::setulong(unsigned long n)
{
    mpz_set_ui(bn, n);
}

unsigned long CBigNum::getulong() const
{
    return mpz_get_ui(bn);
}

unsigned int CBigNum::getuint() const
{
    return mpz_get_ui(bn);
}

int CBigNum::getint() const
{
    unsigned long n = getulong();
    if (mpz_cmp(bn, BN_ZERO.bn) >= 0) {
        return (n > (unsigned long)std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : n);
    } else {
        return (n > (unsigned long)std::numeric_limits<int>::max() ? std::numeric_limits<int>::min() : -(int)n);
    }
}

void CBigNum::setuint256(uint256 n)
{
    mpz_import(bn, n.size(), -1, 1, 0, 0, (unsigned char*)&n);
}

arith_uint256 CBigNum::getuint256() const
{
    if(bitSize() > 256) {
        throw std::range_error("cannot convert to uint256, bignum longer than 256 bits");
    }
    uint256 n = UINT256_ZERO;
    mpz_export((unsigned char*)&n, NULL, -1, 1, 0, 0, bn);
    return UintToArith256(n);
}

void CBigNum::setvch(const std::vector<unsigned char>& vch)
{
    std::vector<unsigned char> vch2 = vch;
    unsigned char sign = 0;
    if (vch2.size() > 0) {
        sign = vch2[vch2.size()-1] & 0x80;
        vch2[vch2.size()-1] = vch2[vch2.size()-1] & 0x7f;
        mpz_import(bn, vch2.size(), -1, 1, 0, 0, vch2.data());
        if (sign)
            mpz_neg(bn, bn);
    }
    else {
        mpz_set_si(bn, 0);
    }
}

std::vector<unsigned char> CBigNum::getvch() const
{
    if (mpz_cmp(bn, BN_ZERO.bn) == 0) {
        return std::vector<unsigned char>(0);
    }
    size_t size = (mpz_sizeinbase (bn, 2) + CHAR_BIT-1) / CHAR_BIT;
    if (size <= 0)
        return std::vector<unsigned char>();
    std::vector<unsigned char> v(size + 1);
    mpz_export(v.data(), &size, -1, 1, 0, 0, bn);
    if (v[v.size()-2] & 0x80) {
        if (mpz_sgn(bn)<0) {
            v[v.size()-1] = 0x80;
        } else {
            v[v.size()-1] = 0x00;
        }
    } else {
        v.pop_back();
        if (mpz_sgn(bn)<0) {
            v[v.size()-1] |= 0x80;
        }
    }
    return v;
}

void CBigNum::SetDec(const std::string& str)
{
    const char* psz = str.c_str();
    mpz_set_str(bn, psz, 10);
}

bool CBigNum::SetHexBool(const std::string& str)
{
    const char* psz = str.c_str();
    int ret = 1 + mpz_set_str(bn, psz, 16);
    return (bool) ret;
}

std::string CBigNum::ToString(int nBase) const
{
    char* c_str = mpz_get_str(NULL, nBase, bn);
    std::string str(c_str);
    // Free c_str with the right free function:
    void (*freefunc)(void *, size_t);
    mp_get_memory_functions (NULL, NULL, &freefunc);
    freefunc(c_str, strlen(c_str) + 1);

    return str;
}

/**
 * exponentiation this^e
 * @param e the exponent
 * @return
 */
CBigNum CBigNum::pow(const CBigNum& e) const
{
    CBigNum ret;
    long unsigned int ei = mpz_get_ui (e.bn);
    mpz_pow_ui(ret.bn, bn, ei);
    return ret;
}

/**
 * modular multiplication: (this * b) mod m
 * @param b operand
 * @param m modulus
 */
CBigNum CBigNum::mul_mod(const CBigNum& b, const CBigNum& m) const
{
    CBigNum ret;
    mpz_mul (ret.bn, bn, b.bn);
    mpz_mod (ret.bn, ret.bn, m.bn);
    return ret;
}

/**
 * modular exponentiation: this^e mod n
 * @param e exponent
 * @param m modulus
 */
CBigNum CBigNum::pow_mod(const CBigNum& e, const CBigNum& m) const
{
    CBigNum ret;
    if (e > BN_ZERO && mpz_odd_p(m.bn))
        mpz_powm_sec (ret.bn, bn, e.bn, m.bn);
    else
        mpz_powm (ret.bn, bn, e.bn, m.bn);
    return ret;
}

/**
* Calculates the inverse of this element mod m.
* i.e. i such this*i = 1 mod m
* @param m the modu
* @return the inverse
*/
CBigNum CBigNum::inverse(const CBigNum& m) const
{
    CBigNum ret;
    mpz_invert(ret.bn, bn, m.bn);
    return ret;
}

/**
 * Calculates the greatest common divisor (GCD) of two numbers.
 * @param m the second element
 * @return the GCD
 */
CBigNum CBigNum::gcd( const CBigNum& b) const
{
    CBigNum ret;
    mpz_gcd(ret.bn, bn, b.bn);
    return ret;
}

/**
* Miller-Rabin primality test on this element
* @param checks: optional, the number of Miller-Rabin tests to run
*               default causes error rate of 2^-80.
* @return true if prime
*/
bool CBigNum::isPrime(const int checks) const
{
    int ret = mpz_probab_prime_p(bn, checks);
    return ret;
}

bool CBigNum::isOne() const
{
    return mpz_cmp(bn, BN_ONE.bn) == 0;
}

bool CBigNum::operator!() const
{
    return mpz_cmp(bn, BN_ZERO.bn) == 0;
}

CBigNum& CBigNum::operator+=(const CBigNum& b)
{
    mpz_add(bn, bn, b.bn);
    return *this;
}

CBigNum& CBigNum::operator-=(const CBigNum& b)
{
    mpz_sub(bn, bn, b.bn);
    return *this;
}

CBigNum& CBigNum::operator*=(const CBigNum& b)
{
    mpz_mul(bn, bn, b.bn);
    return *this;
}

CBigNum& CBigNum::operator<<=(unsigned int shift)
{
    mpz_mul_2exp(bn, bn, shift);
    return *this;
}

CBigNum& CBigNum::operator>>=(unsigned int shift)
{
    mpz_div_2exp(bn, bn, shift);
    return *this;
}

CBigNum& CBigNum::operator++()
{
    // prefix operator
    mpz_add(bn, bn, BN_ONE.bn);
    return *this;
}

CBigNum& CBigNum::operator--()
{
    // prefix operator
    mpz_sub(bn, bn, BN_ONE.bn);
    return *this;
}

std::string CBigNum::GetHex() const
{
    return ToString(16);
}

std::string CBigNum::GetDec() const
{
    return ToString(10);
}

CBigNum CBigNum::pow(const int e) const
{
    return this->pow(CBigNum(e));
}

void CBigNum::SetHex(const std::string& str)
{
    SetHexBool(str);
}

CBigNum& CBigNum::operator/=(const CBigNum& b)
{
    *this = *this / b;
    return *this;
}

CBigNum& CBigNum::operator%=(const CBigNum& b)
{
    *this = *this % b;
    return *this;
}

const CBigNum CBigNum::operator++(int)
{
    // postfix operator
    const CBigNum ret = *this;
    ++(*this);
    return ret;
}

const CBigNum CBigNum::operator--(int)
{
    // postfix operator
    const CBigNum ret = *this;
    --(*this);
    return ret;
}
