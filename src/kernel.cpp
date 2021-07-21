// Copyright (c) 2011-2013 The PPCoin developers
// Copyright (c) 2013-2014 The NovaCoin Developers
// Copyright (c) 2014-2018 The BlackCoin Developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "kernel.h"

#include "db.h"
#include "legacy/stakemodifier.h"
#include "policy/policy.h"
#include "script/interpreter.h"
#include "stakeinput.h"
#include "util/system.h"
#include "utilmoneystr.h"
#include "validation.h"
#include "zpivchain.h"
#include "zpiv/zpos.h"

/**
 * CStakeKernel Constructor
 *
 * @param[in]   pindexPrev      index of the parent of the kernel block
 * @param[in]   stakeInput      input for the coinstake of the kernel block
 * @param[in]   nBits           target difficulty bits of the kernel block
 * @param[in]   nTimeTx         time of the kernel block
 */
CStakeKernel::CStakeKernel(const CBlockIndex* const pindexPrev, CStakeInput* stakeInput, unsigned int nBits, int nTimeTx):
    stakeUniqueness(stakeInput->GetUniqueness()),
    nTime(nTimeTx),
    nBits(nBits),
    stakeValue(stakeInput->GetValue())
{
    // Set kernel stake modifier
    if (!Params().GetConsensus().NetworkUpgradeActive(pindexPrev->nHeight + 1, Consensus::UPGRADE_V3_4)) {
        uint64_t nStakeModifier = 0;
        if (!GetOldStakeModifier(stakeInput, nStakeModifier))
            LogPrintf("%s : ERROR: Failed to get kernel stake modifier\n", __func__);
        // Modifier v1
        stakeModifier << nStakeModifier;
    } else {
        // Modifier v2
        stakeModifier << pindexPrev->GetStakeModifierV2();
    }
    const CBlockIndex* pindexFrom = stakeInput->GetIndexFrom();
    nTimeBlockFrom = pindexFrom->nTime;
}

// Return stake kernel hash
uint256 CStakeKernel::GetHash() const
{
    CDataStream ss(stakeModifier);
    ss << nTimeBlockFrom << stakeUniqueness << nTime;
    return Hash(ss.begin(), ss.end());
}

// Check that the kernel hash meets the target required
bool CStakeKernel::CheckKernelHash(bool fSkipLog) const
{
    // Get weighted target
    arith_uint256 bnTarget;
    bnTarget.SetCompact(nBits);
    bnTarget *= (arith_uint256(stakeValue) / 100);

    // Check PoS kernel hash
    const arith_uint256& hashProofOfStake = UintToArith256(GetHash());
    const bool res = hashProofOfStake < bnTarget;

    if (!fSkipLog || res) {
        LogPrint(BCLog::STAKING, "%s : Proof Of Stake:"
                            "\nstakeModifier=%s"
                            "\nnTimeBlockFrom=%d"
                            "\nssUniqueID=%s"
                            "\nnTimeTx=%d"
                            "\nhashProofOfStake=%s"
                            "\nnBits=%d"
                            "\nweight=%d"
                            "\nbnTarget=%s (res: %d)\n\n",
            __func__, HexStr(stakeModifier), nTimeBlockFrom, HexStr(stakeUniqueness), nTime, hashProofOfStake.GetHex(),
            nBits, stakeValue, bnTarget.GetHex(), res);
    }
    return res;
}


/*
 * PoS Validation
 */

// helper function for CheckProofOfStake and GetStakeKernelHash
static bool LoadStakeInput(const CBlock& block, std::unique_ptr<CStakeInput>& stake, int nHeight)
{
    // Check that this is a PoS block
    if (!block.IsProofOfStake())
        return error("called on non PoS block");

    // Construct the stakeinput object
    const CTxIn& txin = block.vtx[1]->vin[0];
    stake = txin.IsZerocoinSpend() ?
            std::unique_ptr<CStakeInput>(CLegacyZPivStake::NewZPivStake(txin, nHeight)) :
            std::unique_ptr<CStakeInput>(CPivStake::NewPivStake(txin, nHeight, block.nTime));

    return stake != nullptr;
}

/*
 * Stake                Check if stakeInput can stake a block on top of pindexPrev
 *
 * @param[in]   pindexPrev      index of the parent block of the block being staked
 * @param[in]   stakeInput      input for the coinstake
 * @param[in]   nBits           target difficulty bits
 * @param[in]   nTimeTx         new blocktime
 * @return      bool            true if stake kernel hash meets target protocol
 */
bool Stake(const CBlockIndex* pindexPrev, CStakeInput* stakeInput, unsigned int nBits, int64_t& nTimeTx)
{
    if (!stakeInput) return false;

    // Get the new time slot (and verify it's not the same as previous block)
    const bool fRegTest = Params().IsRegTestNet();
    nTimeTx = (fRegTest ? GetAdjustedTime() : GetCurrentTimeSlot());
    if (nTimeTx <= pindexPrev->nTime && !fRegTest) return false;

    // Verify Proof Of Stake
    CStakeKernel stakeKernel(pindexPrev, stakeInput, nBits, nTimeTx);
    return stakeKernel.CheckKernelHash(true);
}


/*
 * CheckProofOfStake    Check if block has valid proof of stake
 *
 * @param[in]   block           block being verified
 * @param[out]  strError        string error (if any, else empty)
 * @param[in]   pindexPrev      index of the parent block
 *                              (if nullptr, it will be searched in mapBlockIndex)
 * @return      bool            true if the block has a valid proof of stake
 */
bool CheckProofOfStake(const CBlock& block, std::string& strError, const CBlockIndex* pindexPrev)
{
    const int nHeight = pindexPrev->nHeight + 1;
    // Initialize stake input
    std::unique_ptr<CStakeInput> stakeInput;
    if (!LoadStakeInput(block, stakeInput, nHeight)) {
        strError = "stake input initialization failed";
        return false;
    }

    // Verify Proof Of Stake
    CStakeKernel stakeKernel(pindexPrev, stakeInput.get(), block.nBits, block.nTime);
    if (!stakeKernel.CheckKernelHash()) {
        strError = "kernel hash check fails";
        return false;
    }

    // zPoS disabled (ContextCheck) before blocks V7, and the tx input signature is in CoinSpend
    if (stakeInput->IsZPIV()) return true;

    // Verify tx input signature
    CTxOut stakePrevout;
    if (!stakeInput->GetTxOutFrom(stakePrevout)) {
        strError = "unable to get stake prevout for coinstake";
        return false;
    }
    const auto& tx = block.vtx[1];
    const CTxIn& txin = tx->vin[0];
    ScriptError serror;
    if (!VerifyScript(txin.scriptSig, stakePrevout.scriptPubKey, STANDARD_SCRIPT_VERIFY_FLAGS,
             TransactionSignatureChecker(tx.get(), 0, stakePrevout.nValue), tx->GetRequiredSigVersion(), &serror)) {
        strError = strprintf("signature fails: %s", serror ? ScriptErrorString(serror) : "");
        return false;
    }

    // All good
    return true;
}


/*
 * GetStakeKernelHash   Return stake kernel of a block
 *
 * @param[out]  hashRet         hash of the kernel (set by this function)
 * @param[in]   block           block with the kernel to return
 * @param[in]   pindexPrev      index of the parent block
 *                              (if nullptr, it will be searched in mapBlockIndex)
 * @return      bool            false if kernel cannot be initialized, true otherwise
 */
bool GetStakeKernelHash(uint256& hashRet, const CBlock& block, const CBlockIndex* pindexPrev)
{
    // Initialize stake input
    std::unique_ptr<CStakeInput> stakeInput;
    if (!LoadStakeInput(block, stakeInput, pindexPrev->nHeight + 1))
        return error("%s : stake input initialization failed", __func__);

    CStakeKernel stakeKernel(pindexPrev, stakeInput.get(), block.nBits, block.nTime);
    hashRet = stakeKernel.GetHash();
    return true;
}

