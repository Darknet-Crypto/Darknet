// Copyright (c) 2018-2021 The Dash Core developers
// Copyright (c) 2021 The PIVX Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "evo/providertx.h"

#include "core_io.h"
#include "evo/deterministicmns.h"
#include "key_io.h"
#include "messagesigner.h"
#include "evo/specialtx.h"
#include "tinyformat.h"
#include "utilstrencodings.h"
#include "validation.h"

/* -- Helper static functions -- */

static bool CheckService(const CService& addr, CValidationState& state)
{
    if (!addr.IsValid()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-ipaddr");
    }
    if (!Params().IsRegTestNet() && !addr.IsRoutable()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-ipaddr");
    }

    // IP port must be the default one on main-net, which cannot be used on other nets.
    static int mainnetDefaultPort = CreateChainParams(CBaseChainParams::MAIN)->GetDefaultPort();
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (addr.GetPort() != mainnetDefaultPort) {
            return state.DoS(10, false, REJECT_INVALID, "bad-protx-ipaddr-port");
        }
    } else if (addr.GetPort() == mainnetDefaultPort) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-ipaddr-port");
    }

    // !TODO: add support for IPv6 and Tor
    if (!addr.IsIPv4()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-ipaddr");
    }

    return true;
}

template <typename Payload>
static bool CheckHashSig(const Payload& pl, const CKeyID& keyID, CValidationState& state)
{
    std::string strError;
    if (!CHashSigner::VerifyHash(::SerializeHash(pl), keyID, pl.vchSig, strError)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-sig", false, strError);
    }
    return true;
}

template <typename Payload>
static bool CheckStringSig(const Payload& pl, const CKeyID& keyID, CValidationState& state)
{
    std::string strError;
    if (!CMessageSigner::VerifyMessage(keyID, pl.vchSig, pl.MakeSignString(), strError)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-sig", false, strError);
    }
    return true;
}

template <typename Payload>
static bool CheckInputsHash(const CTransaction& tx, const Payload& pl, CValidationState& state)
{
    if (CalcTxInputsHash(tx) != pl.inputsHash) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-inputs-hash");
    }

    return true;
}

// Provider Register Payload

static bool CheckCollateralOut(const CTxOut& out, const ProRegPL& pl, CValidationState& state, CTxDestination& collateralDestRet)
{
    if (!ExtractDestination(out.scriptPubKey, collateralDestRet)) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-collateral-dest");
    }
    // don't allow reuse of collateral key for other keys (don't allow people to put the collateral key onto an online server)
    // this check applies to internal and external collateral, but internal collaterals are not necessarely a P2PKH
    if (collateralDestRet == CTxDestination(pl.keyIDOwner) ||
            collateralDestRet == CTxDestination(pl.keyIDVoting) ||
            collateralDestRet == CTxDestination(pl.keyIDOperator)) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-collateral-reuse");
    }
    // check collateral amount
    if (out.nValue != Params().GetConsensus().nMNCollateralAmt) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-collateral-amount");
    }
    return true;
}

bool CheckProRegTx(const CTransaction& tx, const CBlockIndex* pindexPrev, CValidationState& state)
{
    assert(tx.nType == CTransaction::TxType::PROREG);

    ProRegPL pl;
    if (!GetTxPayload(tx, pl)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-payload");
    }

    if (pl.nVersion == 0 || pl.nVersion > ProRegPL::CURRENT_VERSION) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-version");
    }
    if (pl.nType != 0) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-type");
    }
    if (pl.nMode != 0) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-mode");
    }

    if (pl.keyIDOwner.IsNull() || pl.keyIDOperator.IsNull() || pl.keyIDVoting.IsNull()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-key-null");
    }
    // we may support other kinds of scripts later, but restrict it for now
    if (!pl.scriptPayout.IsPayToPublicKeyHash()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-payee");
    }
    if (!pl.scriptOperatorPayout.empty() && !pl.scriptOperatorPayout.IsPayToPublicKeyHash()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-operator-payee");
    }

    CTxDestination payoutDest;
    if (!ExtractDestination(pl.scriptPayout, payoutDest)) {
        // should not happen as we checked script types before
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-payee-dest");
    }
    // don't allow reuse of payout key for other keys (don't allow people to put the payee key onto an online server)
    if (payoutDest == CTxDestination(pl.keyIDOwner) ||
            payoutDest == CTxDestination(pl.keyIDVoting) ||
            payoutDest == CTxDestination(pl.keyIDOperator)) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-payee-reuse");
    }

    // It's allowed to set addr to 0, which will put the MN into PoSe-banned state and require a ProUpServTx to be issues later
    // If any of both is set, it must be valid however
    if (pl.addr != CService() && !CheckService(pl.addr, state)) {
        // pass the state returned by the function above
        return false;
    }

    if (pl.nOperatorReward > 10000) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-operator-reward");
    }

    if (pl.collateralOutpoint.hash.IsNull()) {
        // collateral included in the proReg tx
        if (pl.collateralOutpoint.n >= tx.vout.size()) {
            return state.DoS(10, false, REJECT_INVALID, "bad-protx-collateral-index");
        }
        CTxDestination collateralTxDest;
        if (!CheckCollateralOut(tx.vout[pl.collateralOutpoint.n], pl, state, collateralTxDest)) {
            // pass the state returned by the function above
            return false;
        }
        // collateral is part of this ProRegTx, so we know the collateral is owned by the issuer
        if (!pl.vchSig.empty()) {
            return state.DoS(100, false, REJECT_INVALID, "bad-protx-sig");
        }
    } else if (pindexPrev != nullptr) {
        // Referenced external collateral.
        // This is checked only when pindexPrev is not null (thus during ConnectBlock-->CheckSpecialTx),
        // because this is a contextual check: we need the updated utxo set, to verify that
        // the coin exists and it is unspent.
        Coin coin;
        if (!GetUTXOCoin(pl.collateralOutpoint, coin)) {
            return state.DoS(10, false, REJECT_INVALID, "bad-protx-collateral");
        }
        CTxDestination collateralTxDest;
        if (!CheckCollateralOut(coin.out, pl, state, collateralTxDest)) {
            // pass the state returned by the function above
            return false;
        }
        // Extract key from collateral. This only works for P2PK and P2PKH collaterals and will fail for P2SH.
        // Issuer of this ProRegTx must prove ownership with this key by signing the ProRegTx
        const CKeyID* keyForPayloadSig = boost::get<CKeyID>(&collateralTxDest);
        if (!keyForPayloadSig) {
            return state.DoS(10, false, REJECT_INVALID, "bad-protx-collateral-pkh");
        }
        // collateral is not part of this ProRegTx, so we must verify ownership of the collateral
        if (!CheckStringSig(pl, *keyForPayloadSig, state)) {
            // pass the state returned by the function above
            return false;
        }
    }

    if (!CheckInputsHash(tx, pl, state)) {
        return false;
    }

    if (pindexPrev) {
        auto mnList = deterministicMNManager->GetListForBlock(pindexPrev);
        // only allow reusing of addresses when it's for the same collateral (which replaces the old MN)
        if (mnList.HasUniqueProperty(pl.addr) && mnList.GetUniquePropertyMN(pl.addr)->collateralOutpoint != pl.collateralOutpoint) {
            return state.DoS(10, false, REJECT_DUPLICATE, "bad-protx-dup-IP-address");
        }
        // never allow duplicate keys, even if this ProTx would replace an existing MN
        if (mnList.HasUniqueProperty(pl.keyIDOwner)) {
            return state.DoS(10, false, REJECT_DUPLICATE, "bad-protx-dup-owner-key");
        }
        if (mnList.HasUniqueProperty(pl.keyIDOperator)) {
            return state.DoS(10, false, REJECT_DUPLICATE, "bad-protx-dup-operator-key");
        }
    }

    return true;
}

std::string ProRegPL::MakeSignString() const
{
    std::ostringstream ss;

    ss << HexStr(scriptPayout) << "|";
    ss << strprintf("%d", nOperatorReward) << "|";
    ss << EncodeDestination(keyIDOwner) << "|";
    ss << EncodeDestination(keyIDVoting) << "|";

    // ... and also the full hash of the payload as a protection agains malleability and replays
    ss << ::SerializeHash(*this).ToString();

    return ss.str();
}

std::string ProRegPL::ToString() const
{
    CTxDestination dest;
    std::string payee = ExtractDestination(scriptPayout, dest) ?
                        EncodeDestination(dest) : "unknown";
    return strprintf("ProRegPL(nVersion=%d, collateralOutpoint=%s, addr=%s, nOperatorReward=%f, ownerAddress=%s, operatorAddress=%s, votingAddress=%s, scriptPayout=%s)",
        nVersion, collateralOutpoint.ToStringShort(), addr.ToString(), (double)nOperatorReward / 100, EncodeDestination(keyIDOwner), EncodeDestination(keyIDOperator), EncodeDestination(keyIDVoting), payee);
}

void ProRegPL::ToJson(UniValue& obj) const
{
    obj.clear();
    obj.setObject();
    obj.pushKV("version", nVersion);
    obj.pushKV("collateralHash", collateralOutpoint.hash.ToString());
    obj.pushKV("collateralIndex", (int)collateralOutpoint.n);
    obj.pushKV("service", addr.ToString());
    obj.pushKV("ownerAddress", EncodeDestination(keyIDOwner));
    obj.pushKV("operatorAddress", EncodeDestination(keyIDOperator));
    obj.pushKV("votingAddress", EncodeDestination(keyIDVoting));

    CTxDestination dest1;
    if (ExtractDestination(scriptPayout, dest1)) {
        obj.pushKV("payoutAddress", EncodeDestination(dest1));
    }
    CTxDestination dest2;
    if (ExtractDestination(scriptOperatorPayout, dest2)) {
        obj.pushKV("operatorPayoutAddress", EncodeDestination(dest2));
    }
    obj.pushKV("operatorReward", (double)nOperatorReward / 100);
    obj.pushKV("inputsHash", inputsHash.ToString());
}

// Provider Update Service Payload

bool CheckProUpServTx(const CTransaction& tx, const CBlockIndex* pindexPrev, CValidationState& state)
{
    assert(tx.nType == CTransaction::TxType::PROUPSERV);

    ProUpServPL pl;
    if (!GetTxPayload(tx, pl)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-payload");
    }

    if (pl.nVersion == 0 || pl.nVersion > ProUpServPL::CURRENT_VERSION) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-version");
    }

    if (!CheckService(pl.addr, state)) {
        // pass the state returned by the function above
        return false;
    }

    if (!CheckInputsHash(tx, pl, state)) {
        // pass the state returned by the function above
        return false;
    }

    if (pindexPrev) {
        auto mnList = deterministicMNManager->GetListForBlock(pindexPrev);
        auto mn = mnList.GetMN(pl.proTxHash);
        if (!mn) {
            return state.DoS(100, false, REJECT_INVALID, "bad-protx-hash");
        }

        // don't allow updating to addresses already used by other MNs
        if (mnList.HasUniqueProperty(pl.addr) && mnList.GetUniquePropertyMN(pl.addr)->proTxHash != pl.proTxHash) {
            return state.DoS(10, false, REJECT_DUPLICATE, "bad-protx-dup-addr");
        }

        if (!pl.scriptOperatorPayout.empty()) {
            if (mn->nOperatorReward == 0) {
                // don't allow to set operator reward payee in case no operatorReward was set
                return state.DoS(10, false, REJECT_INVALID, "bad-protx-operator-payee");
            }
            // we may support other kinds of scripts later, but restrict it for now
            if (!pl.scriptOperatorPayout.IsPayToPublicKeyHash()) {
                return state.DoS(10, false, REJECT_INVALID, "bad-protx-operator-payee");
            }
        }

        // we can only check the signature if pindexPrev != nullptr and the MN is known
        if (!CheckHashSig(pl, mn->pdmnState->keyIDOperator, state)) {
            // pass the state returned by the function above
            return false;
        }
    }

    return true;
}

std::string ProUpServPL::ToString() const
{
    CTxDestination dest;
    std::string payee = ExtractDestination(scriptOperatorPayout, dest) ?
                        EncodeDestination(dest) : "unknown";
    return strprintf("ProUpServPL(nVersion=%d, proTxHash=%s, addr=%s, operatorPayoutAddress=%s)",
        nVersion, proTxHash.ToString(), addr.ToString(), payee);
}

void ProUpServPL::ToJson(UniValue& obj) const
{
    obj.clear();
    obj.setObject();
    obj.pushKV("version", nVersion);
    obj.pushKV("proTxHash", proTxHash.ToString());
    obj.pushKV("service", addr.ToString());
    CTxDestination dest;
    if (ExtractDestination(scriptOperatorPayout, dest)) {
        obj.pushKV("operatorPayoutAddress", EncodeDestination(dest));
    }
    obj.pushKV("inputsHash", inputsHash.ToString());
}

// Provider Update Registrar Payload

bool CheckProUpRegTx(const CTransaction& tx, const CBlockIndex* pindexPrev, CValidationState& state)
{
    assert(tx.nType == CTransaction::TxType::PROUPREG);

    ProUpRegPL pl;
    if (!GetTxPayload(tx, pl)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-payload");
    }

    if (pl.nVersion == 0 || pl.nVersion > ProUpRegPL::CURRENT_VERSION) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-version");
    }
    if (pl.nMode != 0) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-mode");
    }

    if (pl.keyIDOperator.IsNull()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-operator-key-null");
    }
    if (pl.keyIDVoting.IsNull()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-voting-key-null");
    }
    // !TODO: enable other scripts
    if (!pl.scriptPayout.IsPayToPublicKeyHash()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-payee");
    }

    CTxDestination payoutDest;
    if (!ExtractDestination(pl.scriptPayout, payoutDest)) {
        // should not happen as we checked script types before
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-payee-dest");
    }

    // don't allow reuse of payee key for other keys
    if (payoutDest == CTxDestination(pl.keyIDVoting) || payoutDest == CTxDestination(pl.keyIDOperator)) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-payee-reuse");
    }

    if (!CheckInputsHash(tx, pl, state)) {
        return false;
    }

    if (pindexPrev) {
        // ProUpReg txes are disabled when the legacy system is still active
        // !TODO: remove after complete transition to DMN
        if (!deterministicMNManager->LegacyMNObsolete(pindexPrev->nHeight + 1)) {
            return state.DoS(10, false, REJECT_INVALID, "spork-21-inactive");
        }

        auto mnList = deterministicMNManager->GetListForBlock(pindexPrev);
        auto dmn = mnList.GetMN(pl.proTxHash);
        if (!dmn) {
            return state.DoS(100, false, REJECT_INVALID, "bad-protx-hash");
        }

        // don't allow reuse of payee key for owner key
        if (payoutDest == CTxDestination(dmn->pdmnState->keyIDOwner)) {
            return state.DoS(10, false, REJECT_INVALID, "bad-protx-payee-reuse");
        }

        Coin coin;
        if (!GetUTXOCoin(dmn->collateralOutpoint, coin)) {
            // this should never happen (there would be no dmn otherwise)
            return state.DoS(100, false, REJECT_INVALID, "bad-protx-collateral");
        }

        // don't allow reuse of collateral key for other keys (don't allow people to put the payee key onto an online server)
        CTxDestination collateralTxDest;
        if (!ExtractDestination(coin.out.scriptPubKey, collateralTxDest)) {
            return state.DoS(100, false, REJECT_INVALID, "bad-protx-collateral-dest");
        }
        if (collateralTxDest == CTxDestination(dmn->pdmnState->keyIDOwner) ||
                collateralTxDest == CTxDestination(pl.keyIDVoting) ||
                collateralTxDest == CTxDestination(pl.keyIDOperator)) {
            return state.DoS(10, false, REJECT_INVALID, "bad-protx-collateral-reuse");
        }

        if (mnList.HasUniqueProperty(pl.keyIDOperator)) {
            auto otherDmn = mnList.GetUniquePropertyMN(pl.keyIDOperator);
            if (pl.proTxHash != otherDmn->proTxHash) {
                return state.DoS(10, false, REJECT_DUPLICATE, "bad-protx-dup-key");
            }
        }

        if (!CheckHashSig(pl, dmn->pdmnState->keyIDOwner, state)) {
            // pass the state returned by the function above
            return false;
        }

    }

    return true;
}

std::string ProUpRegPL::ToString() const
{
    CTxDestination dest;
    std::string payee = ExtractDestination(scriptPayout, dest) ?
                        EncodeDestination(dest) : "unknown";
    return strprintf("ProUpRegPL(nVersion=%d, proTxHash=%s, operatorAddress=%s, votingAddress=%s, payoutAddress=%s)",
        nVersion, proTxHash.ToString(), EncodeDestination(keyIDOperator), EncodeDestination(keyIDVoting), payee);
}

void ProUpRegPL::ToJson(UniValue& obj) const
{
    obj.clear();
    obj.setObject();
    obj.pushKV("version", nVersion);
    obj.pushKV("proTxHash", proTxHash.ToString());
    obj.pushKV("votingAddress", EncodeDestination(keyIDVoting));
    CTxDestination dest;
    if (ExtractDestination(scriptPayout, dest)) {
        obj.pushKV("payoutAddress", EncodeDestination(dest));
    }
    obj.pushKV("operatorAddress", EncodeDestination(keyIDOperator));
    obj.pushKV("inputsHash", inputsHash.ToString());
}

// Provider Update Revoke Payload

bool CheckProUpRevTx(const CTransaction& tx, const CBlockIndex* pindexPrev, CValidationState& state)
{
    assert(tx.nType == CTransaction::TxType::PROUPREV);

    ProUpRevPL pl;
    if (!GetTxPayload(tx, pl)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-payload");
    }

    if (pl.nVersion == 0 || pl.nVersion > ProUpRevPL::CURRENT_VERSION) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-version");
    }

    // pl.nReason < ProUpRevPL::REASON_NOT_SPECIFIED is always `false` since
    // pl.nReason is unsigned and ProUpRevPL::REASON_NOT_SPECIFIED == 0
    if (pl.nReason > ProUpRevPL::REASON_LAST) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-reason");
    }

    if (!CheckInputsHash(tx, pl, state)) {
        // pass the state returned by the function above
        return false;
    }

    if (pindexPrev) {
        auto mnList = deterministicMNManager->GetListForBlock(pindexPrev);
        auto dmn = mnList.GetMN(pl.proTxHash);
        if (!dmn)
            return state.DoS(100, false, REJECT_INVALID, "bad-protx-hash");

        if (!CheckHashSig(pl, dmn->pdmnState->keyIDOperator, state)) {
            // pass the state returned by the function above
            return false;
        }
    }

    return true;
}

std::string ProUpRevPL::ToString() const
{
    return strprintf("ProUpRevPL(nVersion=%d, proTxHash=%s, nReason=%d)",
                      nVersion, proTxHash.ToString(), nReason);
}

void ProUpRevPL::ToJson(UniValue& obj) const
{
    obj.clear();
    obj.setObject();
    obj.pushKV("version", nVersion);
    obj.pushKV("proTxHash", proTxHash.ToString());
    obj.pushKV("reason", (int)nReason);
    obj.pushKV("inputsHash", inputsHash.ToString());
}

bool GetProRegCollateral(const CTransactionRef& tx, COutPoint& outRet)
{
    if (tx == nullptr) {
        return false;
    }
    if (!tx->IsSpecialTx() || tx->nType != CTransaction::TxType::PROREG) {
        return false;
    }
    ProRegPL pl;
    if (!GetTxPayload(*tx, pl)) {
        return false;
    }
    outRet = pl.collateralOutpoint.hash.IsNull() ? COutPoint(tx->GetHash(), pl.collateralOutpoint.n)
                                                 : pl.collateralOutpoint;
    return true;
}
