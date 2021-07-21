// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_KEY_IO_H
#define PIVX_KEY_IO_H

#include "chainparams.h"
#include "key.h"
#include "pubkey.h"
#include "script/standard.h"

#include <string>

std::string EncodeDestination(const CTxDestination& dest, bool isStaking);
std::string EncodeDestination(const CTxDestination& dest, const CChainParams::Base58Type addrType = CChainParams::PUBKEY_ADDRESS);
// DecodeDestinationisStaking flag is set to true when the string arg is from an staking address
CTxDestination DecodeDestination(const std::string& str, bool& isStaking);
CTxDestination DecodeDestination(const std::string& str);

// Return true if the address is valid and is following the fStaking flag type (true means that the destination must be a staking address, false the opposite).
bool IsValidDestinationString(const std::string& str, bool fStaking);
bool IsValidDestinationString(const std::string& str, bool fStaking, const CChainParams& params);

namespace KeyIO {

    CKey DecodeSecret(const std::string &str);

    std::string EncodeSecret(const CKey &key);

    CExtKey DecodeExtKey(const std::string &str);

    std::string EncodeExtKey(const CExtKey &extkey);

    CExtPubKey DecodeExtPubKey(const std::string& str);
    std::string EncodeExtPubKey(const CExtPubKey& extpubkey);

}

/**
 * Wrapper class for every supported address
 */
struct Destination {
public:
    explicit Destination() {}
    explicit Destination(const CTxDestination& _dest, bool _isP2CS) : dest(_dest), isP2CS(_isP2CS) {}

    CTxDestination dest{CNoDestination()};
    bool isP2CS{false};

    Destination& operator=(const Destination& from)
    {
        this->dest = from.dest;
        this->isP2CS = from.isP2CS;
        return *this;
    }

    std::string ToString() const
    {
        if (!IsValidDestination(dest)) {
            // Invalid address
            return "";
        }
        return EncodeDestination(dest, isP2CS ? CChainParams::STAKING_ADDRESS : CChainParams::PUBKEY_ADDRESS);
    }
};

#endif //PIVX_KEY_IO_H
