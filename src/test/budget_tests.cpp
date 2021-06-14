// Copyright (c) 2018-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test_pivx.h"

#include "bls/bls_wrapper.h"
#include "budget/budgetmanager.h"
#include "masternode-payments.h"
#include "masternode-sync.h"
#include "spork.h"
#include "tinyformat.h"
#include "utilmoneystr.h"
#include "validation.h"

#include <boost/test/unit_test.hpp>


BOOST_FIXTURE_TEST_SUITE(budget_tests, TestingSetup)

void CheckBudgetValue(int nHeight, std::string strNetwork, CAmount nExpectedValue)
{
    CBudgetManager budget;
    CAmount nBudget = g_budgetman.GetTotalBudget(nHeight);
    std::string strError = strprintf("Budget is not as expected for %s. Result: %s, Expected: %s", strNetwork, FormatMoney(nBudget), FormatMoney(nExpectedValue));
    BOOST_CHECK_MESSAGE(nBudget == nExpectedValue, strError);
}

BOOST_AUTO_TEST_CASE(budget_value)
{
    SelectParams(CBaseChainParams::TESTNET);
    int nHeightTest = Params().GetConsensus().vUpgrades[Consensus::UPGRADE_ZC_V2].nActivationHeight + 1;
    CheckBudgetValue(nHeightTest-1, "testnet", 7200*COIN);
    CheckBudgetValue(nHeightTest, "testnet", 144*COIN);

    SelectParams(CBaseChainParams::MAIN);
    nHeightTest = Params().GetConsensus().vUpgrades[Consensus::UPGRADE_ZC_V2].nActivationHeight + 1;
    CheckBudgetValue(nHeightTest, "mainnet", 43200*COIN);
}

BOOST_AUTO_TEST_CASE(block_value)
{
    SelectParams(CBaseChainParams::TESTNET);
    std::string strError;

    // force mnsync complete
    masternodeSync.RequestedMasternodeAssets = MASTERNODE_SYNC_FINISHED;

    // enable SPORK_13
    int64_t nTime = GetTime() - 10;
    const CSporkMessage& spork = CSporkMessage(SPORK_13_ENABLE_SUPERBLOCKS, nTime + 1, nTime);
    sporkManager.AddOrUpdateSporkMessage(spork);
    BOOST_CHECK(sporkManager.IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS));

    // regular block
    int nHeight = 100;
    const CAmount nBlockReward = GetBlockValue(nHeight);
    CAmount nExpectedRet = nBlockReward;
    CAmount nBudgetAmtRet = 0;
    BOOST_CHECK(IsBlockValueValid(nHeight, nExpectedRet, nBlockReward-1, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward);
    BOOST_CHECK_EQUAL(nBudgetAmtRet, 0);
    BOOST_CHECK(IsBlockValueValid(nHeight, nExpectedRet, nBlockReward, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward);
    BOOST_CHECK_EQUAL(nBudgetAmtRet, 0);
    BOOST_CHECK(!IsBlockValueValid(nHeight, nExpectedRet, nBlockReward+1, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward);
    BOOST_CHECK_EQUAL(nBudgetAmtRet, 0);

    // superblock - create the finalized budget with a proposal, and vote on it
    nHeight = 144;
    const CTxIn mnVin(GetRandHash(), 0);
    const CScript payee = GetScriptForDestination(CKeyID(uint160(ParseHex("816115944e077fe7c803cfa57f29b36bf87c1d35"))));
    const CAmount propAmt = 100 * COIN;
    const uint256& propHash = GetRandHash(), finTxId = GetRandHash();
    const CTxBudgetPayment txBudgetPayment(propHash, payee, propAmt);
    CFinalizedBudget fin("main (test)", 144, {txBudgetPayment}, finTxId);
    const CFinalizedBudgetVote fvote(mnVin, fin.GetHash());
    BOOST_CHECK(fin.AddOrUpdateVote(fvote, strError));
    g_budgetman.ForceAddFinalizedBudget(fin.GetHash(), fin.GetFeeTXHash(), fin);

    // check superblock's block-value
    nExpectedRet = nBlockReward;
    nBudgetAmtRet = 0;
    BOOST_CHECK(IsBlockValueValid(nHeight, nExpectedRet, nBlockReward, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward + propAmt);
    BOOST_CHECK_EQUAL(nBudgetAmtRet, propAmt);
    nExpectedRet = nBlockReward;
    nBudgetAmtRet = 0;
    BOOST_CHECK(IsBlockValueValid(nHeight, nExpectedRet, nBlockReward+propAmt-1, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward + propAmt);
    BOOST_CHECK_EQUAL(nBudgetAmtRet, propAmt);
    nExpectedRet = nBlockReward;
    nBudgetAmtRet = 0;
    BOOST_CHECK(IsBlockValueValid(nHeight, nExpectedRet, nBlockReward+propAmt, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward + propAmt);
    BOOST_CHECK_EQUAL(nBudgetAmtRet, propAmt);
    nExpectedRet = nBlockReward;
    nBudgetAmtRet = 0;
    BOOST_CHECK(!IsBlockValueValid(nHeight, nExpectedRet, nBlockReward+propAmt+1, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward + propAmt);
    BOOST_CHECK_EQUAL(nBudgetAmtRet, propAmt);

    // disable SPORK_13
    const CSporkMessage& spork2 = CSporkMessage(SPORK_13_ENABLE_SUPERBLOCKS, 4070908800ULL, nTime + 1);
    sporkManager.AddOrUpdateSporkMessage(spork2);
    BOOST_CHECK(!sporkManager.IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS));

    // check with spork disabled
    nExpectedRet = nBlockReward;
    nBudgetAmtRet = 0;
    BOOST_CHECK(IsBlockValueValid(nHeight, nExpectedRet, nBlockReward, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward);
    BOOST_CHECK_EQUAL(nBudgetAmtRet, 0);
    BOOST_CHECK(!IsBlockValueValid(nHeight, nExpectedRet, nBlockReward+propAmt-1, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward);
    BOOST_CHECK_EQUAL(nBudgetAmtRet, 0);
    BOOST_CHECK(!IsBlockValueValid(nHeight, nExpectedRet, nBlockReward+propAmt, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward);
    BOOST_CHECK_EQUAL(nBudgetAmtRet, 0);
    BOOST_CHECK(!IsBlockValueValid(nHeight, nExpectedRet, nBlockReward+propAmt+1, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward);
    BOOST_CHECK_EQUAL(nBudgetAmtRet, 0);
}

static CScript GetRandomP2PKH()
{
    CKey key;
    key.MakeNewKey(false);
    return GetScriptForDestination(key.GetPubKey().GetID());
}

static CMutableTransaction NewCoinBase(int nHeight, CAmount cbaseAmt, const CScript& cbaseScript)
{
    CMutableTransaction tx;
    tx.vout.emplace_back(cbaseAmt, cbaseScript);
    tx.vin.emplace_back();
    tx.vin[0].scriptSig = CScript() << nHeight << OP_0;
    return tx;
}

BOOST_AUTO_TEST_CASE(IsCoinbaseValueValid_test)
{
    const CAmount mnAmt = GetMasternodePayment();
    const CScript& cbaseScript = GetRandomP2PKH();
    CValidationState state;

    // force mnsync complete
    masternodeSync.RequestedMasternodeAssets = MASTERNODE_SYNC_FINISHED;

    // -- Regular blocks

    // Exact
    CMutableTransaction cbase = NewCoinBase(1, mnAmt, cbaseScript);
    BOOST_CHECK(IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state));
    cbase.vout[0].nValue /= 2;
    cbase.vout.emplace_back(cbase.vout[0]);
    BOOST_CHECK(IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state));

    // Underpaying with SPORK_8 disabled (good)
    cbase.vout.clear();
    cbase.vout.emplace_back(mnAmt - 1, cbaseScript);
    BOOST_CHECK(IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state));
    cbase.vout[0].nValue = mnAmt/2;
    cbase.vout.emplace_back(cbase.vout[0]);
    cbase.vout[1].nValue = mnAmt/2 - 1;
    BOOST_CHECK(IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state));

    // Overpaying with SPORK_8 disabled
    cbase.vout.clear();
    cbase.vout.emplace_back(mnAmt + 1, cbaseScript);
    BOOST_CHECK(!IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cb-amt-spork8-disabled");
    state = CValidationState();
    cbase.vout[0].nValue = mnAmt/2;
    cbase.vout.emplace_back(cbase.vout[0]);
    cbase.vout[1].nValue = mnAmt/2 + 1;
    BOOST_CHECK(!IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cb-amt-spork8-disabled");
    state = CValidationState();

    // enable SPORK_8
    int64_t nTime = GetTime() - 10;
    const CSporkMessage& spork = CSporkMessage(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT, nTime + 1, nTime);
    sporkManager.AddOrUpdateSporkMessage(spork);
    BOOST_CHECK(sporkManager.IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT));

    // Underpaying with SPORK_8 enabled
    cbase.vout.clear();
    cbase.vout.emplace_back(mnAmt - 1, cbaseScript);
    BOOST_CHECK(!IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cb-amt");
    state = CValidationState();
    cbase.vout[0].nValue = mnAmt/2;
    cbase.vout.emplace_back(cbase.vout[0]);
    cbase.vout[1].nValue = mnAmt/2 - 1;
    BOOST_CHECK(!IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cb-amt");
    state = CValidationState();

    // Overpaying with SPORK_8 enabled
    cbase.vout.clear();
    cbase.vout.emplace_back(mnAmt + 1, cbaseScript);
    BOOST_CHECK(!IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cb-amt");
    state = CValidationState();
    cbase.vout[0].nValue = mnAmt/2;
    cbase.vout.emplace_back(cbase.vout[0]);
    cbase.vout[1].nValue = mnAmt/2 + 1;
    BOOST_CHECK(!IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cb-amt");
    state = CValidationState();

    const CAmount budgAmt = 200 * COIN;

    // -- Superblocks

    // Exact
    cbase.vout.clear();
    cbase.vout.emplace_back(budgAmt, cbaseScript);
    BOOST_CHECK(IsCoinbaseValueValid(MakeTransactionRef(cbase), budgAmt, state));
    cbase.vout[0].nValue /= 2;
    cbase.vout.emplace_back(cbase.vout[0]);
    BOOST_CHECK(IsCoinbaseValueValid(MakeTransactionRef(cbase), budgAmt, state));

    // Underpaying
    cbase.vout.clear();
    cbase.vout.emplace_back(budgAmt - 1, cbaseScript);
    BOOST_CHECK(!IsCoinbaseValueValid(MakeTransactionRef(cbase), budgAmt, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-superblock-cb-amt");
    state = CValidationState();
    cbase.vout[0].nValue = budgAmt/2;
    cbase.vout.emplace_back(cbase.vout[0]);
    cbase.vout[1].nValue = budgAmt/2 - 1;
    BOOST_CHECK(!IsCoinbaseValueValid(MakeTransactionRef(cbase), budgAmt, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-superblock-cb-amt");
    state = CValidationState();

    // Overpaying
    cbase.vout.clear();
    cbase.vout.emplace_back(budgAmt + 1, cbaseScript);
    BOOST_CHECK(!IsCoinbaseValueValid(MakeTransactionRef(cbase), budgAmt, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-superblock-cb-amt");
    state = CValidationState();
    cbase.vout[0].nValue = budgAmt/2;
    cbase.vout.emplace_back(cbase.vout[0]);
    cbase.vout[1].nValue = budgAmt/2 + 1;
    BOOST_CHECK(!IsCoinbaseValueValid(MakeTransactionRef(cbase), budgAmt, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-superblock-cb-amt");
}

BOOST_AUTO_TEST_CASE(fbv_signverify_bls)
{
    CBLSSecretKey sk1, sk2;
    sk1.MakeNewKey();
    sk2.MakeNewKey();
    BOOST_ASSERT(sk1 != sk2);

    CTxIn vin(COutPoint(uint256S("0000000000000000000000000000000000000000000000000000000000000002"), 0));
    CTxIn vin2(COutPoint(uint256S("000000000000000000000000000000000000000000000000000000000000003"), 0));
    CTxIn vin3(COutPoint(uint256S("0000000000000000000000000000000000000000000000000000000000000002"), 1));

    uint256 budgetHash1 = uint256S("0000000000000000000000000000000000000000000000000000000000000001");
    uint256 budgetHash2 = uint256S("0000000000000000000000000000000000010000000000000000000000000001");

    // Create serialized finalbudgetvote for budgetHash1, signed with sk1
    CFinalizedBudgetVote vote(vin, budgetHash1);
    BOOST_CHECK(vote.Sign(sk1));
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << vote;

    // Verify received message on pk1
    CFinalizedBudgetVote _vote;
    ss >> _vote;
    BOOST_CHECK(_vote.CheckSignature(sk1.GetPublicKey()));

    // Failing verification on pk2
    BOOST_CHECK(!_vote.CheckSignature(sk2.GetPublicKey()));

    std::vector<unsigned char> sig = _vote.GetVchSig();

    // Failing with different time
    CFinalizedBudgetVote vote1(_vote);
    vote1.SetTime(vote1.GetTime()+1);
    BOOST_CHECK(!vote1.CheckSignature(sk1.GetPublicKey()));

    // Failing with different budget hash
    CFinalizedBudgetVote vote2(vin, budgetHash2);
    vote2.SetTime(_vote.GetTime());
    vote2.SetVchSig(sig);
    BOOST_CHECK(!vote2.CheckSignature(sk1.GetPublicKey()));

    // Failing with different vins: different txid (vin2) or voutn (vin3)
    CFinalizedBudgetVote vote3_1(vin, budgetHash1);
    CFinalizedBudgetVote vote3_2(vin2, budgetHash1);
    CFinalizedBudgetVote vote3_3(vin3, budgetHash1);
    vote3_1.SetTime(_vote.GetTime());
    vote3_2.SetTime(_vote.GetTime());
    vote3_3.SetTime(_vote.GetTime());
    vote3_1.SetVchSig(sig);
    vote3_2.SetVchSig(sig);
    vote3_3.SetVchSig(sig);
    BOOST_CHECK(vote3_1.CheckSignature(sk1.GetPublicKey()));
    BOOST_CHECK(!vote3_2.CheckSignature(sk1.GetPublicKey()));
    BOOST_CHECK(!vote3_3.CheckSignature(sk1.GetPublicKey()));
}

BOOST_AUTO_TEST_SUITE_END()
