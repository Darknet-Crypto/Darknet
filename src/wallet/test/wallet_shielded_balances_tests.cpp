// Copyright (c) 2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "wallet/test/wallet_test_fixture.h"

#include "consensus/merkle.h"
#include "primitives/block.h"
#include "random.h"
#include "sapling/note.h"
#include "sapling/noteencryption.h"
#include "sapling/transaction_builder.h"
#include "test/librust/utiltest.h"
#include "wallet/wallet.h"

#include <boost/filesystem.hpp>

#include <boost/test/unit_test.hpp>

CAmount fee = COIN; // Hardcoded fee

BOOST_FIXTURE_TEST_SUITE(wallet_shielded_balances_tests, WalletTestingSetup)

void setupWallet(CWallet& wallet)
{
    wallet.SetMinVersion(FEATURE_SAPLING);
    wallet.SetupSPKM(false);
}

// Find and set notes data in the tx + add any missing ivk to the wallet's keystore.
CWalletTx& SetWalletNotesData(CWallet* wallet, CWalletTx& wtx)
{
    Optional<mapSaplingNoteData_t> saplingNoteData{nullopt};
    wallet->FindNotesDataAndAddMissingIVKToKeystore(*wtx.tx, saplingNoteData);
    assert(static_cast<bool>(saplingNoteData));
    wtx.SetSaplingNoteData(*saplingNoteData);
    BOOST_CHECK(wallet->AddToWallet(wtx));
    // Updated tx
    return wallet->mapWallet.at(wtx.GetHash());
}

/**
 * Creates and send a tx with an input of 'inputAmount' to 'vDest'.
 */
CWalletTx& AddShieldedBalanceToWallet(CAmount inputAmount,
                                      const std::vector<ShieldedDestination>& vDest,
                                      CWallet* wallet,
                                      const Consensus::Params& consensusParams)
{

    // Dummy wallet, used to generate the dummy transparent input key and sign it in the transaction builder
    CWallet dummyWallet("dummy", CWalletDBWrapper::CreateDummy());
    dummyWallet.SetMinVersion(FEATURE_SAPLING);
    dummyWallet.SetupSPKM(false, true);
    LOCK(dummyWallet.cs_wallet);

    // Create a transaction shielding balance to 'vDest' and load it to the wallet.
    CWalletTx wtx = GetValidSaplingReceive(consensusParams, dummyWallet, inputAmount, vDest, wallet);

    // Updated tx after load it to the wallet
    CWalletTx& wtxUpdated = SetWalletNotesData(wallet, wtx);
    // Check tx credit now
    BOOST_CHECK_EQUAL(wtxUpdated.GetCredit(ISMINE_ALL), inputAmount);
    BOOST_CHECK(wtxUpdated.IsAmountCached(CWalletTx::CREDIT, ISMINE_SPENDABLE_SHIELDED));
    return wtxUpdated;
}

CWalletTx& AddShieldedBalanceToWallet(const libzcash::SaplingPaymentAddress& sendTo, CAmount amount,
                                      CWallet& wallet, const Consensus::Params& consensusParams,
                                      libzcash::SaplingExtendedSpendingKey& extskOut)
{
    // Create a transaction shielding balance to 'sendTo' and load it to the wallet.
    BOOST_CHECK(wallet.GetSaplingExtendedSpendingKey(sendTo, extskOut));
    std::vector<ShieldedDestination> vDest;
    vDest.push_back({extskOut, amount});
    return AddShieldedBalanceToWallet(amount, vDest, &wallet, consensusParams);
}

struct SaplingSpendValues {
    libzcash::SaplingNote note;
    const uint256 anchor;
    const SaplingWitness witness;
};

/**
 * Update the wallet internally as if the wallet would had received a valid block containing wtx.
 * Then return the note, anchor and witness for any subsequent spending process.
 */
SaplingSpendValues UpdateWalletInternalNotesData(CWalletTx& wtx, const SaplingOutPoint& sapPoint, CWallet& wallet)
{
    // Get note
    SaplingNoteData nd = wtx.mapSaplingNoteData.at(sapPoint);
    assert(nd.IsMyNote());
    const auto& ivk = *(nd.ivk);
    auto maybe_pt = libzcash::SaplingNotePlaintext::decrypt(
            wtx.tx->sapData->vShieldedOutput[sapPoint.n].encCiphertext,
            ivk,
            wtx.tx->sapData->vShieldedOutput[sapPoint.n].ephemeralKey,
            wtx.tx->sapData->vShieldedOutput[sapPoint.n].cmu);
    assert(static_cast<bool>(maybe_pt));
    boost::optional<libzcash::SaplingNotePlaintext> notePlainText = maybe_pt.get();
    libzcash::SaplingNote note = notePlainText->note(ivk).get();

    // Append note to the tree
    auto commitment = note.cmu().get();
    SaplingMerkleTree tree;
    tree.append(commitment);
    auto anchor = tree.root();
    auto witness = tree.witness();

    // Update wtx credit chain data
    // Pretend we mined the tx by adding a fake witness and nullifier to be able to spend it.
    wtx.mapSaplingNoteData[sapPoint].witnesses.push_front(tree.witness());
    wtx.mapSaplingNoteData[sapPoint].witnessHeight = 1;
    wallet.GetSaplingScriptPubKeyMan()->nWitnessCacheSize = 1;
    wallet.GetSaplingScriptPubKeyMan()->UpdateSaplingNullifierNoteMapWithTx(wtx);
    return {note, anchor, witness};
}

/**
 * Validates:
 * 1) CWalletTx getCredit for shielded credit.
 * Incoming spendable shielded balance must be cached in the cacheableAmounts.
 *
 * 2) CWalletTx getDebit & getCredit for shielded debit to transparent address.
 * Same wallet as point (1), spending half of the credit received in (1) to a transparent remote address.
 * The other half of the balance - minus fee - must appear as credit (shielded change).
 *
 */
BOOST_AUTO_TEST_CASE(GetShieldedSimpleCachedCreditAndDebit)
{

    ///////////////////////
    //////// Credit ////////
    ///////////////////////

    auto consensusParams = Params().GetConsensus();

    // Main wallet
    CWallet &wallet = m_wallet;
    LOCK2(cs_main, wallet.cs_wallet);
    setupWallet(wallet);

    // First generate a shielded address
    libzcash::SaplingPaymentAddress pa = wallet.GenerateNewSaplingZKey();
    CAmount firstCredit = COIN * 10;

    // Add shielded balance.
    libzcash::SaplingExtendedSpendingKey extskOut;
    CWalletTx& wtxUpdated = AddShieldedBalanceToWallet(pa, firstCredit, wallet, consensusParams, extskOut);

    ///////////////////////
    //////// Debit ////////
    ///////////////////////

    // Update transaction and wallet internal state to be able to spend it.
    SaplingOutPoint sapPoint {wtxUpdated.GetHash(), 0};
    SaplingSpendValues sapSpendValues = UpdateWalletInternalNotesData(wtxUpdated, sapPoint, wallet);

    // Debit value
    CAmount firstDebit = COIN * 5;
    CAmount firstDebitShieldedChange = firstDebit - fee;

    // Create the spending transaction
    auto builder = TransactionBuilder(consensusParams, 1, &wallet);
    builder.SetFee(fee);
    builder.AddSaplingSpend(
            extskOut.expsk,
            sapSpendValues.note,
            sapSpendValues.anchor,
            sapSpendValues.witness);

    // Send to transparent address
    builder.AddTransparentOutput(CreateDummyDestinationScript(),
                                 firstDebit);

    CTransaction tx = builder.Build().GetTxOrThrow();
    // add tx to wallet and update it.
    wallet.AddToWallet({&wallet, MakeTransactionRef(tx)});
    CWalletTx& wtxDebit = wallet.mapWallet.at(tx.GetHash());
    // Update tx notes data (shielded change need it)
    CWalletTx& wtxDebitUpdated = SetWalletNotesData(&wallet, wtxDebit);

    // The debit need to be the entire first note value
    BOOST_CHECK_EQUAL(wtxDebitUpdated.GetDebit(ISMINE_ALL), firstCredit);
    BOOST_CHECK(wtxDebitUpdated.IsAmountCached(CWalletTx::DEBIT, ISMINE_SPENDABLE_SHIELDED));
    // The credit should be only the change.
    BOOST_CHECK_EQUAL(wtxDebitUpdated.GetCredit(ISMINE_ALL), firstDebitShieldedChange);
    BOOST_CHECK(wtxDebitUpdated.IsAmountCached(CWalletTx::CREDIT, ISMINE_SPENDABLE_SHIELDED));

    // Checks that the only shielded output of this tx is change.
    BOOST_CHECK(wallet.GetSaplingScriptPubKeyMan()->IsNoteSaplingChange(
            SaplingOutPoint(wtxDebitUpdated.GetHash(), 0), pa));
}

libzcash::SaplingPaymentAddress getNewDummyShieldedAddress()
{
    HDSeed seed;
    auto m = libzcash::SaplingExtendedSpendingKey::Master(seed);
    return m.DefaultAddress();
}

CWalletTx& buildTxAndLoadToWallet(CWallet& wallet, const libzcash::SaplingExtendedSpendingKey& extskOut,
                                  const SaplingSpendValues& sapSpendValues, libzcash::SaplingPaymentAddress dest,
                                  const CAmount& destAmount, const Consensus::Params& consensus)
{
    // Create the spending transaction
    auto builder = TransactionBuilder(consensus, 1, &wallet);
    builder.SetFee(fee);
    builder.AddSaplingSpend(
            extskOut.expsk,
            sapSpendValues.note,
            sapSpendValues.anchor,
            sapSpendValues.witness);

    // Send to shielded address
    builder.AddSaplingOutput(
            extskOut.expsk.ovk,
            dest,
            destAmount,
            {}
    );

    CTransaction tx = builder.Build().GetTxOrThrow();
    // add tx to wallet and update it.
    wallet.AddToWallet({&wallet, MakeTransactionRef(tx)});
    CWalletTx& wtx = wallet.mapWallet.at(tx.GetHash());
    // Update tx notes data and return the updated wtx.
    return SetWalletNotesData(&wallet, wtx);
}

/**
 * Validates shielded to remote shielded + change cached balances.
 */
BOOST_AUTO_TEST_CASE(VerifyShieldedToRemoteShieldedCachedBalance)
{
    auto consensusParams = Params().GetConsensus();

    // Main wallet
    CWallet &wallet = m_wallet;
    LOCK2(cs_main, wallet.cs_wallet);
    setupWallet(wallet);

    // First generate a shielded address
    libzcash::SaplingPaymentAddress pa = wallet.GenerateNewSaplingZKey();
    CAmount firstCredit = COIN * 20;

    // Add shielded balance.
    libzcash::SaplingExtendedSpendingKey extskOut;
    CWalletTx& wtxUpdated = AddShieldedBalanceToWallet(pa, firstCredit, wallet, consensusParams, extskOut);

    // Update transaction and wallet internal state to be able to spend it.
    SaplingOutPoint sapPoint {wtxUpdated.GetHash(), 0};
    SaplingSpendValues sapSpendValues = UpdateWalletInternalNotesData(wtxUpdated, sapPoint, wallet);

    // Remote destination values
    libzcash::SaplingPaymentAddress destShieldedAddress = getNewDummyShieldedAddress();
    CAmount destAmount = COIN * 8;

    // Create the spending transaction and load it to the wallet
    CWalletTx& wtxDebitUpdated = buildTxAndLoadToWallet(wallet,
                           extskOut,
                           sapSpendValues,
                           destShieldedAddress,
                           destAmount,
                           consensusParams);

    // Validate results
    CAmount expectedShieldedChange = firstCredit - destAmount - fee;

    // The debit need to be the entire first note value
    BOOST_CHECK_EQUAL(wtxDebitUpdated.GetDebit(ISMINE_ALL), firstCredit);
    BOOST_CHECK(wtxDebitUpdated.IsAmountCached(CWalletTx::DEBIT, ISMINE_SPENDABLE_SHIELDED));
    // The credit should be only the change.
    BOOST_CHECK_EQUAL(wtxDebitUpdated.GetCredit(ISMINE_ALL), expectedShieldedChange);
    BOOST_CHECK(wtxDebitUpdated.IsAmountCached(CWalletTx::CREDIT, ISMINE_SPENDABLE_SHIELDED));
    // Plus, change should be same and be cached as well
    BOOST_CHECK_EQUAL(wtxDebitUpdated.GetShieldedChange(), expectedShieldedChange);
    BOOST_CHECK(wtxDebitUpdated.fShieldedChangeCached);
}

struct FakeBlock
{
    CBlock block;
    CBlockIndex* pindex;
};

FakeBlock SimpleFakeMine(CWalletTx& wtx, SaplingMerkleTree& currentTree, CWallet& wallet)
{
    FakeBlock fakeBlock;
    fakeBlock.block.nVersion = CBlock::CURRENT_VERSION;
    fakeBlock.block.vtx.emplace_back(wtx.tx);
    fakeBlock.block.hashMerkleRoot = BlockMerkleRoot(fakeBlock.block);
    for (const OutputDescription& out : wtx.tx->sapData->vShieldedOutput) {
        currentTree.append(out.cmu);
    }
    fakeBlock.block.hashFinalSaplingRoot = currentTree.root();
    fakeBlock.pindex = new CBlockIndex(fakeBlock.block);
    mapBlockIndex.insert(std::make_pair(fakeBlock.block.GetHash(), fakeBlock.pindex));
    fakeBlock.pindex->phashBlock = &mapBlockIndex.find(fakeBlock.block.GetHash())->first;
    chainActive.SetTip(fakeBlock.pindex);
    BOOST_CHECK(chainActive.Contains(fakeBlock.pindex));
    WITH_LOCK(wallet.cs_wallet, wallet.SetLastBlockProcessed(fakeBlock.pindex));
    wtx.m_confirm = CWalletTx::Confirmation(CWalletTx::Status::CONFIRMED, fakeBlock.pindex->nHeight, fakeBlock.pindex->GetBlockHash(), 0);
    return fakeBlock;
}

/**
 * Test:
 * 1) receive two shielded notes on the same tx.
 * 2) check available credit.
 * 3) spend one of them.
 * 4) force available credit cache recalculation and validate the updated amount.
 */
BOOST_AUTO_TEST_CASE(GetShieldedAvailableCredit)
{
    auto consensusParams = Params().GetConsensus();

    // Main wallet
    CWallet &wallet = m_wallet;
    LOCK2(cs_main, wallet.cs_wallet);
    setupWallet(wallet);

    // 1) generate a shielded address and send 20 PIV in two shielded outputs
    libzcash::SaplingPaymentAddress pa = wallet.GenerateNewSaplingZKey();
    CAmount credit = COIN * 20;

    // Add two equal shielded outputs.
    libzcash::SaplingExtendedSpendingKey extskOut;
    BOOST_CHECK(wallet.GetSaplingExtendedSpendingKey(pa, extskOut));

    std::vector<ShieldedDestination> vDest;
    vDest.push_back({extskOut, credit / 2});
    vDest.push_back({extskOut, credit / 2});
    CWalletTx& wtxUpdated = AddShieldedBalanceToWallet(credit, vDest, &wallet, consensusParams);

    // Available credit ISMINE_SPENDABLE must be 0
    // Available credit ISMINE_SHIELDED_SPENDABLE must be 'credit' and be cached.
    BOOST_CHECK_EQUAL(wtxUpdated.GetAvailableCredit(true, ISMINE_SPENDABLE), 0);
    BOOST_CHECK_EQUAL(wtxUpdated.GetShieldedAvailableCredit(), credit);
    BOOST_CHECK(wtxUpdated.IsAmountCached(CWalletTx::AVAILABLE_CREDIT, ISMINE_SPENDABLE_SHIELDED));

    // 2) Confirm the tx
    SaplingMerkleTree tree;
    FakeBlock fakeBlock = SimpleFakeMine(wtxUpdated, tree, wallet);
    // Simulate receiving a new block and updating the witnesses/nullifiers
    wallet.IncrementNoteWitnesses(fakeBlock.pindex, &fakeBlock.block, tree);
    wallet.GetSaplingScriptPubKeyMan()->UpdateSaplingNullifierNoteMapForBlock(&fakeBlock.block);
    wtxUpdated = wallet.mapWallet.at(wtxUpdated.GetHash());

    // 3) Now can spend one output and recalculate the shielded credit.
    std::vector<SaplingNoteEntry> saplingEntries;
    Optional<libzcash::SaplingPaymentAddress> opPa(pa);
    wallet.GetSaplingScriptPubKeyMan()->GetFilteredNotes(saplingEntries,
                                                         opPa,
                                                         0);

    std::vector<SaplingOutPoint> ops = {saplingEntries[0].op};
    uint256 anchor;
    std::vector<boost::optional<SaplingWitness>> witnesses;
    wallet.GetSaplingScriptPubKeyMan()->GetSaplingNoteWitnesses(ops, witnesses, anchor);
    SaplingSpendValues sapSpendValues{saplingEntries[0].note, anchor, *witnesses[0]};

    // Remote destination values
    libzcash::SaplingPaymentAddress destShieldedAddress = getNewDummyShieldedAddress();
    CAmount change = COIN * 1;
    CAmount destAmount = credit / 2 - fee - change; // one note - fee

    // Create the spending transaction and load it to the wallet
    CWalletTx& wtxDebitUpdated = buildTxAndLoadToWallet(wallet,
                                                        extskOut,
                                                        sapSpendValues,
                                                        destShieldedAddress,
                                                        destAmount,
                                                        consensusParams);

    // Check previous credit tx balance being the same and then force a recalculation
    BOOST_CHECK_EQUAL(wtxUpdated.GetShieldedAvailableCredit(), credit);
    BOOST_CHECK_EQUAL(wtxUpdated.GetShieldedAvailableCredit(false), credit / 2);
    BOOST_CHECK_EQUAL(wtxUpdated.GetShieldedChange(), 0);

    // Now check the debit tx
    BOOST_CHECK_EQUAL(wtxDebitUpdated.GetDebit(ISMINE_SPENDABLE_SHIELDED), credit / 2);
    BOOST_CHECK_EQUAL(wtxDebitUpdated.GetShieldedChange(), change);
    BOOST_CHECK_EQUAL(wtxDebitUpdated.GetCredit(ISMINE_SPENDABLE_SHIELDED), change);
}

BOOST_AUTO_TEST_SUITE_END()
