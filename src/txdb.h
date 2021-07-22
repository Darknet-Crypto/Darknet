// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2016-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TXDB_H
#define BITCOIN_TXDB_H

#include "coins.h"
#include "chain.h"
#include "dbwrapper.h"
#include "libzerocoin/Coin.h"
#include "libzerocoin/CoinSpend.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

class CCoinsViewDBCursor;
class uint256;

//! No need to periodic flush if at least this much space still available.
static constexpr int MAX_BLOCK_COINSDB_USAGE = 10;
//! -dbcache default (MiB)
static const int64_t nDefaultDbCache = 300;
//! -dbbatchsize default (bytes)
static const int64_t nDefaultDbBatchSize = 16 << 20;
//! max. -dbcache (MiB)
static const int64_t nMaxDbCache = sizeof(void*) > 4 ? 16384 : 1024;
//! min. -dbcache (MiB)
static const int64_t nMinDbCache = 4;
//! Max memory allocated to block tree DB specific cache, if no -txindex (MiB)
static const int64_t nMaxBlockDBCache = 2;
//! Max memory allocated to block tree DB specific cache, if -txindex (MiB)
// Unlike for the UTXO database, for the txindex scenario the leveldb cache make
// a meaningful difference: https://github.com/bitcoin/bitcoin/pull/8273#issuecomment-229601991
static const int64_t nMaxBlockDBAndTxIndexCache = 1024;
//! Max memory allocated to coin DB specific cache (MiB)
static const int64_t nMaxCoinsDBCache = 8;

struct CDiskTxPos : public FlatFilePos
{
    unsigned int nTxOffset; // after header

    SERIALIZE_METHODS(CDiskTxPos, obj)
    {
        READWRITEAS(FlatFilePos, obj);
        READWRITE(VARINT(obj.nTxOffset));
    }

    CDiskTxPos(const FlatFilePos& blockIn, unsigned int nTxOffsetIn) : FlatFilePos(blockIn.nFile, blockIn.nPos), nTxOffset(nTxOffsetIn)
    {
    }

    CDiskTxPos()
    {
        SetNull();
    }

    void SetNull()
    {
        FlatFilePos::SetNull();
        nTxOffset = 0;
    }
};

/** CCoinsView backed by the LevelDB coin database (chainstate/) */
class CCoinsViewDB : public CCoinsView
{
protected:
    CDBWrapper db;

public:
    CCoinsViewDB(size_t nCacheSize, bool fMemory = false, bool fWipe = false);

    bool GetCoin(const COutPoint& outpoint, Coin& coin) const override;
    bool HaveCoin(const COutPoint& outpoint) const override;
    uint256 GetBestBlock() const override;
    std::vector<uint256> GetHeadBlocks() const override;
    CCoinsViewCursor* Cursor() const override;

    //! Attempt to update from an older database format. Returns whether an error occurred.
    bool Upgrade();
    size_t EstimateSize() const override;

    bool BatchWrite(CCoinsMap& mapCoins,
                    const uint256& hashBlock,
                    const uint256& hashSaplingAnchor,
                    CAnchorsSaplingMap& mapSaplingAnchors,
                    CNullifiersMap& mapSaplingNullifiers) override;

    // Sapling, the implementation of the following functions can be found in sapling_txdb.cpp.
    bool GetSaplingAnchorAt(const uint256 &rt, SaplingMerkleTree &tree) const override;
    bool GetNullifier(const uint256 &nf) const override;
    uint256 GetBestAnchor() const override;
    bool BatchWriteSapling(const uint256& hashSaplingAnchor,
                           CAnchorsSaplingMap& mapSaplingAnchors,
                           CNullifiersMap& mapSaplingNullifiers,
                           CDBBatch& batch);
};

/** Specialization of CCoinsViewCursor to iterate over a CCoinsViewDB */
class CCoinsViewDBCursor: public CCoinsViewCursor
{
public:
    ~CCoinsViewDBCursor() {}

    bool GetKey(COutPoint& key) const;
    bool GetValue(Coin& coin) const;
    unsigned int GetValueSize() const;

    bool Valid() const;
    void Next();

private:
    CCoinsViewDBCursor(CDBIterator* pcursorIn, const uint256& hashBlockIn):
        CCoinsViewCursor(hashBlockIn), pcursor(pcursorIn) {}
    std::unique_ptr<CDBIterator> pcursor;
    std::pair<char, COutPoint> keyTmp;

    friend class CCoinsViewDB;
};

/** Access to the block database (blocks/index/) */
class CBlockTreeDB : public CDBWrapper
{
public:
    explicit CBlockTreeDB(size_t nCacheSize, bool fMemory = false, bool fWipe = false);

    CBlockTreeDB(const CBlockTreeDB&) = delete;
    CBlockTreeDB& operator=(const CBlockTreeDB&) = delete;

    bool WriteBlockIndex(const CDiskBlockIndex& blockindex);
    bool WriteBatchSync(const std::vector<std::pair<int, const CBlockFileInfo*> >& fileInfo, int nLastFile, const std::vector<const CBlockIndex*>& blockinfo);
    bool ReadBlockFileInfo(int nFile, CBlockFileInfo& info);
    bool ReadLastBlockFile(int& nFile);
    bool WriteReindexing(bool fReindexing);
    bool ReadReindexing(bool& fReindexing);
    bool ReadTxIndex(const uint256& txid, CDiskTxPos& pos);
    bool WriteTxIndex(const std::vector<std::pair<uint256, CDiskTxPos> >& vect);
    bool WriteFlag(const std::string& name, bool fValue);
    bool ReadFlag(const std::string& name, bool& fValue);
    bool WriteInt(const std::string& name, int nValue);
    bool ReadInt(const std::string& name, int& nValue);
    bool LoadBlockIndexGuts(std::function<CBlockIndex*(const uint256&)> insertBlockIndex);
};

/** Zerocoin database (zerocoin/) */
class CZerocoinDB : public CDBWrapper
{
public:
    CZerocoinDB(size_t nCacheSize, bool fMemory = false, bool fWipe = false);

private:
    CZerocoinDB(const CZerocoinDB&);
    void operator=(const CZerocoinDB&);

public:
    /** Write zPIV spends to the zerocoinDB in a batch
     * Pair of: CBigNum -> coinSerialNumber and uint256 -> txHash.
     */
    bool WriteCoinSpendBatch(const std::vector<std::pair<CBigNum, uint256> >& spendInfo);
    bool ReadCoinSpend(const CBigNum& bnSerial, uint256& txHash);
    bool EraseCoinSpend(const CBigNum& bnSerial);

    /** Accumulators (only for zPoS IBD): [checksum, denom] --> block height **/
    bool WriteAccChecksum(const uint32_t nChecksum, const libzerocoin::CoinDenomination denom, const int nHeight);
    bool ReadAccChecksum(const uint32_t nChecksum, const libzerocoin::CoinDenomination denom, int& nHeightRet);
    bool EraseAccChecksum(const uint32_t nChecksum, const libzerocoin::CoinDenomination denom);
    bool WipeAccChecksums();
};

#endif // BITCOIN_TXDB_H
