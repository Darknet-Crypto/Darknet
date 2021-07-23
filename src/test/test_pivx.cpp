// Copyright (c) 2011-2013 The Bitcoin Core developers
// Copyright (c) 2017-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#define BOOST_TEST_MODULE Pivx Test Suite

#include "test/test_pivx.h"

#include "blockassembler.h"
#include "consensus/merkle.h"
#include "guiinterface.h"
#include "evo/deterministicmns.h"
#include "evo/evodb.h"
#include "evo/evonotificationinterface.h"
#include "miner.h"
#include "net_processing.h"
#include "rpc/server.h"
#include "rpc/register.h"
#include "script/sigcache.h"
#include "sporkdb.h"
#include "txmempool.h"
#include "txdb.h"
#include "validation.h"

#include <boost/test/unit_test.hpp>

std::unique_ptr<CConnman> g_connman;

CClientUIInterface uiInterface;  // Declared but not defined in guiinterface.h

FastRandomContext insecure_rand_ctx;

extern bool fPrintToConsole;
extern void noui_connect();

std::ostream& operator<<(std::ostream& os, const uint256& num)
{
    os << num.ToString();
    return os;
}

BasicTestingSetup::BasicTestingSetup(const std::string& chainName)
    : m_path_root(fs::temp_directory_path() / "test_pivx" / strprintf("%lu_%i", (unsigned long)GetTime(), (int)(InsecureRandRange(1 << 30))))
{
    ECC_Start();
    SetupEnvironment();
    InitSignatureCache();
    fCheckBlockIndex = true;
    SelectParams(chainName);
    evoDb.reset(new CEvoDB(1 << 20, true, true));
    deterministicMNManager.reset(new CDeterministicMNManager(*evoDb));
}

BasicTestingSetup::~BasicTestingSetup()
{
    fs::remove_all(m_path_root);
    ECC_Stop();
    deterministicMNManager.reset();
    evoDb.reset();
}

fs::path BasicTestingSetup::SetDataDir(const std::string& name)
{
    fs::path ret = m_path_root / name;
    fs::create_directories(ret);
    gArgs.ForceSetArg("-datadir", ret.string());
    return ret;
}

TestingSetup::TestingSetup(const std::string& chainName) : BasicTestingSetup(chainName)
{
        SetDataDir("tempdir");
        ClearDatadirCache();

        // Start the lightweight task scheduler thread
        CScheduler::Function serviceLoop = std::bind(&CScheduler::serviceQueue, &scheduler);
        threadGroup.create_thread(std::bind(&TraceThread<CScheduler::Function>, "scheduler", serviceLoop));

        // Note that because we don't bother running a scheduler thread here,
        // callbacks via CValidationInterface are unreliable, but that's OK,
        // our unit tests aren't testing multiple parts of the code at once.
        GetMainSignals().RegisterBackgroundSignalScheduler(scheduler);

        // Register EvoNotificationInterface
        g_connman = std::unique_ptr<CConnman>(new CConnman(0x1337, 0x1337)); // Deterministic randomness for tests.
        connman = g_connman.get();
        pEvoNotificationInterface = new EvoNotificationInterface(*connman);
        RegisterValidationInterface(pEvoNotificationInterface);

        // Ideally we'd move all the RPC tests to the functional testing framework
        // instead of unit tests, but for now we need these here.
        RegisterAllCoreRPCCommands(tableRPC);
        zerocoinDB = new CZerocoinDB(0, true);
        pSporkDB = new CSporkDB(0, true);
        pblocktree = new CBlockTreeDB(1 << 20, true);
        pcoinsdbview = new CCoinsViewDB(1 << 23, true);
        pcoinsTip = new CCoinsViewCache(pcoinsdbview);
        if (!LoadGenesisBlock()) {
            throw std::runtime_error("Error initializing block database");
        }
        {
            CValidationState state;
            bool ok = ActivateBestChain(state);
            BOOST_CHECK(ok);
        }
        nScriptCheckThreads = 3;
        for (int i=0; i < nScriptCheckThreads-1; i++)
            threadGroup.create_thread(&ThreadScriptCheck);
        peerLogic.reset(new PeerLogicValidation(connman));
}

TestingSetup::~TestingSetup()
{
        threadGroup.interrupt_all();
        threadGroup.join_all();
        GetMainSignals().FlushBackgroundCallbacks();
        UnregisterAllValidationInterfaces();
        GetMainSignals().UnregisterBackgroundSignalScheduler();
        g_connman.reset();
        peerLogic.reset();
        UnloadBlockIndex();
        delete pEvoNotificationInterface;
        delete pcoinsTip;
        delete pcoinsdbview;
        delete pblocktree;
        delete zerocoinDB;
        delete pSporkDB;
}

// Test chain only available on regtest
TestChainSetup::TestChainSetup(int blockCount) : TestingSetup(CBaseChainParams::REGTEST)
{
    // if blockCount is over PoS start, delay it to 100 blocks after.
    if (blockCount > Params().GetConsensus().vUpgrades[Consensus::UPGRADE_POS].nActivationHeight) {
        UpdateNetworkUpgradeParameters(Consensus::UPGRADE_POS, blockCount + 100);
        UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V3_4, blockCount + 101);
    }

    // Generate a blockCount-block chain:
    coinbaseKey.MakeNewKey(true);
    CScript scriptPubKey = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    for (int i = 0; i < blockCount; i++)
    {
        std::vector<CMutableTransaction> noTxns;
        CBlock b = CreateAndProcessBlock(noTxns, scriptPubKey);
        coinbaseTxns.push_back(*b.vtx[0]);
    }
}

// Create a new block with coinbase paying to scriptPubKey, and try to add it to the current chain.
// Include given transactions, and, if fNoMempoolTx=true, remove transactions coming from the mempool.
CBlock TestChainSetup::CreateAndProcessBlock(const std::vector<CMutableTransaction>& txns, const CScript& scriptPubKey, bool fNoMempoolTx)
{
    CBlock block = CreateBlock(txns, scriptPubKey, fNoMempoolTx);
    ProcessNewBlock(std::make_shared<const CBlock>(block), nullptr);
    return block;
}

CBlock TestChainSetup::CreateAndProcessBlock(const std::vector<CMutableTransaction>& txns, const CKey& scriptKey)
{
    CScript scriptPubKey = CScript() <<  ToByteVector(scriptKey.GetPubKey()) << OP_CHECKSIG;
    return CreateAndProcessBlock(txns, scriptPubKey);
}

CBlock TestChainSetup::CreateBlock(const std::vector<CMutableTransaction>& txns, const CScript& scriptPubKey, bool fNoMempoolTx)
{
    std::unique_ptr<CBlockTemplate> pblocktemplate = BlockAssembler(
            Params(), DEFAULT_PRINTPRIORITY).CreateNewBlock(scriptPubKey, nullptr, false, nullptr, fNoMempoolTx);
    std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>(pblocktemplate->block);

    // Add passed-in txns:
    for (const CMutableTransaction& tx : txns) {
        pblock->vtx.push_back(MakeTransactionRef(tx));
    }

    const int nHeight = WITH_LOCK(cs_main, return chainActive.Height()) + 1;

    // Re-compute sapling root
    pblock->hashFinalSaplingRoot = CalculateSaplingTreeRoot(pblock.get(), nHeight, Params());

    // Find valid PoW
    assert(SolveBlock(pblock, nHeight));
    return *pblock;
}

CBlock TestChainSetup::CreateBlock(const std::vector<CMutableTransaction>& txns, const CKey& scriptKey)
{
    CScript scriptPubKey = CScript() <<  ToByteVector(scriptKey.GetPubKey()) << OP_CHECKSIG;
    return CreateBlock(txns, scriptPubKey);
}

std::shared_ptr<CBlock> FinalizeBlock(std::shared_ptr<CBlock> pblock)
{
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
    while (!CheckProofOfWork(pblock->GetHash(), pblock->nBits)) { ++(pblock->nNonce); }
    return pblock;
}

TestChainSetup::~TestChainSetup()
{
}

CTxMemPoolEntry TestMemPoolEntryHelper::FromTx(const CMutableTransaction& tx)
{
    CTransaction txn(tx);
    return FromTx(txn);
}

CTxMemPoolEntry TestMemPoolEntryHelper::FromTx(const CTransaction& txn)
{
    return CTxMemPoolEntry(MakeTransactionRef(txn), nFee, nTime, nHeight,
                           spendsCoinbaseOrCoinstake, sigOpCount);
}

[[noreturn]] void Shutdown(void* parg)
{
    std::exit(0);
}

[[noreturn]] void StartShutdown()
{
    std::exit(0);
}

bool ShutdownRequested()
{
  return false;
}
