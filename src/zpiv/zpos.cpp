// Copyright (c) 2017-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zpiv/zpos.h"

#include "validation.h"
#include "zpivchain.h"


/*
 * LEGACY: Kept for IBD in order to verify zerocoin stakes occurred when zPoS was active
 * Find the first occurrence of a certain accumulator checksum.
 * Return block index pointer or nullptr if not found
 */

uint32_t ParseAccChecksum(uint256 nCheckpoint, const libzerocoin::CoinDenomination denom)
{
    int pos = std::distance(libzerocoin::zerocoinDenomList.begin(),
            find(libzerocoin::zerocoinDenomList.begin(), libzerocoin::zerocoinDenomList.end(), denom));
    return (UintToArith256(nCheckpoint) >> (32*((libzerocoin::zerocoinDenomList.size() - 1) - pos))).Get32();
}

static const CBlockIndex* FindIndexFrom(uint32_t nChecksum, libzerocoin::CoinDenomination denom, int cpHeight)
{
    // First look in the legacy database
    Optional<int> nHeightChecksum = accumulatorCache ? accumulatorCache->Get(nChecksum, denom) : nullopt;
    if (nHeightChecksum != nullopt) {
        return mapBlockIndex.at(chainActive[*nHeightChecksum]->GetBlockHash());
    }

    // Not found. This should never happen (during IBD we save the accumulator checksums
    // in the cache as they are updated, and persist the cache to DB) but better to have a fallback.
    LogPrintf("WARNING: accumulatorCache corrupt - missing (%d-%d), height=%d",
              nChecksum, libzerocoin::ZerocoinDenominationToInt(denom), cpHeight);

    // Start at the current checkpoint and go backwards
    const Consensus::Params& consensus = Params().GetConsensus();
    int zc_activation = consensus.vUpgrades[Consensus::UPGRADE_ZC].nActivationHeight;
    // Height limits are ensured by the contextual checks in NewZPivStake
    assert(cpHeight <= consensus.height_last_ZC_AccumCheckpoint && cpHeight > zc_activation);

    CBlockIndex* pindex = chainActive[(cpHeight/10)*10 - 10];
    if (!pindex) return nullptr;
    while (ParseAccChecksum(pindex->nAccumulatorCheckpoint, denom) == nChecksum && pindex->nHeight > zc_activation) {
        //Skip backwards in groups of 10 blocks since checkpoints only change every 10 blocks
        pindex = chainActive[pindex->nHeight - 10];
    }
    if (pindex->nHeight > zc_activation) {
        // Found. update cache.
        CBlockIndex* pindexFrom = mapBlockIndex.at(chainActive[pindex->nHeight + 10]->GetBlockHash());
        if (accumulatorCache) accumulatorCache->Set(nChecksum, denom, pindexFrom->nHeight);
        return pindexFrom;
    }
    return nullptr;
}

CLegacyZPivStake* CLegacyZPivStake::NewZPivStake(const CTxIn& txin, int nHeight)
{
    // Construct the stakeinput object
    if (!txin.IsZerocoinSpend()) {
        LogPrintf("%s: unable to initialize CLegacyZPivStake from non zc-spend", __func__);
        return nullptr;
    }

    // Return immediately if zPOS not enforced
    const Consensus::Params& consensus = Params().GetConsensus();
    if (!consensus.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_ZC_V2) ||
            nHeight >= consensus.height_last_ZC_AccumCheckpoint) {
        LogPrint(BCLog::LEGACYZC, "%s : zPIV stake block: height %d outside range", __func__, nHeight);
        return nullptr;
    }

    // Check spend type
    libzerocoin::CoinSpend spend = TxInToZerocoinSpend(txin);
    if (spend.getSpendType() != libzerocoin::SpendType::STAKE) {
        LogPrintf("%s : spend is using the wrong SpendType (%d)", __func__, (int)spend.getSpendType());
        return nullptr;
    }

    uint32_t _nChecksum = spend.getAccumulatorChecksum();
    libzerocoin::CoinDenomination _denom = spend.getDenomination();
    const arith_uint256& nSerial = spend.getCoinSerialNumber().getuint256();
    const uint256& _hashSerial = Hash(nSerial.begin(), nSerial.end());

    // The checkpoint needs to be from 200 blocks ago
    const int cpHeight = nHeight - 1 - consensus.ZC_MinStakeDepth;
    if (ParseAccChecksum(chainActive[cpHeight]->nAccumulatorCheckpoint, _denom) != _nChecksum) {
        LogPrint(BCLog::LEGACYZC, "%s : accum. checksum at height %d is wrong.", __func__, nHeight);
    }

    // Find the pindex of the first block with the accumulator checksum
    const CBlockIndex* _pindexFrom = FindIndexFrom(_nChecksum, _denom, cpHeight);
    if (_pindexFrom == nullptr) {
        LogPrintf("%s : Failed to find the block index for zpiv stake origin", __func__);
        return nullptr;
    }

    // All good
    return new CLegacyZPivStake(_pindexFrom, _nChecksum, _denom, _hashSerial);
}

const CBlockIndex* CLegacyZPivStake::GetIndexFrom() const
{
    return pindexFrom;
}

CAmount CLegacyZPivStake::GetValue() const
{
    return denom * COIN;
}

CDataStream CLegacyZPivStake::GetUniqueness() const
{
    CDataStream ss(SER_GETHASH, 0);
    ss << hashSerial;
    return ss;
}
