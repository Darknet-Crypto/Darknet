// Copyright (c) 2015-2019 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_TEST_TEST_PIVX_H
#define PIVX_TEST_TEST_PIVX_H

#include "fs.h"
#include "scheduler.h"
#include "txdb.h"

#include <boost/thread.hpp>

extern uint256 insecure_rand_seed;
extern FastRandomContext insecure_rand_ctx;

static inline void SeedInsecureRand(bool fDeterministic = false)
{
    if (fDeterministic) {
        insecure_rand_seed = uint256();
    } else {
        insecure_rand_seed = GetRandHash();
    }
    insecure_rand_ctx = FastRandomContext(insecure_rand_seed);
}

static inline uint32_t InsecureRand32() { return insecure_rand_ctx.rand32(); }
static inline uint256 InsecureRand256() { return insecure_rand_ctx.rand256(); }
static inline uint64_t InsecureRandBits(int bits) { return insecure_rand_ctx.randbits(bits); }
static inline uint64_t InsecureRandRange(uint64_t range) { return insecure_rand_ctx.randrange(range); }
static inline bool InsecureRandBool() { return insecure_rand_ctx.randbool(); }
static inline std::vector<unsigned char> InsecureRandBytes(size_t len) { return insecure_rand_ctx.randbytes(len); }

/** Basic testing setup.
 * This just configures logging and chain parameters.
 */
struct BasicTestingSetup {
    BasicTestingSetup();
    ~BasicTestingSetup();
};

/** Testing setup that configures a complete environment.
 * Included are data directory, coins database, script check threads
 * and wallet (if enabled) setup.
 */
class CConnman;
struct TestingSetup: public BasicTestingSetup {
    CCoinsViewDB *pcoinsdbview;
    fs::path pathTemp;
    boost::thread_group threadGroup;
    CConnman* connman;
    CScheduler scheduler;
    ECCVerifyHandle globalVerifyHandle;

    TestingSetup();
    ~TestingSetup();
};

class CBlock;
struct CMutableTransaction;
class CScript;

struct TestChainSetup : public TestingSetup
{
    TestChainSetup(int blockCount);
    ~TestChainSetup();

    // Create a new block with just given transactions, coinbase paying to
    // scriptPubKey, and try to add it to the current chain.
    CBlock CreateAndProcessBlock(const std::vector<CMutableTransaction>& txns, const CScript& scriptPubKey);
    CBlock CreateAndProcessBlock(const std::vector<CMutableTransaction>& txns, const CKey& scriptKey);
    CBlock CreateBlock(const std::vector<CMutableTransaction>& txns, const CScript& scriptPubKey);
    CBlock CreateBlock(const std::vector<CMutableTransaction>& txns, const CKey& scriptKey);

    std::vector<CTransaction> coinbaseTxns; // For convenience, coinbase transactions
    CKey coinbaseKey; // private/public key needed to spend coinbase transactions
};

// Testing fixture that pre-creates a 100-block REGTEST-mode blockchain
struct TestChain100Setup : public TestChainSetup {
    TestChain100Setup() : TestChainSetup(100) {}
};

// Testing fixture that pre-creates a 400-block REGTEST-mode blockchain
// all 400 blocks are PoW. PoS starts at height 500
struct TestChain400Setup : public TestChainSetup {
    TestChain400Setup() : TestChainSetup(400) {}
};

class CTxMemPoolEntry;
class CTxMemPool;

struct TestMemPoolEntryHelper
{
    // Default values
    CAmount nFee;
    int64_t nTime;
    double dPriority;
    unsigned int nHeight;
    bool hadNoDependencies;
    bool spendsCoinbaseOrCoinstake;
    unsigned int sigOpCount;

    TestMemPoolEntryHelper() :
        nFee(0), nTime(0), dPriority(0.0), nHeight(1),
        hadNoDependencies(false), spendsCoinbaseOrCoinstake(false), sigOpCount(1) { }

    CTxMemPoolEntry FromTx(CMutableTransaction &tx, CTxMemPool *pool = nullptr);

    // Change the default value
    TestMemPoolEntryHelper &Fee(CAmount _fee) { nFee = _fee; return *this; }
    TestMemPoolEntryHelper &Time(int64_t _time) { nTime = _time; return *this; }
    TestMemPoolEntryHelper &Priority(double _priority) { dPriority = _priority; return *this; }
    TestMemPoolEntryHelper &Height(unsigned int _height) { nHeight = _height; return *this; }
    TestMemPoolEntryHelper &HadNoDependencies(bool _hnd) { hadNoDependencies = _hnd; return *this; }
    TestMemPoolEntryHelper &SpendsCoinbaseOrCoinstake(bool _flag) { spendsCoinbaseOrCoinstake = _flag; return *this; }
    TestMemPoolEntryHelper &SigOps(unsigned int _sigops) { sigOpCount = _sigops; return *this; }
};

#endif
