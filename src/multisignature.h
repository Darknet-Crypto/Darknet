// Copyright (c) 2017-2018 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_MULTISIGNATURE_H
#define PIVX_MULTISIGNATURE_H


#include <script/script.h>
#include <utility>
#include "base58.h"
#include "init.h"


class CMultisignatureAddress
{
private:
    CBitcoinAddress addressFull;
    CScript scriptRedeem;
    std::vector<CPubKey> vOwners;
    int nOwners = -1;
    int nSigsRequired = -1;
    std::string strErrorStatus = "";

    CMultisignatureAddress() = default;
    bool HandleError(const std::string& err);
    bool AddToWallet();
    void CreateEmptyInstance();
    void CreateRedeemScript();
    bool ValidateConfiguration();
    bool ConvertAndValidatePubKeys(std::vector<std::string> vstrPubKeys);
    bool ParseRedeemScript(const std::string& strRedeemScript);
    void ParseRPCRedeem(const std::string& strRedeemScript);
    void ParseHexRedeem(const std::string& strRedeemScript);
    void ParseSpacedRedeem(const std::string& strRedeemScript);

public:
    CMultisignatureAddress(int nSignaturesRequired, std::vector<std::string> vAddressOwners);
    explicit CMultisignatureAddress(std::string strRedeemScript);
    CScript getRedeemScript() const { return scriptRedeem; }
    CBitcoinAddress getAddress() const { return addressFull; }
    int getSignaturesRequired() const { return nSigsRequired; }
    std::string getErrorStatus() const { return strErrorStatus; }
    std::vector<CPubKey> getAddressOwners() const { return vOwners; }
};


class CMultisignatureException : public std::exception
{
private:
    const std::string strErr;
public:
    explicit CMultisignatureException(std::string strError) : strErr(std::move(strError)) {}
    const char * what() const noexcept override { return strErr.data(); }
};


#endif //PIVX_MULTISIGNATURE_H
