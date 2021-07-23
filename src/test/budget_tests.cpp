// Copyright (c) 2018-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test_pivx.h"

#include "budget/budgetmanager.h"
#include "masternode-payments.h"
#include "masternode-sync.h"
#include "spork.h"
#include "test/util/blocksutil.h"
#include "tinyformat.h"
#include "utilmoneystr.h"
#include "validation.h"

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(budget_tests)

void CheckBudgetValue(int nHeight, std::string strNetwork, CAmount nExpectedValue)
{
    CBudgetManager budget;
    CAmount nBudget = g_budgetman.GetTotalBudget(nHeight);
    std::string strError = strprintf("Budget is not as expected for %s. Result: %s, Expected: %s", strNetwork, FormatMoney(nBudget), FormatMoney(nExpectedValue));
    BOOST_CHECK_MESSAGE(nBudget == nExpectedValue, strError);
}

void enableMnSyncAndSuperblocksPayment()
{
    // force mnsync complete
    masternodeSync.RequestedMasternodeAssets = MASTERNODE_SYNC_FINISHED;

    // enable SPORK_13
    int64_t nTime = GetTime() - 10;
    CSporkMessage spork(SPORK_13_ENABLE_SUPERBLOCKS, nTime + 1, nTime);
    sporkManager.AddOrUpdateSporkMessage(spork);
    BOOST_CHECK(sporkManager.IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS));

    spork = CSporkMessage(SPORK_9_MASTERNODE_BUDGET_ENFORCEMENT, nTime + 1, nTime);
    sporkManager.AddOrUpdateSporkMessage(spork);
    BOOST_CHECK(sporkManager.IsSporkActive(SPORK_9_MASTERNODE_BUDGET_ENFORCEMENT));
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

BOOST_FIXTURE_TEST_CASE(block_value, TestnetSetup)
{
    enableMnSyncAndSuperblocksPayment();
    // regular block
    int nHeight = 100; std::string strError;
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
    const CSporkMessage& spork2 = CSporkMessage(SPORK_13_ENABLE_SUPERBLOCKS, 4070908800ULL, GetTime());
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


/**
 * 1) Create two proposals and two budget finalizations with a different proposal payment order:
         BudA pays propA and propB, BudB pays propB and propA.
   2) Vote both finalization budgets, adding more votes to budA (so it becomes the most voted one).
 */
void forceAddFakeProposals(const CTxOut& payee1, const CTxOut& payee2)
{
    const CTxIn mnVin(GetRandHash(), 0);
    const uint256& propHash = GetRandHash(), finTxId = GetRandHash();
    const CTxBudgetPayment txBudgetPayment(propHash, payee1.scriptPubKey, payee1.nValue);

    const CTxIn mnVin2(GetRandHash(), 0);
    const uint256& propHash2 = GetRandHash(), finTxId2 = GetRandHash();
    const CTxBudgetPayment txBudgetPayment2(propHash2, payee2.scriptPubKey, payee2.nValue);

    // Create first finalization
    CFinalizedBudget fin("main (test)", 144, {txBudgetPayment, txBudgetPayment2}, finTxId);
    const CFinalizedBudgetVote fvote(mnVin, fin.GetHash());
    const CFinalizedBudgetVote fvote1_a({GetRandHash(), 0}, fin.GetHash());
    const CFinalizedBudgetVote fvote1_b({GetRandHash(), 0}, fin.GetHash());
    std::string strError;
    BOOST_CHECK(fin.AddOrUpdateVote(fvote, strError));
    BOOST_CHECK(fin.AddOrUpdateVote(fvote1_a, strError));
    BOOST_CHECK(fin.AddOrUpdateVote(fvote1_b, strError));
    g_budgetman.ForceAddFinalizedBudget(fin.GetHash(), fin.GetFeeTXHash(), fin);

    // Create second finalization
    CFinalizedBudget fin2("main2 (test)", 144, {txBudgetPayment2, txBudgetPayment}, finTxId2);
    const CFinalizedBudgetVote fvote2(mnVin2, fin2.GetHash());
    const CFinalizedBudgetVote fvote2_a({GetRandHash(), 0}, fin2.GetHash());
    BOOST_CHECK(fin2.AddOrUpdateVote(fvote2, strError));
    BOOST_CHECK(fin2.AddOrUpdateVote(fvote2_a, strError));
    g_budgetman.ForceAddFinalizedBudget(fin2.GetHash(), fin2.GetFeeTXHash(), fin2);
}

BOOST_FIXTURE_TEST_CASE(budget_blocks_payee_test, TestChain100Setup)
{
    // Regtest superblock is every 144 blocks.
    for (int i=0; i<43; i++) CreateAndProcessBlock({}, coinbaseKey);
    enableMnSyncAndSuperblocksPayment();
    g_budgetman.Clear();
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Height();), 143);
    BOOST_ASSERT(g_budgetman.GetFinalizedBudgets().size() == 0);

    // Now we are at the superblock height, let's add a proposal to pay.
    const CScript payee1 = GetScriptForDestination(CKeyID(uint160(ParseHex("816115944e077fe7c803cfa57f29b36bf87c1d35"))));
    const CAmount propAmt1 = 100 * COIN;
    const CScript payee2 = GetScriptForDestination(CKeyID(uint160(ParseHex("8d5b4f83212214d6ef693e02e6d71969fddad976"))));
    const CAmount propAmt2 = propAmt1;
    forceAddFakeProposals({propAmt1, payee1}, {propAmt2, payee2});

    CBlock block = CreateBlock({}, coinbaseKey);
    // Check payee validity:
    CTxOut payeeOut = block.vtx[0]->vout[1];
    BOOST_CHECK_EQUAL(payeeOut.nValue, propAmt1);
    BOOST_CHECK(payeeOut.scriptPubKey == payee1);

    // Good tx
    CMutableTransaction goodMtx(*block.vtx[0]);

    // Modify payee
    CMutableTransaction mtx(*block.vtx[0]);
    mtx.vout[1].scriptPubKey = GetScriptForDestination(CKeyID(uint160(ParseHex("8c988f1a4a4de2161e0f50aac7f17e7f9555caa4"))));
    block.vtx[0] = MakeTransactionRef(mtx);
    std::shared_ptr<CBlock> pblock = FinalizeBlock(std::make_shared<CBlock>(block));
    BOOST_CHECK(block.vtx[0]->vout[1].scriptPubKey != payee1);

    // Verify block rejection reason.
    ProcessBlockAndCheckRejectionReason(pblock, "bad-cb-payee", 143);

    // Try to overmint, valid payee --> bad amount.
    mtx = goodMtx; // reset
    mtx.vout[1].nValue *= 2; // invalid amount
    block.vtx[0] = MakeTransactionRef(mtx);
    pblock = FinalizeBlock(std::make_shared<CBlock>(block));
    BOOST_CHECK(block.vtx[0]->vout[1].scriptPubKey == payee1);
    BOOST_CHECK(block.vtx[0]->vout[1].nValue == payeeOut.nValue * 2);
    ProcessBlockAndCheckRejectionReason(pblock, "bad-blk-amount", 143);

    // Try to send less to a valid payee --> bad amount.
    mtx = goodMtx; // reset
    mtx.vout[1].nValue /= 2;
    block.vtx[0] = MakeTransactionRef(mtx);
    pblock = FinalizeBlock(std::make_shared<CBlock>(block));
    BOOST_CHECK(block.vtx[0]->vout[1].scriptPubKey == payee1);
    BOOST_CHECK(block.vtx[0]->vout[1].nValue == payeeOut.nValue / 2);
    ProcessBlockAndCheckRejectionReason(pblock, "bad-cb-payee", 143);

    // Context, this has:
    // 1) Two proposals and two budget finalizations with a different proposal payment order (read `forceAddFakeProposals()` description):
    //      BudA pays propA and propB, BudB pays propB and propA.
    // 2) Voted both budgets, adding more votes to budA (so it becomes the most voted one).
    // 3) Now: in the superblock, pay to budB order (the less voted finalization) --> which will fail.

    // Try to pay proposals in different order
    mtx = goodMtx; // reset
    std::vector<CFinalizedBudget*> vecFin = g_budgetman.GetFinalizedBudgets();
    CFinalizedBudget* secondFin{nullptr};
    for (auto fin : vecFin) {
        if (!secondFin || fin->GetVoteCount() < secondFin->GetVoteCount()) {
            secondFin = fin;
        }
    }
    secondFin->GetPayeeAndAmount(144, mtx.vout[1].scriptPubKey, mtx.vout[1].nValue);
    BOOST_CHECK(mtx.vout[1].scriptPubKey != goodMtx.vout[1].scriptPubKey);
    BOOST_CHECK(mtx.vout[1].nValue == goodMtx.vout[1].nValue);
    block.vtx[0] = MakeTransactionRef(mtx);
    pblock = FinalizeBlock(std::make_shared<CBlock>(block));
    ProcessBlockAndCheckRejectionReason(pblock, "bad-cb-payee", 143);

    // Now create the good block
    block.vtx[0] = MakeTransactionRef(goodMtx);
    pblock = FinalizeBlock(std::make_shared<CBlock>(block));
    CValidationState state;
    ProcessNewBlock(state, pblock, nullptr);
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash();), pblock->GetHash());
}

BOOST_FIXTURE_TEST_CASE(budget_blocks_reorg_test, TestChain100Setup)
{
    // Regtest superblock is every 144 blocks.
    for (int i=0; i<43; i++) CreateAndProcessBlock({}, coinbaseKey);
    enableMnSyncAndSuperblocksPayment();
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Height();), 143);

    // Now we are at the superblock height, let's add a proposal to pay.
    const CScript payee = GetScriptForDestination(CKeyID(uint160(ParseHex("816115944e077fe7c803cfa57f29b36bf87c1d35"))));
    const CAmount propAmt = 100 * COIN;
    const CScript payee2 = GetScriptForDestination(CKeyID(uint160(ParseHex("816115944e077fe7c803cfa57f29b36bf87c1d35"))));
    const CAmount propAmt2 = propAmt * 2;
    forceAddFakeProposals({propAmt, payee}, {propAmt2, payee2});

    // This will:
    // 1) Create a proposal to be paid at block 144 (first superblock).
    // 1) create blocksA and blockB at block 144 (paying for the proposal).
    // 2) Process and connect blockA.
    // 3) Create blockC on top of BlockA and blockD on top of blockB. At height 145.
    // 4) Process and connect blockC.
    // 5) Now force the reorg:
    //    a) Process blockB and blockD.
    //    b) Create and process blockE on top of blockD.
    // 6) Verify that tip is at blockE.

    CScript forkCoinbaseScript = GetScriptForDestination(CKeyID(uint160(ParseHex("8c988f1a4a4de2161e0f50aac7f17e7f9555caa4"))));
    CBlock blockA = CreateBlock({}, coinbaseKey, false);
    CBlock blockB = CreateBlock({}, forkCoinbaseScript, false);
    BOOST_CHECK(blockA.GetHash() != blockB.GetHash());
    // Check blocks payee validity:
    CTxOut payeeOut = blockA.vtx[0]->vout[1];
    BOOST_CHECK_EQUAL(payeeOut.nValue, propAmt);
    BOOST_CHECK(payeeOut.scriptPubKey == payee);
    payeeOut = blockB.vtx[0]->vout[1];
    BOOST_CHECK_EQUAL(payeeOut.nValue, propAmt);
    BOOST_CHECK(payeeOut.scriptPubKey == payee);

    // Now let's process BlockA:
    CValidationState stateA;
    auto pblockA = std::make_shared<const CBlock>(blockA);
    ProcessNewBlock(stateA, pblockA, nullptr);
    BOOST_CHECK(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash()) == blockA.GetHash());

    // Now let's create blockC on top of BlockA, blockD on top of blockB
    // and process blockC to expand the chain.
    CBlock blockC = CreateBlock({}, coinbaseKey, false);
    BOOST_CHECK(blockC.hashPrevBlock == blockA.GetHash());
    CBlock blockD = CreateBlock({}, forkCoinbaseScript, false);

    // Process and connect blockC
    ProcessNewBlock(stateA, std::make_shared<const CBlock>(blockC), nullptr);
    BOOST_CHECK(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash()) == blockC.GetHash());

    // Now let's process the secondary chain
    blockD.hashPrevBlock = blockB.GetHash();
    std::shared_ptr<CBlock> pblockD = FinalizeBlock(std::make_shared<CBlock>(blockD));

    CValidationState stateB;
    ProcessNewBlock(stateB, std::make_shared<const CBlock>(blockB), nullptr);
    ProcessNewBlock(stateB, pblockD, nullptr);
    CBlock blockE = CreateBlock({}, forkCoinbaseScript, false);
    blockE.hashPrevBlock = pblockD->GetHash();
    std::shared_ptr<CBlock> pblockE = FinalizeBlock(std::make_shared<CBlock>(blockE));
    ProcessNewBlock(stateB, pblockE, nullptr);
    BOOST_CHECK(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash()) == pblockE->GetHash());
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

BOOST_FIXTURE_TEST_CASE(IsCoinbaseValueValid_test, TestingSetup)
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

BOOST_AUTO_TEST_SUITE_END()
