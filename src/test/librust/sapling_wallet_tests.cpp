// Copyright (c) 2016-2020 The ZCash developers
// Copyright (c) 2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "wallet/test/wallet_test_fixture.h"

#include <sodium.h>

#include "base58.h"
#include "chainparams.h"
#include "key_io.h"
#include "validation.h"
#include "optional.h"
#include "primitives/block.h"
#include "random.h"
#include "sapling/transaction_builder.h"
#include "test/librust/utiltest.h"
#include "wallet/wallet.h"
#include "consensus/merkle.h"
#include "sapling/note.h"
#include "sapling/noteencryption.h"

#include <boost/filesystem.hpp>

#include <boost/test/unit_test.hpp>

void setupWallet(CWallet& wallet)
{
    wallet.SetMinVersion(FEATURE_SAPLING);
    wallet.SetupSPKM(false);
}

std::vector<SaplingOutPoint> SetSaplingNoteData(CWalletTx& wtx) {
    mapSaplingNoteData_t saplingNoteData;
    SaplingOutPoint saplingOutPoint = {wtx.GetHash(), 0};
    SaplingNoteData saplingNd;
    // set this as internal note (with non-nullopt ivk)
    saplingNd.ivk = libzcash::SaplingIncomingViewingKey();
    saplingNoteData[saplingOutPoint] = saplingNd;
    wtx.SetSaplingNoteData(saplingNoteData);
    std::vector<SaplingOutPoint> saplingNotes {saplingOutPoint};
    return saplingNotes;
}

SaplingOutPoint CreateValidBlock(CWallet& wallet,
                                libzcash::SaplingExtendedSpendingKey& sk,
                                const CBlockIndex& index,
                                CBlock& block,
                                SaplingMerkleTree& saplingTree)
{
    CWalletTx wtx = GetValidSaplingReceive(Params().GetConsensus(),
                                           wallet, sk, 0, true);
    auto saplingNotes = SetSaplingNoteData(wtx);
    wallet.LoadToWallet(wtx);

    block.vtx.emplace_back(wtx.tx);
    wallet.IncrementNoteWitnesses(&index, &block, saplingTree);

    return saplingNotes[0];
}

uint256 GetWitnessesAndAnchors(CWallet& wallet,
                               const std::vector<SaplingOutPoint>& saplingNotes,
                               std::vector<Optional<SaplingWitness>>& saplingWitnesses)
{
    saplingWitnesses.clear();
    uint256 saplingAnchor;
    wallet.GetSaplingScriptPubKeyMan()->GetSaplingNoteWitnesses(saplingNotes, saplingWitnesses, saplingAnchor);
    return saplingAnchor;
}

BOOST_FIXTURE_TEST_SUITE(sapling_wallet_tests, WalletRegTestingSetup)

BOOST_AUTO_TEST_CASE(SetSaplingNoteAddrsInCWalletTx) {
    auto consensusParams = Params().GetConsensus();

    CWallet& wallet = m_wallet;
    LOCK(wallet.cs_wallet);
    setupWallet(wallet);

    auto sk = GetTestMasterSaplingSpendingKey();
    auto expsk = sk.expsk;
    auto fvk = expsk.full_viewing_key();
    auto ivk = fvk.in_viewing_key();
    auto pk = sk.DefaultAddress();

    libzcash::SaplingNote note(pk, 50000000);
    auto cm = note.cmu().get();
    SaplingMerkleTree tree;
    tree.append(cm);
    auto anchor = tree.root();
    auto witness = tree.witness();

    Optional<uint256> nf = note.nullifier(fvk, witness.position());
    BOOST_CHECK(nf != boost::none);
    uint256 nullifier = nf.get();

    auto builder = TransactionBuilder(consensusParams, 1);
    builder.AddSaplingSpend(expsk, note, anchor, witness);
    builder.AddSaplingOutput(fvk.ovk, pk, 40000000, {});
    builder.SetFee(10000000);
    auto tx = builder.Build().GetTxOrThrow();

    CWalletTx wtx {&wallet, MakeTransactionRef(tx)};

    BOOST_CHECK_EQUAL(0, wtx.mapSaplingNoteData.size());
    mapSaplingNoteData_t noteData;

    SaplingOutPoint op {wtx.GetHash(), 0};
    SaplingNoteData nd;
    nd.nullifier = nullifier;
    nd.ivk = ivk;
    nd.witnesses.push_front(witness);
    nd.witnessHeight = 123;
    noteData.insert(std::make_pair(op, nd));

    wtx.SetSaplingNoteData(noteData);
    BOOST_CHECK(noteData == wtx.mapSaplingNoteData);

    // Test individual fields in case equality operator is defined/changed.
    BOOST_CHECK(wtx.mapSaplingNoteData[op].IsMyNote());
    BOOST_CHECK(ivk == *(wtx.mapSaplingNoteData[op].ivk));
    BOOST_CHECK(nullifier == wtx.mapSaplingNoteData[op].nullifier);
    BOOST_CHECK(nd.witnessHeight == wtx.mapSaplingNoteData[op].witnessHeight);
    BOOST_CHECK(witness == wtx.mapSaplingNoteData[op].witnesses.front());
}

// Cannot add note data for an index which does not exist in tx.vShieldedOutput
BOOST_AUTO_TEST_CASE(SetInvalidSaplingNoteDataInCWalletTx) {
    CWalletTx wtx(nullptr /* pwallet */, MakeTransactionRef());
    BOOST_CHECK_EQUAL(0, wtx.mapSaplingNoteData.size());

    mapSaplingNoteData_t noteData;
    SaplingOutPoint op {uint256(), 1};
    SaplingNoteData nd;
    noteData.insert(std::make_pair(op, nd));

    BOOST_CHECK_THROW(wtx.SetSaplingNoteData(noteData), std::logic_error);
}

BOOST_AUTO_TEST_CASE(FindMySaplingNotes)
{
    auto consensusParams = Params().GetConsensus();

    CWallet& wallet = m_wallet;
    LOCK(wallet.cs_wallet);
    wallet.SetupSPKM(false);

    // Generate dummy Sapling address
    auto sk = GetTestMasterSaplingSpendingKey();
    auto expsk = sk.expsk;
    auto extfvk = sk.ToXFVK();
    auto pa = sk.DefaultAddress();

    auto testNote = GetTestSaplingNote(pa, 50000000);

    // Generate transaction
    auto builder = TransactionBuilder(consensusParams, 1);
    builder.AddSaplingSpend(expsk, testNote.note, testNote.tree.root(), testNote.tree.witness());
    builder.AddSaplingOutput(extfvk.fvk.ovk, pa, 25000000, {});
    builder.SetFee(10000000);
    auto tx = builder.Build().GetTxOrThrow();

    // No Sapling notes can be found in tx which does not belong to the wallet
    CWalletTx wtx {&wallet, MakeTransactionRef(tx)};
    BOOST_CHECK(!wallet.HaveSaplingSpendingKey(extfvk));
    auto noteMap = wallet.GetSaplingScriptPubKeyMan()->FindMySaplingNotes(*wtx.tx).first;
    BOOST_CHECK_EQUAL(0, noteMap.size());

    // Add spending key to wallet, so Sapling notes can be found
    BOOST_CHECK(wallet.AddSaplingZKey(sk));
    BOOST_CHECK(wallet.HaveSaplingSpendingKey(extfvk));
    noteMap = wallet.GetSaplingScriptPubKeyMan()->FindMySaplingNotes(*wtx.tx).first;
    BOOST_CHECK_EQUAL(2, noteMap.size());
}

// Generate note A and spend to create note B, from which we spend to create two conflicting transactions
BOOST_AUTO_TEST_CASE(GetConflictedSaplingNotes)
{
    auto consensusParams = Params().GetConsensus();

    CWallet& wallet = m_wallet;
    LOCK2(cs_main, wallet.cs_wallet);
    setupWallet(wallet);

    // Generate Sapling address
    auto sk = GetTestMasterSaplingSpendingKey();
    auto expsk = sk.expsk;
    auto extfvk = sk.ToXFVK();
    auto ivk = extfvk.fvk.in_viewing_key();
    auto pk = sk.DefaultAddress();

    BOOST_CHECK(wallet.AddSaplingZKey(sk));
    BOOST_CHECK(wallet.HaveSaplingSpendingKey(extfvk));

    // Generate note A
    libzcash::SaplingNote note(pk, 50000000);
    auto cm = note.cmu().get();
    SaplingMerkleTree saplingTree;
    saplingTree.append(cm);
    auto anchor = saplingTree.root();
    auto witness = saplingTree.witness();

    // Generate tx to create output note B
    auto builder = TransactionBuilder(consensusParams, 1);
    builder.AddSaplingSpend(expsk, note, anchor, witness);
    builder.AddSaplingOutput(extfvk.fvk.ovk, pk, 35000000, {});
    builder.SetFee(10000000);
    auto tx = builder.Build().GetTxOrThrow();
    CWalletTx wtx {&wallet, MakeTransactionRef(tx)};

    // Fake-mine the transaction
    BOOST_CHECK_EQUAL(0, chainActive.Height());
    CBlock block;
    block.hashPrevBlock = chainActive[0]->GetBlockHash();
    block.vtx.emplace_back(wtx.tx);
    block.hashMerkleRoot = BlockMerkleRoot(block);
    const auto& blockHash = block.GetHash();
    CBlockIndex fakeIndex {block};
    BlockMap::iterator mi = mapBlockIndex.emplace(blockHash, &fakeIndex).first;
    fakeIndex.phashBlock = &((*mi).first);
    chainActive.SetTip(&fakeIndex);
    BOOST_CHECK(chainActive.Contains(&fakeIndex));
    BOOST_CHECK_EQUAL(0, chainActive.Height());

    // Simulate SyncTransaction which calls AddToWalletIfInvolvingMe
    auto saplingNoteData = wallet.GetSaplingScriptPubKeyMan()->FindMySaplingNotes(*wtx.tx).first;
    BOOST_CHECK(saplingNoteData.size() > 0);
    wtx.SetSaplingNoteData(saplingNoteData);
    wtx.m_confirm = CWalletTx::Confirmation(CWalletTx::Status::CONFIRMED, fakeIndex.nHeight, block.GetHash(), 0);
    BOOST_CHECK(wallet.LoadToWallet(wtx));

    // Simulate receiving new block and ChainTip signal
    wallet.IncrementNoteWitnesses(&fakeIndex, &block, saplingTree);
    wallet.GetSaplingScriptPubKeyMan()->UpdateSaplingNullifierNoteMapForBlock(&block);

    // Retrieve the updated wtx from wallet
    const uint256& hash = wtx.GetHash();
    wtx = wallet.mapWallet.at(hash);

    // Decrypt output note B
    auto maybe_pt = libzcash::SaplingNotePlaintext::decrypt(
            wtx.tx->sapData->vShieldedOutput[0].encCiphertext,
            ivk,
            wtx.tx->sapData->vShieldedOutput[0].ephemeralKey,
            wtx.tx->sapData->vShieldedOutput[0].cmu);
    BOOST_CHECK(static_cast<bool>(maybe_pt) == true);
    auto maybe_note = maybe_pt.get().note(ivk);
    BOOST_CHECK(static_cast<bool>(maybe_note) == true);
    auto note2 = maybe_note.get();

    SaplingOutPoint sop0(wtx.GetHash(), 0);
    auto spend_note_witness =  wtx.mapSaplingNoteData[sop0].witnesses.front();
    auto maybe_nf = note2.nullifier(extfvk.fvk, spend_note_witness.position());
    BOOST_CHECK(static_cast<bool>(maybe_nf) == true);

    anchor = saplingTree.root();

    // Create transaction to spend note B
    auto builder2 = TransactionBuilder(consensusParams, 2);
    builder2.SetFee(10000000);
    builder2.AddSaplingSpend(expsk, note2, anchor, spend_note_witness);
    builder2.AddSaplingOutput(extfvk.fvk.ovk, pk, 20000000, {});
    auto tx2 = builder2.Build().GetTxOrThrow();

    // Create conflicting transaction which also spends note B
    auto builder3 = TransactionBuilder(consensusParams, 2);
    builder3.SetFee(10000000);
    builder3.AddSaplingSpend(expsk, note2, anchor, spend_note_witness);
    builder3.AddSaplingOutput(extfvk.fvk.ovk, pk, 19999000, {});
    auto tx3 = builder3.Build().GetTxOrThrow();

    CWalletTx wtx2 {&wallet, MakeTransactionRef(tx2)};
    CWalletTx wtx3 {&wallet, MakeTransactionRef(tx3)};

    const auto& hash2 = wtx2.GetHash();
    const auto& hash3 = wtx3.GetHash();

    // No conflicts for no spends (wtx is currently the only transaction in the wallet)
    BOOST_CHECK_EQUAL(0, wallet.GetConflicts(hash2).size());
    BOOST_CHECK_EQUAL(0, wallet.GetConflicts(hash3).size());

    // No conflicts for one spend
    BOOST_CHECK(wallet.LoadToWallet(wtx2));
    BOOST_CHECK_EQUAL(0, wallet.GetConflicts(hash2).size());

    // Conflicts for two spends
    BOOST_CHECK(wallet.LoadToWallet(wtx3));
    auto c3 = wallet.GetConflicts(hash2);
    BOOST_CHECK_EQUAL(2, c3.size());
    BOOST_CHECK(std::set<uint256>({hash2, hash3}) == c3);

    // Tear down
    chainActive.SetTip(nullptr);
    mapBlockIndex.erase(blockHash);
}

BOOST_AUTO_TEST_CASE(SaplingNullifierIsSpent)
{
    auto consensusParams = Params().GetConsensus();

    CWallet& wallet = m_wallet;
    LOCK2(cs_main, wallet.cs_wallet);
    setupWallet(wallet);

    // Generate dummy Sapling address
    auto sk = GetTestMasterSaplingSpendingKey();
    auto expsk = sk.expsk;
    auto extfvk = sk.ToXFVK();
    auto pa = sk.DefaultAddress();

    auto testNote = GetTestSaplingNote(pa, 50000000);

    // Generate transaction
    auto builder = TransactionBuilder(consensusParams, 1);
    builder.AddSaplingSpend(expsk,  testNote.note, testNote.tree.root(), testNote.tree.witness());
    builder.AddSaplingOutput(extfvk.fvk.ovk, pa, 2500000, {});
    builder.SetFee(10000000);
    auto tx = builder.Build().GetTxOrThrow();

    CWalletTx wtx {&wallet, MakeTransactionRef(tx)};
    BOOST_CHECK(wallet.AddSaplingZKey(sk));
    BOOST_CHECK(wallet.HaveSaplingSpendingKey(extfvk));

    // Manually compute the nullifier based on the known position
    auto nf = testNote.note.nullifier(extfvk.fvk, testNote.tree.witness().position());
    BOOST_CHECK(nf);
    uint256 nullifier = nf.get();

    // Verify note has not been spent
    BOOST_CHECK(!wallet.GetSaplingScriptPubKeyMan()->IsSaplingSpent(nullifier));

    // Fake-mine the transaction
    BOOST_CHECK_EQUAL(0, chainActive.Height());
    CBlock block;
    block.hashPrevBlock = chainActive[0]->GetBlockHash();
    block.vtx.emplace_back(wtx.tx);
    block.hashMerkleRoot = BlockMerkleRoot(block);
    const auto& blockHash = block.GetHash();
    CBlockIndex fakeIndex {block};
    BlockMap::iterator mi = mapBlockIndex.emplace(blockHash, &fakeIndex).first;
    fakeIndex.phashBlock = &((*mi).first);
    chainActive.SetTip(&fakeIndex);
    BOOST_CHECK(chainActive.Contains(&fakeIndex));
    BOOST_CHECK_EQUAL(0, chainActive.Height());

    wallet.BlockConnected(std::make_shared<CBlock>(block), mi->second);

    // Verify note has been spent
    BOOST_CHECK(wallet.GetSaplingScriptPubKeyMan()->IsSaplingSpent(nullifier));

    // Tear down
    chainActive.SetTip(nullptr);
    mapBlockIndex.erase(blockHash);
}

BOOST_AUTO_TEST_CASE(NavigateFromSaplingNullifierToNote)
{
    auto consensusParams = Params().GetConsensus();

    CWallet& wallet = m_wallet;
    LOCK2(cs_main, wallet.cs_wallet);
    setupWallet(wallet);

    // Generate dummy Sapling address
    auto sk = GetTestMasterSaplingSpendingKey();
    auto expsk = sk.expsk;
    auto extfvk = sk.ToXFVK();
    auto pa = sk.DefaultAddress();

    auto testNote = GetTestSaplingNote(pa, 50000000);

    // Generate transaction
    auto builder = TransactionBuilder(consensusParams, 1);
    builder.AddSaplingSpend(expsk, testNote.note, testNote.tree.root(), testNote.tree.witness());
    builder.AddSaplingOutput(extfvk.fvk.ovk, pa, 25000000, {});
    builder.SetFee(10000000);
    auto tx = builder.Build().GetTxOrThrow();

    CWalletTx wtx {&wallet, MakeTransactionRef(tx)};
    BOOST_CHECK(wallet.AddSaplingZKey(sk));
    BOOST_CHECK(wallet.HaveSaplingSpendingKey(extfvk));

    // Manually compute the nullifier based on the expected position
    auto nf = testNote.note.nullifier(extfvk.fvk, testNote.tree.witness().position());
    BOOST_CHECK(nf);
    uint256 nullifier = nf.get();

    // Verify dummy note is unspent
    BOOST_CHECK(!wallet.GetSaplingScriptPubKeyMan()->IsSaplingSpent(nullifier));

    // Fake-mine the transaction
    BOOST_CHECK_EQUAL(0, chainActive.Height());
    CBlock block;
    block.hashPrevBlock = chainActive[0]->GetBlockHash();
    block.vtx.emplace_back(wtx.tx);
    block.hashMerkleRoot = BlockMerkleRoot(block);
    const auto& blockHash = block.GetHash();
    CBlockIndex fakeIndex {block};
    BlockMap::iterator mi = mapBlockIndex.emplace(blockHash, &fakeIndex).first;
    fakeIndex.phashBlock = &((*mi).first);
    chainActive.SetTip(&fakeIndex);
    BOOST_CHECK(chainActive.Contains(&fakeIndex));
    BOOST_CHECK_EQUAL(0, chainActive.Height());

    // Simulate SyncTransaction which calls AddToWalletIfInvolvingMe
    wtx.m_confirm = CWalletTx::Confirmation(CWalletTx::Status::CONFIRMED, fakeIndex.nHeight, block.GetHash(), 0);
    auto saplingNoteData = wallet.GetSaplingScriptPubKeyMan()->FindMySaplingNotes(*wtx.tx).first;
    BOOST_CHECK(saplingNoteData.size() > 0);
    wtx.SetSaplingNoteData(saplingNoteData);
    wallet.LoadToWallet(wtx);

    // Verify dummy note is now spent, as AddToWallet invokes AddToSpends()
    wallet.SetLastBlockProcessed(mi->second);
    BOOST_CHECK(wallet.GetSaplingScriptPubKeyMan()->IsSaplingSpent(nullifier));

    // Test invariant: no witnesses means no nullifier.
    BOOST_CHECK_EQUAL(0, wallet.GetSaplingScriptPubKeyMan()->mapSaplingNullifiersToNotes.size());
    for (mapSaplingNoteData_t::value_type &item : wtx.mapSaplingNoteData) {
        SaplingNoteData nd = item.second;
        BOOST_CHECK(nd.witnesses.empty());
        BOOST_CHECK(!nd.nullifier);
    }

    // Simulate receiving new block and ChainTip signal
    wallet.IncrementNoteWitnesses(&fakeIndex, &block, testNote.tree);
    wallet.GetSaplingScriptPubKeyMan()->UpdateSaplingNullifierNoteMapForBlock(&block);

    // Retrieve the updated wtx from wallet
    const uint256& hash = wtx.GetHash();
    wtx = wallet.mapWallet.at(hash);

    // Verify Sapling nullifiers map to SaplingOutPoints
    BOOST_CHECK_EQUAL(2, wallet.GetSaplingScriptPubKeyMan()->mapSaplingNullifiersToNotes.size());
    for (mapSaplingNoteData_t::value_type &item : wtx.mapSaplingNoteData) {
        SaplingOutPoint op = item.first;
        SaplingNoteData nd = item.second;
        BOOST_CHECK(hash == op.hash);
        BOOST_CHECK_EQUAL(1, nd.witnesses.size());
        BOOST_CHECK(nd.nullifier);
        auto nf = nd.nullifier.get();
        BOOST_CHECK_EQUAL(1, wallet.GetSaplingScriptPubKeyMan()->mapSaplingNullifiersToNotes.count(nf));
        BOOST_CHECK(op.hash == wallet.GetSaplingScriptPubKeyMan()->mapSaplingNullifiersToNotes[nf].hash);
        BOOST_CHECK_EQUAL(op.n, wallet.GetSaplingScriptPubKeyMan()->mapSaplingNullifiersToNotes[nf].n);
    }

    // Tear down
    chainActive.SetTip(nullptr);
    mapBlockIndex.erase(blockHash);
}

// Create note A, spend A to create note B, spend and verify note B is from me.
BOOST_AUTO_TEST_CASE(SpentSaplingNoteIsFromMe)
{
    auto consensusParams = Params().GetConsensus();

    CWallet& wallet = m_wallet;
    LOCK2(cs_main, wallet.cs_wallet);
    setupWallet(wallet);

    // Generate Sapling address
    auto sk = GetTestMasterSaplingSpendingKey();
    auto expsk = sk.expsk;
    auto extfvk = sk.ToXFVK();
    auto ivk = extfvk.fvk.in_viewing_key();
    auto pk = sk.DefaultAddress();

    // Generate Sapling note A
    libzcash::SaplingNote note(pk, 50000000);
    auto cm = note.cmu().get();
    SaplingMerkleTree saplingTree;
    saplingTree.append(cm);
    auto anchor = saplingTree.root();
    auto witness = saplingTree.witness();

    // Generate transaction, which sends funds to note B
    auto builder = TransactionBuilder(consensusParams, 1);
    builder.AddSaplingSpend(expsk, note, anchor, witness);
    builder.AddSaplingOutput(extfvk.fvk.ovk, pk, 25000000, {});
    builder.SetFee(10000000);
    auto tx = builder.Build().GetTxOrThrow();

    CWalletTx wtx {&wallet, MakeTransactionRef(tx)};
    BOOST_CHECK(wallet.AddSaplingZKey(sk));
    BOOST_CHECK(wallet.HaveSaplingSpendingKey(extfvk));

    // Fake-mine the transaction
    BOOST_CHECK_EQUAL(0, chainActive.Height());
    CBlock block;
    block.hashPrevBlock = chainActive[0]->GetBlockHash();
    block.vtx.emplace_back(wtx.tx);
    block.hashMerkleRoot = BlockMerkleRoot(block);
    const auto& blockHash = block.GetHash();
    CBlockIndex fakeIndex {block};
    BlockMap::iterator mi = mapBlockIndex.emplace(blockHash, &fakeIndex).first;
    fakeIndex.phashBlock = &((*mi).first);
    chainActive.SetTip(&fakeIndex);
    BOOST_CHECK(chainActive.Contains(&fakeIndex));
    BOOST_CHECK_EQUAL(0, chainActive.Height());

    auto saplingNoteData = wallet.GetSaplingScriptPubKeyMan()->FindMySaplingNotes(*wtx.tx).first;
    BOOST_CHECK(saplingNoteData.size() > 0);
    wtx.SetSaplingNoteData(saplingNoteData);
    wtx.m_confirm = CWalletTx::Confirmation(CWalletTx::Status::CONFIRMED, fakeIndex.nHeight, block.GetHash(), 0);
    wallet.LoadToWallet(wtx);

    // Simulate receiving new block and ChainTip signal.
    // This triggers calculation of nullifiers for notes belonging to this wallet
    // in the output descriptions of wtx.
    wallet.IncrementNoteWitnesses(&fakeIndex, &block, saplingTree);
    wallet.GetSaplingScriptPubKeyMan()->UpdateSaplingNullifierNoteMapForBlock(&block);
    wallet.SetLastBlockProcessed(mi->second);

    // Retrieve the updated wtx from wallet
    wtx = wallet.mapWallet.at(wtx.GetHash());

    // The test wallet never received the fake note which is being spent, so there
    // is no mapping from nullifier to notedata stored in mapSaplingNullifiersToNotes.
    // Therefore the wallet does not know the tx belongs to the wallet.
    BOOST_CHECK(!wallet.IsFromMe(wtx.tx));

    // Manually compute the nullifier and check map entry does not exist
    auto nf = note.nullifier(extfvk.fvk, witness.position());
    BOOST_CHECK(nf);
    BOOST_CHECK(!wallet.GetSaplingScriptPubKeyMan()->mapSaplingNullifiersToNotes.count(nf.get()));

    // Decrypt note B
    auto maybe_pt = libzcash::SaplingNotePlaintext::decrypt(
            wtx.tx->sapData->vShieldedOutput[0].encCiphertext,
            ivk,
            wtx.tx->sapData->vShieldedOutput[0].ephemeralKey,
            wtx.tx->sapData->vShieldedOutput[0].cmu);
    BOOST_CHECK_EQUAL(static_cast<bool>(maybe_pt), true);
    auto maybe_note = maybe_pt.get().note(ivk);
    BOOST_CHECK_EQUAL(static_cast<bool>(maybe_note), true);
    auto note2 = maybe_note.get();

    // Get witness to retrieve position of note B we want to spend
    SaplingOutPoint sop0(wtx.GetHash(), 0);
    auto spend_note_witness =  wtx.mapSaplingNoteData[sop0].witnesses.front();
    auto maybe_nf = note2.nullifier(extfvk.fvk, spend_note_witness.position());
    BOOST_CHECK_EQUAL(static_cast<bool>(maybe_nf), true);
    auto nullifier2 = maybe_nf.get();

    // NOTE: Not updating the anchor results in a core dump.  Shouldn't builder just return error?
    // *** Error in `./zcash-gtest': double free or corruption (out): 0x00007ffd8755d990 ***
    anchor = saplingTree.root();

    // Create transaction to spend note B
    auto builder2 = TransactionBuilder(consensusParams, 2);
    builder2.AddSaplingSpend(expsk, note2, anchor, spend_note_witness);
    builder2.AddSaplingOutput(extfvk.fvk.ovk, pk, 12500000, {});
    builder2.SetFee(10000000);
    auto tx2 = builder2.Build().GetTxOrThrow();
    BOOST_CHECK_EQUAL(tx2.vin.size(), 0);
    BOOST_CHECK_EQUAL(tx2.vout.size(), 0);
    BOOST_CHECK_EQUAL(tx2.sapData->vShieldedSpend.size(), 1);
    BOOST_CHECK_EQUAL(tx2.sapData->vShieldedOutput.size(), 2);
    BOOST_CHECK_EQUAL(tx2.sapData->valueBalance, 10000000);

    CWalletTx wtx2 {&wallet, MakeTransactionRef(tx2)};

    // Fake-mine this tx into the next block
    BOOST_CHECK_EQUAL(0, chainActive.Height());
    CBlock block2;
    block2.vtx.emplace_back(wtx2.tx);
    block.hashMerkleRoot = BlockMerkleRoot(block);
    block2.hashPrevBlock = blockHash;
    auto blockHash2 = block2.GetHash();
    CBlockIndex fakeIndex2 {block2};
    BlockMap::iterator mi2 = mapBlockIndex.emplace(blockHash2, &fakeIndex2).first;
    fakeIndex2.phashBlock = &((*mi2).first);
    fakeIndex2.nHeight = 1;
    chainActive.SetTip(&fakeIndex2);
    BOOST_CHECK(chainActive.Contains(&fakeIndex2));
    BOOST_CHECK_EQUAL(1, chainActive.Height());

    auto saplingNoteData2 = wallet.GetSaplingScriptPubKeyMan()->FindMySaplingNotes(*wtx2.tx).first;
    BOOST_CHECK(saplingNoteData2.size() > 0);
    wtx2.SetSaplingNoteData(saplingNoteData2);
    wtx2.m_confirm = CWalletTx::Confirmation(CWalletTx::Status::CONFIRMED, fakeIndex2.nHeight, block2.GetHash(), 0);
    wallet.LoadToWallet(wtx2);
    wallet.SetLastBlockProcessed(mi2->second);

    // Verify note B is spent. AddToWallet invokes AddToSpends which updates mapTxSaplingNullifiers
    BOOST_CHECK(wallet.GetSaplingScriptPubKeyMan()->IsSaplingSpent(nullifier2));

    // Verify note B belongs to wallet.
    BOOST_CHECK(wallet.IsFromMe(wtx2.tx));
    BOOST_CHECK(wallet.GetSaplingScriptPubKeyMan()->mapSaplingNullifiersToNotes.count(nullifier2));

    // Tear down
    chainActive.SetTip(nullptr);
    mapBlockIndex.erase(blockHash);
    mapBlockIndex.erase(blockHash2);
}

BOOST_AUTO_TEST_CASE(CachedWitnessesEmptyChain)
{
    auto consensusParams = Params().GetConsensus();

    CWallet& wallet = m_wallet;
    {
        LOCK(wallet.cs_wallet);
        setupWallet(wallet);
    }

    auto sk = GetTestMasterSaplingSpendingKey();
    CWalletTx wtx = GetValidSaplingReceive(Params().GetConsensus(), wallet, sk, 10, true);

    std::vector<SaplingOutPoint> saplingNotes = SetSaplingNoteData(wtx);
    std::vector<Optional<SaplingWitness>> saplingWitnesses;

    ::GetWitnessesAndAnchors(wallet, saplingNotes, saplingWitnesses);

    BOOST_CHECK(!(bool) saplingWitnesses[0]);

    wallet.LoadToWallet(wtx);

    ::GetWitnessesAndAnchors(wallet, saplingNotes, saplingWitnesses);

    BOOST_CHECK(!(bool) saplingWitnesses[0]);

    CBlock block;
    block.vtx.emplace_back(wtx.tx);
    CBlockIndex index(block);
    SaplingMerkleTree saplingTree;
    wallet.IncrementNoteWitnesses(&index, &block, saplingTree);

    ::GetWitnessesAndAnchors(wallet, saplingNotes, saplingWitnesses);

    BOOST_CHECK((bool) saplingWitnesses[0]);

    // Until zcash#1302 is implemented, this should triggger an assertion
    BOOST_CHECK_THROW(wallet.DecrementNoteWitnesses(&index), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(CachedWitnessesChainTip)
{
    auto consensusParams = Params().GetConsensus();

    libzcash::SaplingExtendedSpendingKey sk = GetTestMasterSaplingSpendingKey();
    CWallet& wallet = m_wallet;
    {
        LOCK(wallet.cs_wallet);
        setupWallet(wallet);
        BOOST_CHECK(wallet.AddSaplingZKey(sk));
        BOOST_CHECK(wallet.HaveSaplingSpendingKey(sk.ToXFVK()));
    }

    uint256 anchors1;
    CBlock block1;
    SaplingMerkleTree saplingTree;

    {
        // First block (case tested in _empty_chain)
        CBlockIndex index1(block1);
        index1.nHeight = 1;
        auto output = CreateValidBlock(wallet, sk, index1, block1, saplingTree);

        // Called to fetch anchor
        std::vector<SaplingOutPoint> saplingNotes {output};
        std::vector<Optional<SaplingWitness>> saplingWitnesses;

        anchors1 = GetWitnessesAndAnchors(wallet, saplingNotes, saplingWitnesses);
    }

    {
        // Second transaction
        CWalletTx wtx = GetValidSaplingReceive(Params().GetConsensus(),
                wallet, sk, 50, true);

        std::vector<SaplingOutPoint> saplingNotes = SetSaplingNoteData(wtx);
        wallet.LoadToWallet(wtx);

        std::vector<Optional<SaplingWitness>> saplingWitnesses;
        GetWitnessesAndAnchors(wallet , saplingNotes, saplingWitnesses);

        BOOST_CHECK(!(bool) saplingWitnesses[0]);

        // Second block
        CBlock block2;
        block2.hashPrevBlock = block1.GetHash();
        block2.vtx.emplace_back(wtx.tx);
        CBlockIndex index2(block2);
        index2.nHeight = 2;
        SaplingMerkleTree saplingTree2 {saplingTree};
        wallet.IncrementNoteWitnesses(&index2, &block2, saplingTree2);

        auto anchors2 = GetWitnessesAndAnchors(wallet, saplingNotes, saplingWitnesses);
        BOOST_CHECK((bool) saplingWitnesses[0]);
        BOOST_CHECK(anchors1 != anchors2);

        // Decrementing should give us the previous anchor
        wallet.DecrementNoteWitnesses(&index2);
        auto anchors3 = GetWitnessesAndAnchors(wallet, saplingNotes, saplingWitnesses);

        BOOST_CHECK(!(bool) saplingWitnesses[0]);
        // Should not equal first anchor because none of these notes had witnesses
        BOOST_CHECK(anchors1 != anchors3);

        // Re-incrementing with the same block should give the same result
        wallet.IncrementNoteWitnesses(&index2, &block2, saplingTree);
        auto anchors4 = GetWitnessesAndAnchors(wallet, saplingNotes, saplingWitnesses);
        BOOST_CHECK((bool) saplingWitnesses[0]);
        BOOST_CHECK(anchors2 == anchors4);

        // Incrementing with the same block again should not change the cache
        wallet.IncrementNoteWitnesses(&index2, &block2, saplingTree);
        std::vector<Optional<SaplingWitness>> saplingWitnesses5;

        auto anchors5 = GetWitnessesAndAnchors(wallet, saplingNotes, saplingWitnesses5);
        BOOST_CHECK(saplingWitnesses == saplingWitnesses5);
        BOOST_CHECK(anchors4 == anchors5);
    }
}

BOOST_AUTO_TEST_CASE(CachedWitnessesDecrementFirst)
{
    auto consensusParams = Params().GetConsensus();
    libzcash::SaplingExtendedSpendingKey sk = GetTestMasterSaplingSpendingKey();
    CWallet& wallet = m_wallet;
    {
        LOCK(wallet.cs_wallet);
        setupWallet(wallet);
        BOOST_CHECK(wallet.AddSaplingZKey(sk));
    }

    SaplingMerkleTree saplingTree;
    {
        // First block (case tested in _empty_chain)
        CBlock block1;
        CBlockIndex index1(block1);
        index1.nHeight = 1;
        CreateValidBlock(wallet, sk, index1, block1, saplingTree);
    }

    uint256 anchors2;
    CBlock block2;
    CBlockIndex index2(block2);

    {
        // Second block (case tested in _chain_tip)
        index2.nHeight = 2;
        auto output = CreateValidBlock(wallet, sk, index2, block2, saplingTree);

        // Called to fetch anchor
        std::vector<SaplingOutPoint> saplingNotes {output};
        std::vector<Optional<SaplingWitness>> saplingWitnesses;
        anchors2 = GetWitnessesAndAnchors(wallet, saplingNotes, saplingWitnesses);
    }

    {
        // Third transaction - never mined
        CWalletTx wtx = GetValidSaplingReceive(Params().GetConsensus(),
                                               wallet, sk, 20, true);
        std::vector<SaplingOutPoint> saplingNotes = SetSaplingNoteData(wtx);
        wallet.LoadToWallet(wtx);

        std::vector<Optional<SaplingWitness>> saplingWitnesses;

        auto anchors3 = GetWitnessesAndAnchors(wallet, saplingNotes, saplingWitnesses);

        BOOST_CHECK(!(bool) saplingWitnesses[0]);

        // Decrementing (before the transaction has ever seen an increment)
        // should give us the previous anchor
        wallet.DecrementNoteWitnesses(&index2);

        auto anchors4 = GetWitnessesAndAnchors(wallet, saplingNotes, saplingWitnesses);

        BOOST_CHECK(!(bool) saplingWitnesses[0]);
        // Should not equal second anchor because none of these notes had witnesses
        BOOST_CHECK(anchors2 != anchors4);

        // Re-incrementing with the same block should give the same result
        wallet.IncrementNoteWitnesses(&index2, &block2, saplingTree);

        auto anchors5 = GetWitnessesAndAnchors(wallet, saplingNotes, saplingWitnesses);

        BOOST_CHECK(!(bool) saplingWitnesses[0]);
        BOOST_CHECK(anchors3 == anchors5);
    }
}

BOOST_AUTO_TEST_CASE(CachedWitnessesCleanIndex)
{
    auto consensusParams = Params().GetConsensus();

    libzcash::SaplingExtendedSpendingKey sk = GetTestMasterSaplingSpendingKey();
    CWallet& wallet = m_wallet;
    {
        LOCK(wallet.cs_wallet);
        setupWallet(wallet);
        BOOST_CHECK(wallet.AddSaplingZKey(sk));
    }

    std::vector<CBlock> blocks;
    std::vector<CBlockIndex> indices;
    std::vector<SaplingOutPoint> saplingNotes;
    std::vector<uint256> saplingAnchors;
    SaplingMerkleTree saplingTree;
    SaplingMerkleTree saplingRiTree = saplingTree;
    std::vector<Optional<SaplingWitness>> saplingWitnesses;

    // Generate a chain
    size_t numBlocks = WITNESS_CACHE_SIZE + 10;
    blocks.resize(numBlocks);
    indices.resize(numBlocks);
    for (size_t i = 0; i < numBlocks; i++) {
        indices[i].nHeight = i;
        auto oldSaplingRoot = saplingTree.root();
        auto outpts = CreateValidBlock(wallet, sk, indices[i], blocks[i], saplingTree);
        BOOST_CHECK(oldSaplingRoot != saplingTree.root());
        saplingNotes.push_back(outpts);

        auto anchor = GetWitnessesAndAnchors(wallet, saplingNotes, saplingWitnesses);
        for (size_t j = 0; j <= i; j++) {
            BOOST_CHECK((bool) saplingWitnesses[j]);
        }
        saplingAnchors.push_back(anchor);
    }

    // Now pretend we are reindexing: the chain is cleared, and each block is
    // used to increment witnesses again.
    for (size_t i = 0; i < numBlocks; i++) {
        SaplingMerkleTree saplingRiPrevTree {saplingRiTree};
        wallet.IncrementNoteWitnesses(&(indices[i]), &(blocks[i]), saplingRiTree);

        auto anchors = GetWitnessesAndAnchors(wallet, saplingNotes, saplingWitnesses);
        for (size_t j = 0; j < numBlocks; j++) {
            BOOST_CHECK((bool) saplingWitnesses[j]);
        }
        // Should equal final anchor because witness cache unaffected
        BOOST_CHECK(saplingAnchors.back() == anchors);

        if ((i == 5) || (i == 50)) {
            // Pretend a reorg happened that was recorded in the block files
            {
                wallet.DecrementNoteWitnesses(&(indices[i]));

                auto anchors = GetWitnessesAndAnchors(wallet, saplingNotes, saplingWitnesses);
                for (size_t j = 0; j < numBlocks; j++) {
                    BOOST_CHECK((bool) saplingWitnesses[j]);
                }
                // Should equal final anchor because witness cache unaffected
                BOOST_CHECK(saplingAnchors.back() == anchors);
            }

            {
                wallet.IncrementNoteWitnesses(&(indices[i]), &(blocks[i]), saplingRiPrevTree);
                auto anchors = GetWitnessesAndAnchors(wallet, saplingNotes, saplingWitnesses);
                for (size_t j = 0; j < numBlocks; j++) {
                    BOOST_CHECK((bool) saplingWitnesses[j]);
                }
                // Should equal final anchor because witness cache unaffected
                BOOST_CHECK(saplingAnchors.back() == anchors);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(ClearNoteWitnessCache)
{
    auto consensusParams = Params().GetConsensus();

    libzcash::SaplingExtendedSpendingKey sk = GetTestMasterSaplingSpendingKey();
    CWallet& wallet = m_wallet;
    {
        LOCK(wallet.cs_wallet);
        setupWallet(wallet);
        BOOST_CHECK(wallet.AddSaplingZKey(sk));
    }

    CWalletTx wtx = GetValidSaplingReceive(Params().GetConsensus(),
                                           wallet, sk, 10, true);
    auto hash = wtx.GetHash();
    auto saplingNotes = SetSaplingNoteData(wtx);

    // Pretend we mined the tx by adding a fake witness
    SaplingMerkleTree saplingTree;
    wtx.mapSaplingNoteData[saplingNotes[0]].witnesses.push_front(saplingTree.witness());
    wtx.mapSaplingNoteData[saplingNotes[0]].witnessHeight = 1;
    wallet.GetSaplingScriptPubKeyMan()->nWitnessCacheSize = 1;

    wallet.LoadToWallet(wtx);

    // SetSaplingNoteData() only created a single Sapling output
    // which is in the wallet, so we add a second SaplingOutPoint here to
    // exercise the "note not in wallet" case.
    saplingNotes.emplace_back(wtx.GetHash(), 1);
    BOOST_CHECK_EQUAL(saplingNotes.size(), 2);

    std::vector<Optional<SaplingWitness>> saplingWitnesses;

    // Before clearing, we should have a witness for one note
    GetWitnessesAndAnchors(wallet, saplingNotes, saplingWitnesses);
    BOOST_CHECK((bool) saplingWitnesses[0]);
    BOOST_CHECK(!(bool) saplingWitnesses[1]);
    BOOST_CHECK_EQUAL(1, wallet.mapWallet.at(hash).mapSaplingNoteData[saplingNotes[0]].witnessHeight);
    BOOST_CHECK_EQUAL(1, wallet.GetSaplingScriptPubKeyMan()->nWitnessCacheSize);

    // After clearing, we should not have a witness for either note
    wallet.GetSaplingScriptPubKeyMan()->ClearNoteWitnessCache();
    GetWitnessesAndAnchors(wallet, saplingNotes, saplingWitnesses);
    BOOST_CHECK(!(bool) saplingWitnesses[0]);
    BOOST_CHECK(!(bool) saplingWitnesses[1]);
    BOOST_CHECK_EQUAL(-1, wallet.mapWallet.at(hash).mapSaplingNoteData[saplingNotes[0]].witnessHeight);
    BOOST_CHECK_EQUAL(0, wallet.GetSaplingScriptPubKeyMan()->nWitnessCacheSize);
}

BOOST_AUTO_TEST_CASE(UpdatedSaplingNoteData)
{
    auto consensusParams = Params().GetConsensus();

    CWallet& wallet = m_wallet;
    // Need to lock cs_main for now due the lock ordering. future: revamp all of this function to only lock where is needed.
    LOCK2(cs_main, wallet.cs_wallet);
    setupWallet(wallet);

    auto m = GetTestMasterSaplingSpendingKey();

    // Generate dummy Sapling address
    auto sk = m.Derive(0);
    auto expsk = sk.expsk;
    auto extfvk = sk.ToXFVK();
    auto pa = sk.DefaultAddress();

    // Generate dummy recipient Sapling address
    auto sk2 = m.Derive(1);
    auto extfvk2 = sk2.ToXFVK();
    auto pa2 = sk2.DefaultAddress();

    auto testNote = GetTestSaplingNote(pa, 50000000);

    // Generate transaction
    auto builder = TransactionBuilder(consensusParams, 1);
    builder.AddSaplingSpend(expsk, testNote.note, testNote.tree.root(), testNote.tree.witness());
    builder.AddSaplingOutput(extfvk.fvk.ovk, pa2, 25000000, {});
    builder.SetFee(10000000);
    auto tx = builder.Build().GetTxOrThrow();

    // Wallet contains extfvk1 but not extfvk2
    CWalletTx wtx {&wallet, MakeTransactionRef(tx)};
    BOOST_CHECK(wallet.AddSaplingZKey(sk));
    BOOST_CHECK(wallet.HaveSaplingSpendingKey(extfvk));
    BOOST_CHECK(!wallet.HaveSaplingSpendingKey(extfvk2));

    // Fake-mine the transaction
    BOOST_CHECK_EQUAL(0, chainActive.Height());
    CBlock block;
    block.vtx.emplace_back(wtx.tx);
    block.hashMerkleRoot = BlockMerkleRoot(block);
    const auto& blockHash = block.GetHash();
    CBlockIndex fakeIndex {block};
    BlockMap::iterator mi = mapBlockIndex.emplace(blockHash, &fakeIndex).first;
    fakeIndex.phashBlock = &((*mi).first);
    chainActive.SetTip(&fakeIndex);
    BOOST_CHECK(chainActive.Contains(&fakeIndex));
    BOOST_CHECK_EQUAL(0, chainActive.Height());

    // Simulate SyncTransaction which calls AddToWalletIfInvolvingMe
    auto saplingNoteData = wallet.GetSaplingScriptPubKeyMan()->FindMySaplingNotes(*wtx.tx).first;
    BOOST_CHECK(saplingNoteData.size() == 1); // wallet only has key for change output
    wtx.SetSaplingNoteData(saplingNoteData);
    wtx.m_confirm = CWalletTx::Confirmation(CWalletTx::Status::CONFIRMED, fakeIndex.nHeight, block.GetHash(), 0);
    wallet.LoadToWallet(wtx);

    // Simulate receiving new block and ChainTip signal
    wallet.IncrementNoteWitnesses(&fakeIndex, &block, testNote.tree);
    wallet.GetSaplingScriptPubKeyMan()->UpdateSaplingNullifierNoteMapForBlock(&block);

    // Retrieve the updated wtx from wallet
    const uint256& hash = wtx.GetHash();
    wtx = wallet.mapWallet.at(hash);

    // Now lets add key extfvk2 so wallet can find the payment note sent to pa2
    BOOST_CHECK(wallet.AddSaplingZKey(sk2));
    BOOST_CHECK(wallet.HaveSaplingSpendingKey(extfvk2));
    CWalletTx wtx2 = wtx;
    auto saplingNoteData2 = wallet.GetSaplingScriptPubKeyMan()->FindMySaplingNotes(*wtx2.tx).first;
    BOOST_CHECK(saplingNoteData2.size() == 2);
    wtx2.SetSaplingNoteData(saplingNoteData2);

    // The payment note has not been witnessed yet, so let's fake the witness.
    SaplingOutPoint sop0(wtx2.GetHash(), 0);
    SaplingOutPoint sop1(wtx2.GetHash(), 1);
    wtx2.mapSaplingNoteData[sop0].witnesses.push_front(testNote.tree.witness());
    wtx2.mapSaplingNoteData[sop0].witnessHeight = 0;

    // The txs are different as wtx is aware of just the change output,
    // whereas wtx2 is aware of both payment and change outputs.
    BOOST_CHECK(wtx.mapSaplingNoteData != wtx2.mapSaplingNoteData);
    BOOST_CHECK_EQUAL(1, wtx.mapSaplingNoteData.size());
    BOOST_CHECK_EQUAL(1, wtx.mapSaplingNoteData[sop1].witnesses.size());    // wtx has witness for change

    BOOST_CHECK_EQUAL(2, wtx2.mapSaplingNoteData.size());
    BOOST_CHECK_EQUAL(1, wtx2.mapSaplingNoteData[sop0].witnesses.size());    // wtx2 has fake witness for payment output
    BOOST_CHECK_EQUAL(0, wtx2.mapSaplingNoteData[sop1].witnesses.size());    // wtx2 never had incrementnotewitness called

    // After updating, they should be the same
    BOOST_CHECK(wallet.GetSaplingScriptPubKeyMan()->UpdatedNoteData(wtx2, wtx));

    // We can't do this:
    // BOOST_CHECK_EQUAL(wtx.mapSaplingNoteData, wtx2.mapSaplingNoteData);
    // because nullifiers (if part of == comparator) have not all been computed
    // Also note that mapwallet[hash] is not updated with the updated wtx.
    // wtx = wallet.mapWallet[hash];

    BOOST_CHECK_EQUAL(2, wtx.mapSaplingNoteData.size());
    BOOST_CHECK_EQUAL(2, wtx2.mapSaplingNoteData.size());
    // wtx copied over the fake witness from wtx2 for the payment output
    BOOST_CHECK(wtx.mapSaplingNoteData[sop0].witnesses.front() == wtx2.mapSaplingNoteData[sop0].witnesses.front());
    // wtx2 never had its change output witnessed even though it has been in wtx
    BOOST_CHECK_EQUAL(0, wtx2.mapSaplingNoteData[sop1].witnesses.size());
    BOOST_CHECK(wtx.mapSaplingNoteData[sop1].witnesses.front() == testNote.tree.witness());

    // Tear down
    chainActive.SetTip(nullptr);
    mapBlockIndex.erase(blockHash);
}

BOOST_AUTO_TEST_CASE(MarkAffectedSaplingTransactionsDirty)
{
    auto consensusParams = Params().GetConsensus();

    CWallet& wallet = m_wallet;
    LOCK2(cs_main, wallet.cs_wallet);
    setupWallet(wallet);

    // Generate Sapling address
    auto sk = GetTestMasterSaplingSpendingKey();
    auto expsk = sk.expsk;
    auto extfvk = sk.ToXFVK();
    auto ivk = extfvk.fvk.in_viewing_key();
    auto pk = sk.DefaultAddress();

    BOOST_CHECK(wallet.AddSaplingZKey(sk));
    BOOST_CHECK(wallet.HaveSaplingSpendingKey(extfvk));

    // Set up transparent address
    CBasicKeyStore keystore;
    CKey tsk = AddTestCKeyToKeyStore(keystore);
    auto scriptPubKey = GetScriptForDestination(tsk.GetPubKey().GetID());

    // Generate shielding tx from transparent to Sapling
    // 0.5 t-PIV in, 0.4 z-PIV out, 0.1 t-PIV fee
    auto builder = TransactionBuilder(consensusParams, 1, &keystore);
    builder.AddTransparentInput(COutPoint(), scriptPubKey, 50000000);
    builder.AddSaplingOutput(extfvk.fvk.ovk, pk, 40000000, {});
    builder.SetFee(10000000);
    auto tx1 = builder.Build().GetTxOrThrow();

    BOOST_CHECK_EQUAL(tx1.vin.size(), 1);
    BOOST_CHECK_EQUAL(tx1.vout.size(), 0);
    BOOST_CHECK_EQUAL(tx1.sapData->vShieldedSpend.size(), 0);
    BOOST_CHECK_EQUAL(tx1.sapData->vShieldedOutput.size(), 1);
    BOOST_CHECK_EQUAL(tx1.sapData->valueBalance, -40000000);

    CWalletTx wtx {&wallet, MakeTransactionRef(tx1)};

    // Fake-mine the transaction
    BOOST_CHECK_EQUAL(0, chainActive.Height());
    SaplingMerkleTree saplingTree;
    CBlock block;
    block.vtx.emplace_back(wtx.tx);
    block.hashMerkleRoot = BlockMerkleRoot(block);
    const auto& blockHash = block.GetHash();
    CBlockIndex fakeIndex {block};
    BlockMap::iterator mi = mapBlockIndex.emplace(blockHash, &fakeIndex).first;
    fakeIndex.phashBlock = &((*mi).first);
    chainActive.SetTip(&fakeIndex);
    BOOST_CHECK(chainActive.Contains(&fakeIndex));
    BOOST_CHECK_EQUAL(0, chainActive.Height());

    // Simulate SyncTransaction which calls AddToWalletIfInvolvingMe
    auto saplingNoteData = wallet.GetSaplingScriptPubKeyMan()->FindMySaplingNotes(*wtx.tx).first;
    BOOST_CHECK(saplingNoteData.size() > 0);
    wtx.SetSaplingNoteData(saplingNoteData);
    wtx.m_confirm = CWalletTx::Confirmation(CWalletTx::Status::CONFIRMED, fakeIndex.nHeight, block.GetHash(), 0);
    wallet.LoadToWallet(wtx);

    // Simulate receiving new block and ChainTip signal
    wallet.IncrementNoteWitnesses(&fakeIndex, &block, saplingTree);
    wallet.GetSaplingScriptPubKeyMan()->UpdateSaplingNullifierNoteMapForBlock(&block);

    // Retrieve the updated wtx from wallet
    const uint256& hash = wtx.GetHash();
    wtx = wallet.mapWallet.at(hash);

    // Prepare to spend the note that was just created
    auto maybe_pt = libzcash::SaplingNotePlaintext::decrypt(
            tx1.sapData->vShieldedOutput[0].encCiphertext, ivk, tx1.sapData->vShieldedOutput[0].ephemeralKey, tx1.sapData->vShieldedOutput[0].cmu);
    BOOST_CHECK(static_cast<bool>(maybe_pt));
    auto maybe_note = maybe_pt.get().note(ivk);
    BOOST_CHECK(static_cast<bool>(maybe_note));
    auto note = maybe_note.get();
    auto anchor = saplingTree.root();
    auto witness = saplingTree.witness();

    // Create a Sapling-only transaction
    // 0.4 z-PIV in, 0.25 z-PIV out, 0.1 t-PIV fee, 0.05 z-PIV change
    auto builder2 = TransactionBuilder(consensusParams, 2);
    builder2.AddSaplingSpend(expsk, note, anchor, witness);
    builder2.AddSaplingOutput(extfvk.fvk.ovk, pk, 25000000, {});
    builder2.SetFee(10000000);
    auto tx2 = builder2.Build().GetTxOrThrow();

    BOOST_CHECK_EQUAL(tx2.vin.size(), 0);
    BOOST_CHECK_EQUAL(tx2.vout.size(), 0);
    BOOST_CHECK_EQUAL(tx2.sapData->vShieldedSpend.size(), 1);
    BOOST_CHECK_EQUAL(tx2.sapData->vShieldedOutput.size(), 2);
    BOOST_CHECK_EQUAL(tx2.sapData->valueBalance, 10000000);

    CWalletTx wtx2 {&wallet, MakeTransactionRef(tx2)};

    wallet.MarkAffectedTransactionsDirty(*wtx.tx);

    // After getting a cached value, the first tx should be clean
    wallet.mapWallet.at(hash).GetDebit(ISMINE_SPENDABLE);
    BOOST_CHECK(wallet.mapWallet.at(hash).IsAmountCached(CWalletTx::AmountType::DEBIT, ISMINE_SPENDABLE));

    // After adding the note spend, the first tx should be dirty
    wallet.LoadToWallet(wtx2);
    wallet.MarkAffectedTransactionsDirty(*wtx2.tx);
    BOOST_CHECK(!wallet.mapWallet.at(hash).IsAmountCached(CWalletTx::AmountType::DEBIT, ISMINE_SPENDABLE));

    // Tear down
    chainActive.SetTip(nullptr);
    mapBlockIndex.erase(blockHash);
}

BOOST_AUTO_TEST_CASE(GetNotes)
{
    auto consensusParams = Params().GetConsensus();

    CWallet& wallet = m_wallet;
    libzcash::SaplingPaymentAddress pk;
    uint256 blockHash;
    std::vector<SaplingOutPoint> saplingOutpoints;
    {
        LOCK2(cs_main, wallet.cs_wallet);
        setupWallet(wallet);

        // Generate Sapling address
        auto sk = GetTestMasterSaplingSpendingKey();
        auto extfvk = sk.ToXFVK();
        pk = sk.DefaultAddress();

        BOOST_CHECK(wallet.AddSaplingZKey(sk));
        BOOST_CHECK(wallet.HaveSaplingSpendingKey(extfvk));

        // Set up transparent address
        CBasicKeyStore keystore;
        CKey tsk = AddTestCKeyToKeyStore(keystore);
        auto scriptPubKey = GetScriptForDestination(tsk.GetPubKey().GetID());

        // Generate shielding tx from transparent to Sapling (five 1 PIV notes)
        auto builder = TransactionBuilder(consensusParams, 1, &keystore);
        builder.AddTransparentInput(COutPoint(), scriptPubKey, 510000000);
        for (int i=0; i<5; i++) builder.AddSaplingOutput(extfvk.fvk.ovk, pk, 100000000, {});
        builder.SetFee(10000000);
        auto tx1 = builder.Build().GetTxOrThrow();

        CWalletTx wtx {&wallet, MakeTransactionRef(tx1)};

        // Fake-mine the transaction
        BOOST_CHECK_EQUAL(0, chainActive.Height());
        SaplingMerkleTree saplingTree;
        CBlock block;
        block.vtx.emplace_back(wtx.tx);
        block.hashMerkleRoot = BlockMerkleRoot(block);
        blockHash = block.GetHash();
        CBlockIndex fakeIndex {block};
        BlockMap::iterator mi = mapBlockIndex.emplace(blockHash, &fakeIndex).first;
        fakeIndex.phashBlock = &((*mi).first);
        chainActive.SetTip(&fakeIndex);
        BOOST_CHECK(chainActive.Contains(&fakeIndex));
        BOOST_CHECK_EQUAL(0, chainActive.Height());

        // Simulate SyncTransaction which calls AddToWalletIfInvolvingMe
        auto saplingNoteData = wallet.GetSaplingScriptPubKeyMan()->FindMySaplingNotes(*wtx.tx).first;
        BOOST_CHECK(saplingNoteData.size() > 0);
        wtx.SetSaplingNoteData(saplingNoteData);
        wtx.m_confirm = CWalletTx::Confirmation(CWalletTx::Status::CONFIRMED, fakeIndex.nHeight, block.GetHash(), 0);
        wallet.LoadToWallet(wtx);

        // Simulate receiving new block and ChainTip signal
        wallet.IncrementNoteWitnesses(&fakeIndex, &block, saplingTree);
        wallet.GetSaplingScriptPubKeyMan()->UpdateSaplingNullifierNoteMapForBlock(&block);
        wallet.SetLastBlockProcessed(mi->second);

        const uint256& txid = wtx.GetHash();
        for (int i=0; i<5; i++) saplingOutpoints.emplace_back(txid, i);
    }

    // Check GetFilteredNotes
    std::vector<SaplingNoteEntry> entries;
    Optional<libzcash::SaplingPaymentAddress> address = pk;
    wallet.GetSaplingScriptPubKeyMan()->GetFilteredNotes(entries, address, 0, true, false);
    for (int i=0; i<5; i++) {
        BOOST_CHECK(entries[i].op == saplingOutpoints[i]);
        BOOST_CHECK(entries[i].address == pk);
        BOOST_CHECK_EQUAL(entries[i].confirmations, 1);
    }

    // Check GetNotes
    std::vector<SaplingNoteEntry> entries2;
    wallet.GetSaplingScriptPubKeyMan()->GetNotes(saplingOutpoints, entries2);
    for (int i=0; i<5; i++) {
        BOOST_CHECK(entries2[i].op == entries[i].op);
        BOOST_CHECK(entries2[i].address == entries[i].address);
        BOOST_CHECK(entries2[i].note.d == entries[i].note.d);
        BOOST_CHECK_EQUAL(entries2[i].note.pk_d, entries[i].note.pk_d);
        BOOST_CHECK_EQUAL(entries2[i].note.r, entries[i].note.r);
        BOOST_CHECK(entries2[i].memo == entries[i].memo);
        BOOST_CHECK_EQUAL(entries2[i].confirmations, entries[i].confirmations);
    }

    // Tear down
    LOCK(cs_main);
    chainActive.SetTip(nullptr);
    mapBlockIndex.erase(blockHash);
}

// TODO: Back port WriteWitnessCache & SetBestChainIgnoresTxsWithoutShieldedData test cases.

BOOST_AUTO_TEST_SUITE_END()
