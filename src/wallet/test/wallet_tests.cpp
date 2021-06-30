// Copyright (c) 2012-2014 The Bitcoin Core developers
// Copyright (c) 2019-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "wallet/test/wallet_test_fixture.h"

#include "consensus/merkle.h"
#include "rpc/server.h"
#include "txmempool.h"
#include "validation.h"
#include "wallet/wallet.h"
#include "wallet/walletutil.h"

#include <set>
#include <utility>
#include <vector>

#include <boost/test/unit_test.hpp>
#include <univalue.h>

extern UniValue importmulti(const JSONRPCRequest& request);
extern UniValue dumpwallet(const JSONRPCRequest& request);
extern UniValue importwallet(const JSONRPCRequest& request);

// how many times to run all the tests to have a chance to catch errors that only show up with particular random shuffles
#define RUN_TESTS 100

// some tests fail 1% of the time due to bad luck.
// we repeat those tests this many times and only complain if all iterations of the test fail
#define RANDOM_REPEATS 5

std::vector<std::unique_ptr<CWalletTx>> wtxn;

typedef std::set<std::pair<const CWalletTx*,unsigned int> > CoinSet;

BOOST_FIXTURE_TEST_SUITE(wallet_tests, WalletTestingSetup)

static const CWallet testWallet("dummy", CWalletDBWrapper::CreateDummy());
static std::vector<COutput> vCoins;

static void add_coin(const CAmount& nValue, int nAge = 6*24, bool fIsFromMe = false, int nInput=0)
{
    static int nextLockTime = 0;
    CMutableTransaction tx;
    tx.nLockTime = nextLockTime++;        // so all transactions get different hashes
    tx.vout.resize(nInput+1);
    tx.vout[nInput].nValue = nValue;
    if (fIsFromMe) {
        // IsFromMe() returns (GetDebit() > 0), and GetDebit() is 0 if vin.empty(),
        // so stop vin being empty, and cache a non-zero Debit to fake out IsFromMe()
        tx.vin.resize(1);
    }
    std::unique_ptr<CWalletTx> wtx(new CWalletTx(&testWallet, MakeTransactionRef(std::move(tx))));
    if (fIsFromMe) {
        wtx->m_amounts[CWalletTx::DEBIT].Set(ISMINE_SPENDABLE, 1);
    }
    COutput output(wtx.get(), nInput, nAge, true /* spendable */, true /* solvable */, true /* safe */);
    vCoins.push_back(output);
    wtxn.emplace_back(std::move(wtx));
}

static void empty_wallet(void)
{
    vCoins.clear();
    wtxn.clear();
}

static bool equal_sets(CoinSet a, CoinSet b)
{
    std::pair<CoinSet::iterator, CoinSet::iterator> ret = mismatch(a.begin(), a.end(), b.begin());
    return ret.first == a.end() && ret.second == b.end();
}

BOOST_AUTO_TEST_CASE(coin_selection_tests)
{
    CoinSet setCoinsRet, setCoinsRet2;
    CAmount nValueRet;

    LOCK(testWallet.cs_wallet);

    // test multiple times to allow for differences in the shuffle order
    for (int i = 0; i < RUN_TESTS; i++)
    {
        empty_wallet();

        // with an empty wallet we can't even pay one cent
        BOOST_CHECK(!testWallet.SelectCoinsMinConf( 1 * CENT, 1, 6, vCoins, setCoinsRet, nValueRet));

        add_coin(1*CENT, 4);        // add a new 1 cent coin

        // with a new 1 cent coin, we still can't find a mature 1 cent
        BOOST_CHECK(!testWallet.SelectCoinsMinConf( 1 * CENT, 1, 6, vCoins, setCoinsRet, nValueRet));

        // but we can find a new 1 cent
        BOOST_CHECK(testWallet.SelectCoinsMinConf( 1 * CENT, 1, 1, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 1 * CENT);

        add_coin(2*CENT);           // add a mature 2 cent coin

        // we can't make 3 cents of mature coins
        BOOST_CHECK(!testWallet.SelectCoinsMinConf( 3 * CENT, 1, 6, vCoins, setCoinsRet, nValueRet));

        // we can make 3 cents of new  coins
        BOOST_CHECK(testWallet.SelectCoinsMinConf( 3 * CENT, 1, 1, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 3 * CENT);

        add_coin(5*CENT);           // add a mature 5 cent coin,
        add_coin(10*CENT, 3, true); // a new 10 cent coin sent from one of our own addresses
        add_coin(20*CENT);          // and a mature 20 cent coin

        // now we have new: 1+10=11 (of which 10 was self-sent), and mature: 2+5+20=27.  total = 38

        // we can't make 38 cents only if we disallow new coins:
        BOOST_CHECK(!testWallet.SelectCoinsMinConf(38 * CENT, 1, 6, vCoins, setCoinsRet, nValueRet));
        // we can't even make 37 cents if we don't allow new coins even if they're from us
        BOOST_CHECK(!testWallet.SelectCoinsMinConf(38 * CENT, 6, 6, vCoins, setCoinsRet, nValueRet));
        // but we can make 37 cents if we accept new coins from ourself
        BOOST_CHECK(testWallet.SelectCoinsMinConf(37 * CENT, 1, 6, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 37 * CENT);
        // and we can make 38 cents if we accept all new coins
        BOOST_CHECK(testWallet.SelectCoinsMinConf(38 * CENT, 1, 1, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 38 * CENT);

        // try making 34 cents from 1,2,5,10,20 - we can't do it exactly
        BOOST_CHECK(testWallet.SelectCoinsMinConf(34 * CENT, 1, 1, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_GT(nValueRet, 34 * CENT);         // but should get more than 34 cents
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 3U);     // the best should be 20+10+5.  it's incredibly unlikely the 1 or 2 got included (but possible)

        // when we try making 7 cents, the smaller coins (1,2,5) are enough.  We should see just 2+5
        BOOST_CHECK(testWallet.SelectCoinsMinConf( 7 * CENT, 1, 1, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 7 * CENT);
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 2U);

        // when we try making 8 cents, the smaller coins (1,2,5) are exactly enough.
        BOOST_CHECK(testWallet.SelectCoinsMinConf( 8 * CENT, 1, 1, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK(nValueRet == 8 * CENT);
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 3U);

        // when we try making 9 cents, no subset of smaller coins is enough, and we get the next bigger coin (10)
        BOOST_CHECK(testWallet.SelectCoinsMinConf( 9 * CENT, 1, 1, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 10 * CENT);
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 1U);

        // now clear out the wallet and start again to test choosing between subsets of smaller coins and the next biggest coin
        empty_wallet();

        add_coin(6*CENT);
        add_coin(7*CENT);
        add_coin(8*CENT);
        add_coin(20*CENT);
        add_coin(30*CENT); // now we have 6+7+8+20+30 = 71 cents total

        // check that we have 71 and not 72
        BOOST_CHECK(testWallet.SelectCoinsMinConf(71 * CENT, 1, 1, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK(!testWallet.SelectCoinsMinConf(72 * CENT, 1, 1, vCoins, setCoinsRet, nValueRet));

        // now try making 16 cents.  the best smaller coins can do is 6+7+8 = 21; not as good at the next biggest coin, 20
        BOOST_CHECK(testWallet.SelectCoinsMinConf(16 * CENT, 1, 1, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 20 * CENT); // we should get 20 in one coin
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 1U);

        add_coin(5*CENT); // now we have 5+6+7+8+20+30 = 75 cents total

        // now if we try making 16 cents again, the smaller coins can make 5+6+7 = 18 cents, better than the next biggest coin, 20
        BOOST_CHECK(testWallet.SelectCoinsMinConf(16 * CENT, 1, 1, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 18 * CENT); // we should get 18 in 3 coins
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 3U);

        add_coin(18*CENT); // now we have 5+6+7+8+18+20+30

        // and now if we try making 16 cents again, the smaller coins can make 5+6+7 = 18 cents, the same as the next biggest coin, 18
        BOOST_CHECK(testWallet.SelectCoinsMinConf(16 * CENT, 1, 1, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 18 * CENT);  // we should get 18 in 1 coin
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 1U); // because in the event of a tie, the biggest coin wins

        // now try making 11 cents.  we should get 5+6
        BOOST_CHECK(testWallet.SelectCoinsMinConf(11 * CENT, 1, 1, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 11 * CENT);
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 2U);

        // check that the smallest bigger coin is used
        add_coin(1*COIN);
        add_coin(2*COIN);
        add_coin(3*COIN);
        add_coin(4*COIN); // now we have 5+6+7+8+18+20+30+100+200+300+400 = 1094 cents
        BOOST_CHECK(testWallet.SelectCoinsMinConf(95 * CENT, 1, 1, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 1 * COIN);  // we should get 1 BTC in 1 coin
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 1U);

        BOOST_CHECK(testWallet.SelectCoinsMinConf(195 * CENT, 1, 1, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 2 * COIN);  // we should get 2 BTC in 1 coin
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 1U);

        // empty the wallet and start again, now with fractions of a cent, to test sub-cent change avoidance
        empty_wallet();
        add_coin(0.1*CENT);
        add_coin(0.2*CENT);
        add_coin(0.3*CENT);
        add_coin(0.4*CENT);
        add_coin(0.5*CENT);

        // try making 1 cent from 0.1 + 0.2 + 0.3 + 0.4 + 0.5 = 1.5 cents
        // we'll get sub-cent change whatever happens, so can expect 1.0 exactly
        BOOST_CHECK(testWallet.SelectCoinsMinConf(1 * CENT, 1, 1, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 1 * CENT);

        // but if we add a bigger coin, making it possible to avoid sub-cent change, things change:
        add_coin(1111*CENT);

        // try making 1 cent from 0.1 + 0.2 + 0.3 + 0.4 + 0.5 + 1111 = 1112.5 cents
        BOOST_CHECK(testWallet.SelectCoinsMinConf(1 * CENT, 1, 1, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 1 * CENT); // we should get the exact amount

        // if we add more sub-cent coins:
        add_coin(0.6*CENT);
        add_coin(0.7*CENT);

        // and try again to make 1.0 cents, we can still make 1.0 cents
        BOOST_CHECK(testWallet.SelectCoinsMinConf(1 * CENT, 1, 1, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 1 * CENT); // we should get the exact amount

        // run the 'mtgox' test (see http://blockexplorer.com/tx/29a3efd3ef04f9153d47a990bd7b048a4b2d213daaa5fb8ed670fb85f13bdbcf)
        // they tried to consolidate 10 50k coins into one 500k coin, and ended up with 50k in change
        empty_wallet();
        for (int j = 0; j < 20; j++)
            add_coin(50000 * COIN);

        BOOST_CHECK(testWallet.SelectCoinsMinConf(500000 * COIN, 1, 1, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 500000 * COIN); // we should get the exact amount
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 10U); // in ten coins

        // if there's not enough in the smaller coins to make at least 1 cent change (0.5+0.6+0.7 < 1.0+1.0),
        // we need to try finding an exact subset anyway

        // sometimes it will fail, and so we use the next biggest coin:
        empty_wallet();
        add_coin(0.5 * CENT);
        add_coin(0.6 * CENT);
        add_coin(0.7 * CENT);
        add_coin(1111 * CENT);
        BOOST_CHECK(testWallet.SelectCoinsMinConf(1 * CENT, 1, 1, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 1111 * CENT); // we get the bigger coin
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 1U);

        // but sometimes it's possible, and we use an exact subset (0.4 + 0.6 = 1.0)
        empty_wallet();
        add_coin(0.4 * CENT);
        add_coin(0.6 * CENT);
        add_coin(0.8 * CENT);
        add_coin(1111 * CENT);
        BOOST_CHECK(testWallet.SelectCoinsMinConf(1 * CENT, 1, 1, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 1 * CENT);   // we should get the exact amount
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 2U); // in two coins 0.4+0.6

        // test avoiding sub-cent change
        empty_wallet();
        add_coin(0.0005 * COIN);
        add_coin(0.01 * COIN);
        add_coin(1 * COIN);

        // trying to make 1.0001 from these three coins
        BOOST_CHECK(testWallet.SelectCoinsMinConf(1.0001 * COIN, 1, 1, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 1.0105 * COIN);   // we should get all coins
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 3U);

        // but if we try to make 0.999, we should take the bigger of the two small coins to avoid sub-cent change
        BOOST_CHECK(testWallet.SelectCoinsMinConf(0.999 * COIN, 1, 1, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 1.01 * COIN);   // we should get 1 + 0.01
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 2U);

        // test randomness
        {
            empty_wallet();
            for (int i2 = 0; i2 < 100; i2++)
                add_coin(COIN);

            // picking 50 from 100 coins doesn't depend on the shuffle,
            // but does depend on randomness in the stochastic approximation code
            BOOST_CHECK(testWallet.SelectCoinsMinConf(50 * COIN, 1, 6, vCoins, setCoinsRet , nValueRet));
            BOOST_CHECK(testWallet.SelectCoinsMinConf(50 * COIN, 1, 6, vCoins, setCoinsRet2, nValueRet));
            BOOST_CHECK(!equal_sets(setCoinsRet, setCoinsRet2));

            int fails = 0;
            for (int j = 0; j < RANDOM_REPEATS; j++)
            {
                // selecting 1 from 100 identical coins depends on the shuffle; this test will fail 1% of the time
                // run the test RANDOM_REPEATS times and only complain if all of them fail
                BOOST_CHECK(testWallet.SelectCoinsMinConf(COIN, 1, 6, vCoins, setCoinsRet , nValueRet));
                BOOST_CHECK(testWallet.SelectCoinsMinConf(COIN, 1, 6, vCoins, setCoinsRet2, nValueRet));
                if (equal_sets(setCoinsRet, setCoinsRet2))
                    fails++;
            }
            BOOST_CHECK_NE(fails, RANDOM_REPEATS);

            // add 75 cents in small change.  not enough to make 90 cents,
            // then try making 90 cents.  there are multiple competing "smallest bigger" coins,
            // one of which should be picked at random
            add_coin(5 * CENT);
            add_coin(10 * CENT);
            add_coin(15 * CENT);
            add_coin(20 * CENT);
            add_coin(25 * CENT);

            fails = 0;
            for (int j = 0; j < RANDOM_REPEATS; j++)
            {
                // selecting 1 from 100 identical coins depends on the shuffle; this test will fail 1% of the time
                // run the test RANDOM_REPEATS times and only complain if all of them fail
                BOOST_CHECK(testWallet.SelectCoinsMinConf(90*CENT, 1, 6, vCoins, setCoinsRet , nValueRet));
                BOOST_CHECK(testWallet.SelectCoinsMinConf(90*CENT, 1, 6, vCoins, setCoinsRet2, nValueRet));
                if (equal_sets(setCoinsRet, setCoinsRet2))
                    fails++;
            }
            BOOST_CHECK_NE(fails, RANDOM_REPEATS);
        }
    }
    empty_wallet();
}

static void AddKey(CWallet& wallet, const CKey& key)
{
    LOCK(wallet.cs_wallet);
    wallet.AddKeyPubKey(key, key.GetPubKey());
}

BOOST_FIXTURE_TEST_CASE(rescan, TestChain100Setup)
{
    // Cap last block file size, and mine new block in a new block file.
    CBlockIndex* const nullBlock = nullptr;
    CBlockIndex* oldTip = chainActive.Tip();
    GetBlockFileInfo(oldTip->GetBlockPos().nFile)->nSize = MAX_BLOCKFILE_SIZE;
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    CBlockIndex* newTip = chainActive.Tip();

    LOCK(cs_main);

    // Verify ScanForWalletTransactions picks up transactions in both the old
    // and new block files.
    {
        CWallet wallet("dummy", CWalletDBWrapper::CreateDummy());
        WITH_LOCK(wallet.cs_wallet, wallet.SetLastBlockProcessed(newTip); );
        AddKey(wallet, coinbaseKey);
        WalletRescanReserver reserver(&wallet);
        reserver.reserve();
        BOOST_CHECK_EQUAL(nullBlock, wallet.ScanForWalletTransactions(oldTip, nullptr, reserver));
        BOOST_CHECK_EQUAL(wallet.GetImmatureBalance(), 500 * COIN);
    }

    // !TODO: Prune the older block file.
    /*
    PruneOneBlockFile(oldTip->GetBlockPos().nFile);
    UnlinkPrunedFiles({oldTip->GetBlockPos().nFile});

    // Verify ScanForWalletTransactions only picks transactions in the new block
    // file.
    {
        CWallet wallet("dummy", CWalletDBWrapper::CreateDummy());
        AddKey(wallet, coinbaseKey);
        WalletRescanReserver reserver(&wallet);
        reserver.reserve();
        BOOST_CHECK_EQUAL(oldTip, wallet.ScanForWalletTransactions(oldTip, nullptr, reserver));;
        BOOST_CHECK_EQUAL(wallet.GetImmatureBalance(), 250 * COIN);
    }
    */

    // Verify importmulti RPC returns failure for a key whose creation time is
    // before the missing block, and success for a key whose creation time is
    // after.
    {
        CWallet wallet("dummy", CWalletDBWrapper::CreateDummy());
        WITH_LOCK(wallet.cs_wallet, wallet.SetLastBlockProcessed(newTip); );
        vpwallets.insert(vpwallets.begin(), &wallet);
        UniValue keys;
        keys.setArray();
        UniValue key;
        key.setObject();
        key.pushKV("scriptPubKey", HexStr(GetScriptForRawPubKey(coinbaseKey.GetPubKey())));
        key.pushKV("timestamp", 0);
        key.pushKV("internal", UniValue(true));
        keys.push_back(key);
        key.clear();
        key.setObject();
        CKey futureKey;
        futureKey.MakeNewKey(true);
        key.pushKV("scriptPubKey", HexStr(GetScriptForRawPubKey(futureKey.GetPubKey())));
        key.pushKV("timestamp", newTip->GetBlockTimeMax() + TIMESTAMP_WINDOW + 1);
        key.pushKV("internal", UniValue(true));
        keys.push_back(key);
        JSONRPCRequest request;
        request.params.setArray();
        request.params.push_back(keys);

        UniValue response = importmulti(request);
        // !TODO: after pruning, check that the rescan for the first key fails.
        BOOST_CHECK_EQUAL(response.write(), "[{\"success\":true},{\"success\":true}]");
        vpwallets.erase(vpwallets.begin());
    }
}

// Verify importwallet RPC starts rescan at earliest block with timestamp
// greater or equal than key birthday. Previously there was a bug where
// importwallet RPC would start the scan at the latest block with timestamp less
// than or equal to key birthday.
BOOST_FIXTURE_TEST_CASE(importwallet_rescan, TestChain100Setup)
{
    // Create one block
    const int64_t BLOCK_TIME = chainActive.Tip()->GetBlockTimeMax() + 15;
    SetMockTime(BLOCK_TIME);
    coinbaseTxns.emplace_back(*CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey())).vtx[0]);

    // Set key birthday to block time increased by the timestamp window, so
    // rescan will start at the block time.
    const int64_t KEY_TIME = BLOCK_TIME + TIMESTAMP_WINDOW;
    SetMockTime(KEY_TIME);
    coinbaseTxns.emplace_back(*CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey())).vtx[0]);

    std::string backup_file = (SetDataDir("importwallet_rescan") / "wallet.backup").string();

    // Import key into wallet and call dumpwallet to create backup file.
    {
        CWallet wallet("dummy", CWalletDBWrapper::CreateDummy());
        {
            LOCK(wallet.cs_wallet);
            wallet.mapKeyMetadata[coinbaseKey.GetPubKey().GetID()].nCreateTime = KEY_TIME;
            wallet.AddKeyPubKey(coinbaseKey, coinbaseKey.GetPubKey());
        }

        JSONRPCRequest request;
        request.params.setArray();
        request.params.push_back(backup_file);
        vpwallets.insert(vpwallets.begin(), &wallet);
        ::dumpwallet(request);
    }

    // Call importwallet RPC and verify all blocks with timestamps >= BLOCK_TIME
    // were scanned, and no prior blocks were scanned.
    {
        CWallet wallet("dummy", CWalletDBWrapper::CreateDummy());

        JSONRPCRequest request;
        request.params.setArray();
        request.params.push_back(backup_file);
        vpwallets[0] = &wallet;
        ::importwallet(request);

        LOCK(wallet.cs_wallet);
        BOOST_CHECK_EQUAL(wallet.mapWallet.size(), 2);
        BOOST_CHECK_EQUAL(coinbaseTxns.size(), 102);
        for (size_t i = 0; i < coinbaseTxns.size(); ++i) {
            bool found = wallet.GetWalletTx(coinbaseTxns[i].GetHash());
            bool expected = i >= 100;
            BOOST_CHECK_EQUAL(found, expected);
        }
    }

    SetMockTime(0);
    vpwallets.erase(vpwallets.begin());
}

void removeTxFromMempool(CWalletTx& wtx)
{
    LOCK(mempool.cs);
    if (mempool.exists(wtx.GetHash())) {
        auto it = mempool.mapTx.find(wtx.GetHash());
        if (it != mempool.mapTx.end()) {
            mempool.mapTx.erase(it);
        }
    }
}

/**
 * Mimic block creation.
 */
CBlockIndex* SimpleFakeMine(CWalletTx& wtx, CWallet &wallet, CBlockIndex* pprev = nullptr)
{
    CBlock block;
    block.vtx.emplace_back(wtx.tx);
    block.hashMerkleRoot = BlockMerkleRoot(block);
    if (pprev) block.hashPrevBlock = pprev->GetBlockHash();
    CBlockIndex* fakeIndex = new CBlockIndex(block);
    fakeIndex->pprev = pprev;
    mapBlockIndex.emplace(block.GetHash(), fakeIndex);
    fakeIndex->phashBlock = &mapBlockIndex.find(block.GetHash())->first;
    chainActive.SetTip(fakeIndex);
    BOOST_CHECK(chainActive.Contains(fakeIndex));
    WITH_LOCK(wallet.cs_wallet, wallet.SetLastBlockProcessed(fakeIndex));
    wtx.m_confirm = CWalletTx::Confirmation(CWalletTx::Status::CONFIRMED, fakeIndex->nHeight, fakeIndex->GetBlockHash(), 0);
    removeTxFromMempool(wtx);
    wtx.fInMempool = false;
    return fakeIndex;
}

void fakeMempoolInsertion(const CTransactionRef& wtxCredit)
{
    CTxMemPoolEntry entry(wtxCredit, 0, 0, 0, 0, false, 0, false, 0);
    LOCK(mempool.cs);
    mempool.mapTx.insert(entry);
}

CWalletTx& BuildAndLoadTxToWallet(const std::vector<CTxIn>& vin,
                                  const std::vector<CTxOut>& vout,
                                  CWallet& wallet)
{
    CMutableTransaction mTx;
    mTx.vin = vin;
    mTx.vout = vout;
    CTransaction tx(mTx);
    CWalletTx wtx(&wallet, MakeTransactionRef(tx));
    wallet.LoadToWallet(wtx);
    return wallet.mapWallet.at(tx.GetHash());
}

CWalletTx& ReceiveBalanceWith(const std::vector<CTxOut>& vout,
                          CWallet& wallet)
{
    std::vector<CTxIn> vin;
    vin.emplace_back(CTxIn(COutPoint(uint256(), 999)));
    return BuildAndLoadTxToWallet(vin, vout, wallet);
}

void CheckBalances(const CWalletTx& tx,
                   const CAmount& nCreditAll,
                   const CAmount& nCreditSpendable,
                   const CAmount& nAvailableCredit,
                   const CAmount& nDebitAll,
                   const CAmount& nDebitSpendable)
{
    BOOST_CHECK_EQUAL(tx.GetCredit(ISMINE_ALL), nCreditAll);
    BOOST_CHECK_EQUAL(tx.GetCredit(ISMINE_SPENDABLE), nCreditSpendable);
    BOOST_CHECK(tx.IsAmountCached(CWalletTx::CREDIT, ISMINE_SPENDABLE));
    BOOST_CHECK_EQUAL(tx.GetAvailableCredit(), nAvailableCredit);
    BOOST_CHECK(tx.IsAmountCached(CWalletTx::AVAILABLE_CREDIT, ISMINE_SPENDABLE));
    BOOST_CHECK_EQUAL(tx.GetDebit(ISMINE_ALL), nDebitAll);
    BOOST_CHECK_EQUAL(tx.GetDebit(ISMINE_SPENDABLE), nDebitSpendable);
    BOOST_CHECK(tx.IsAmountCached(CWalletTx::DEBIT, ISMINE_SPENDABLE));
}

/**
 * Validates the correct behaviour of the CWalletTx "standard" balance methods.
 * (where "standard" is defined by direct P2PKH scripts, no P2CS contracts nor other types)
 *
 * 1) CWalletTx::GetCredit.
 * 2) CWalletTx::GetDebit.
 * 4) CWalletTx::GetAvailableCredit
 * 3) CWallet::GetUnconfirmedBalance.
 */
BOOST_AUTO_TEST_CASE(cached_balances_tests)
{
    // 1) Receive balance from an external source and verify:
    // * GetCredit(ISMINE_ALL) correctness (must be equal to 'nCredit' amount)
    // * GetCredit(ISMINE_SPENDABLE) correctness (must be equal to ISMINE_ALL) + must be cached.
    // * GetAvailableCredit() correctness (must be equal to ISMINE_ALL)
    // * GetDebit(ISMINE_ALL) correctness (must be 0)
    // * wallet.GetUnconfirmedBalance() correctness (must be equal 'nCredit')

    // 2) Confirm the tx and verify:
    // * wallet.GetUnconfirmedBalance() correctness (must be 0)
    // * GetAvailableCredit() correctness (must be equal to (1) ISMINE_ALL)

    // 3) Spend one of the two outputs of the receiving tx to an external source
    // and verify:
    // * creditTx.GetAvailableCredit() correctness (must be equal to 'nCredit' / 2) + must be cached.
    // * debitTx.GetDebit(ISMINE_ALL) correctness (must be equal to 'nCredit' / 2)
    // * debitTx.GetDebit(ISMINE_SPENDABLE) correctness (must be equal to 'nCredit' / 2) + must be cached.
    // * debitTx.GetAvailableCredit() correctness (must be 0).

    CAmount nCredit = 20 * COIN;

    // Setup wallet
    CWallet wallet("testWallet1", CWalletDBWrapper::CreateMock());
    bool fFirstRun;
    BOOST_CHECK_EQUAL(wallet.LoadWallet(fFirstRun), DB_LOAD_OK);
    LOCK2(cs_main, wallet.cs_wallet);
    wallet.SetMinVersion(FEATURE_PRE_SPLIT_KEYPOOL);
    wallet.SetupSPKM(false);
    wallet.SetLastBlockProcessed(chainActive.Tip());

    // Receive balance from an external source
    CTxDestination receivingAddr;
    BOOST_ASSERT(wallet.getNewAddress(receivingAddr, "receiving_address").result);
    CTxOut creditOut(nCredit/2, GetScriptForDestination(receivingAddr));
    CWalletTx& wtxCredit = ReceiveBalanceWith({creditOut, creditOut},wallet);

    // Validates (1)
    CheckBalances(
            wtxCredit,
            nCredit,            // CREDIT-ISMINE_ALL
            nCredit,            // CREDIT-ISMINE_SPENDABLE
            nCredit,            // AVAILABLE_CREDIT
            0,                  // DEBIT-ISMINE_ALL
            0                   // DEBIT-ISMINE_SPENDABLE
    );

    // GetUnconfirmedBalance requires tx in mempool.
    fakeMempoolInsertion(wtxCredit.tx);
    wtxCredit.fInMempool = true;
    BOOST_CHECK_EQUAL(wallet.GetUnconfirmedBalance(), nCredit);

    // 2) Confirm tx and verify
    SimpleFakeMine(wtxCredit, wallet);
    BOOST_CHECK_EQUAL(wallet.GetUnconfirmedBalance(), 0);
    BOOST_CHECK_EQUAL(wtxCredit.GetAvailableCredit(), nCredit);

    // 3) Spend one of the two outputs of the receiving tx to an external source and verify.
    // Create debit transaction.
    CAmount nDebit = nCredit / 2;
    std::vector<CTxIn> vinDebit = {CTxIn(COutPoint(wtxCredit.GetHash(), 0))};
    CKey key;
    key.MakeNewKey(true);
    std::vector<CTxOut> voutDebit = {CTxOut(nDebit, GetScriptForDestination(key.GetPubKey().GetID()))};
    CWalletTx& wtxDebit = BuildAndLoadTxToWallet(vinDebit, voutDebit, wallet);

    // Validates (3)

    // First the debit tx
    CheckBalances(
            wtxDebit,
            0,                   // CREDIT-ISMINE_ALL
            0,                   // CREDIT-ISMINE_SPENDABLE
            0,                   // AVAILABLE_CREDIT
            nDebit,              // DEBIT-ISMINE_ALL
            nDebit               // DEBIT-ISMINE_SPENDABLE
    );

    // Secondly the prev credit tx update

    // One output spent, the other one not. Force available credit recalculation.
    // If we don't request it, it will not happen.
    BOOST_CHECK_EQUAL(wtxCredit.GetAvailableCredit(false), nCredit - nDebit);
    BOOST_CHECK(wtxCredit.IsAmountCached(CWalletTx::AVAILABLE_CREDIT, ISMINE_SPENDABLE));

}

BOOST_AUTO_TEST_SUITE_END()
