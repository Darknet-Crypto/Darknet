// Copyright (c) 2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.
#include "test/test_pivx.h"

#include "base58.h"
#include "key.h"
#include "policy/policy.h"
#include "wallet/test/wallet_test_fixture.h"
#include "wallet/wallet.h"

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(script_P2CS_tests, WalletTestingSetup)

void CheckValidKeyId(const CTxDestination& dest, const CKeyID& expectedKey)
{
    const CKeyID* keyid = boost::get<CKeyID>(&dest);
    if (keyid) {
        BOOST_CHECK(keyid);
        BOOST_CHECK(*keyid == expectedKey);
    } else {
        BOOST_ERROR("Destination is not a CKeyID");
    }
}

// Goal: check cold staking script keys extraction
BOOST_AUTO_TEST_CASE(extract_cold_staking_destination_keys)
{
    CKey ownerKey;
    ownerKey.MakeNewKey(true);
    CKeyID ownerId = ownerKey.GetPubKey().GetID();
    CKey stakerKey;
    stakerKey.MakeNewKey(true);
    CKeyID stakerId = stakerKey.GetPubKey().GetID();
    CScript script = GetScriptForStakeDelegation(stakerId, ownerId);

    // Check owner
    CTxDestination ownerDest;
    BOOST_CHECK(ExtractDestination(script, ownerDest, false));
    CheckValidKeyId(ownerDest, ownerId);

    // Check staker
    CTxDestination stakerDest;
    BOOST_CHECK(ExtractDestination(script, stakerDest, true));
    CheckValidKeyId(stakerDest, stakerId);

    // Now go with ExtractDestinations.
    txnouttype type;
    int nRequiredRet = -1;
    std::vector<CTxDestination> destVector;
    BOOST_CHECK(ExtractDestinations(script, type, destVector, nRequiredRet));
    BOOST_CHECK(type == TX_COLDSTAKE);
    BOOST_CHECK(nRequiredRet == 2);
    BOOST_CHECK(destVector.size() == 2);
    CheckValidKeyId(destVector[0], stakerId);
    CheckValidKeyId(destVector[1], ownerId);
}

static CScript GetNewP2CS(CKey& stakerKey, CKey& ownerKey, bool fLastOutFree)
{
    stakerKey = DecodeSecret("YNdsth3BsW53DYmCiR12SofWSAt2utXQUSGoin3PekVQCMbzfS7E");
    ownerKey = DecodeSecret("YUo8oW3y8cUQdQxQxCdnUJ4Ww5H7nHBEMwD2bNDpBbuLM59t4rvd");
    return fLastOutFree ? GetScriptForStakeDelegationLOF(stakerKey.GetPubKey().GetID(), ownerKey.GetPubKey().GetID())
                        : GetScriptForStakeDelegation(stakerKey.GetPubKey().GetID(), ownerKey.GetPubKey().GetID());
}

static CScript GetDummyP2CS(const CKeyID& dummyKeyID)
{
    return GetScriptForStakeDelegation(dummyKeyID, dummyKeyID);
}

static CScript GetDummyP2PKH(const CKeyID& dummyKeyID)
{
    return GetScriptForDestination(dummyKeyID);
}

static const CAmount amtIn = 200 * COIN;
static const unsigned int flags = STANDARD_SCRIPT_VERIFY_FLAGS;

static CMutableTransaction CreateNewColdStakeTx(CScript& scriptP2CS, CKey& stakerKey, CKey& ownerKey, bool fLastOutFree)
{
    scriptP2CS = GetNewP2CS(stakerKey, ownerKey, fLastOutFree);

    // Create prev transaction:
    CMutableTransaction txFrom;
    txFrom.vout.resize(1);
    txFrom.vout[0].nValue = amtIn;
    txFrom.vout[0].scriptPubKey = scriptP2CS;

    // Create coldstake
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vout.resize(2);
    tx.vin[0].prevout.n = 0;
    tx.vin[0].prevout.hash = txFrom.GetHash();
    tx.vout[0].nValue = 0;
    tx.vout[0].scriptPubKey.clear();
    tx.vout[1].nValue = amtIn + 2 * COIN;
    tx.vout[1].scriptPubKey = scriptP2CS;

    return tx;
}

void SignColdStake(CMutableTransaction& tx, int nIn, const CScript& prevScript, const CKey& key, bool fStaker)
{
    assert(nIn < (int) tx.vin.size());
    tx.vin[nIn].scriptSig.clear();
    const CTransaction _tx(tx);
    SigVersion sv = _tx.GetRequiredSigVersion();
    const uint256& hash = SignatureHash(prevScript, _tx, nIn, SIGHASH_ALL, amtIn, sv);
    std::vector<unsigned char> vchSig;
    BOOST_CHECK(key.Sign(hash, vchSig));
    vchSig.push_back((unsigned char)SIGHASH_ALL);
    std::vector<unsigned char> selector(1, fStaker ? (int) OP_TRUE : OP_FALSE);
    tx.vin[nIn].scriptSig << vchSig << selector << ToByteVector(key.GetPubKey());
}

static bool CheckP2CSScript(const CScript& scriptSig, const CScript& scriptPubKey, const CMutableTransaction& tx, ScriptError& err)
{
    err = SCRIPT_ERR_OK;
    return VerifyScript(scriptSig, scriptPubKey, flags, MutableTransactionSignatureChecker(&tx, 0, amtIn), tx.GetRequiredSigVersion(), &err);
}

BOOST_AUTO_TEST_CASE(coldstake_lof_script)
{
    CScript scriptP2CS;
    CKey stakerKey, ownerKey;

    // create unsigned coinstake transaction
    CMutableTransaction good_tx = CreateNewColdStakeTx(scriptP2CS, stakerKey, ownerKey, true);

    // sign the input with the staker key
    SignColdStake(good_tx, 0, scriptP2CS, stakerKey, true);

    // check the signature and script
    ScriptError err = SCRIPT_ERR_OK;
    CMutableTransaction tx(good_tx);
    BOOST_CHECK(CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err));

    // pay less than expected
    tx.vout[1].nValue -= 3 * COIN;
    SignColdStake(tx, 0, scriptP2CS, stakerKey, true);
    BOOST_CHECK(!CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_CHECKCOLDSTAKEVERIFY, ScriptErrorString(err));

    // Add another p2cs out
    tx.vout.emplace_back(3 * COIN, scriptP2CS);
    SignColdStake(tx, 0, scriptP2CS, stakerKey, true);
    BOOST_CHECK(CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err));

    const CKey& dummyKey = DecodeSecret("YNdsth3BsW53DYmCiR12SofWSAt2utXQUSGoin3PekVQCMbzfS7E");
    const CKeyID& dummyKeyID = dummyKey.GetPubKey().GetID();
    const CScript& dummyP2PKH = GetDummyP2PKH(dummyKeyID);

    // Add a masternode out
    tx.vout.emplace_back(3 * COIN, dummyP2PKH);
    SignColdStake(tx, 0, scriptP2CS, stakerKey, true);
    BOOST_CHECK(CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err));

    // Transfer more coins to the masternode
    tx.vout[2].nValue -= 3 * COIN;
    tx.vout[3].nValue += 3 * COIN;
    SignColdStake(tx, 0, scriptP2CS, stakerKey, true);
    BOOST_CHECK(!CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_CHECKCOLDSTAKEVERIFY, ScriptErrorString(err));

    // Add two "free" outputs
    tx = good_tx;
    tx.vout[1].nValue -= 3 * COIN;
    tx.vout.emplace_back(3 * COIN, dummyP2PKH);
    tx.vout.emplace_back(3 * COIN, dummyP2PKH);
    SignColdStake(tx, 0, scriptP2CS, stakerKey, true);
    BOOST_CHECK(!CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_CHECKCOLDSTAKEVERIFY, ScriptErrorString(err));
    // -- but the owner can
    SignColdStake(tx, 0, scriptP2CS, ownerKey, false);
    BOOST_CHECK(CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err));

    // Replace with new p2cs
    tx = good_tx;
    tx.vout[1].scriptPubKey = GetDummyP2CS(dummyKeyID);
    SignColdStake(tx, 0, scriptP2CS, stakerKey, true);
    BOOST_CHECK(!CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_CHECKCOLDSTAKEVERIFY, ScriptErrorString(err));
    // -- but the owner can
    SignColdStake(tx, 0, scriptP2CS, ownerKey, false);
    BOOST_CHECK(CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err));

    // Replace with single dummy out
    tx = good_tx;
    tx.vout[1] = CTxOut(COIN, dummyP2PKH);
    SignColdStake(tx, 0, scriptP2CS, stakerKey, true);
    BOOST_CHECK(!CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_CHECKCOLDSTAKEVERIFY, ScriptErrorString(err));
    // -- but the owner can
    SignColdStake(tx, 0, scriptP2CS, ownerKey, false);
    BOOST_CHECK(CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err));
}

BOOST_AUTO_TEST_CASE(coldstake_script)
{
    CScript scriptP2CS;
    CKey stakerKey, ownerKey;

    // create unsigned coinstake transaction
    CMutableTransaction good_tx = CreateNewColdStakeTx(scriptP2CS, stakerKey, ownerKey, false);

    // sign the input with the staker key
    SignColdStake(good_tx, 0, scriptP2CS, stakerKey, true);

    // check the signature and script
    ScriptError err = SCRIPT_ERR_OK;
    CMutableTransaction tx(good_tx);
    BOOST_CHECK(CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err));

    // pay less than expected
    tx.vout[1].nValue -= 3 * COIN;
    SignColdStake(tx, 0, scriptP2CS, stakerKey, true);
    BOOST_CHECK(!CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_CHECKCOLDSTAKEVERIFY, ScriptErrorString(err));

    // Add another p2cs out
    tx.vout.emplace_back(3 * COIN, scriptP2CS);
    SignColdStake(tx, 0, scriptP2CS, stakerKey, true);
    BOOST_CHECK(CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err));

    const CKey& dummyKey = DecodeSecret("YNdsth3BsW53DYmCiR12SofWSAt2utXQUSGoin3PekVQCMbzfS7E");
    const CKeyID& dummyKeyID = dummyKey.GetPubKey().GetID();
    const CScript& dummyP2PKH = GetDummyP2PKH(dummyKeyID);

    // Add a dummy P2PKH out at the end
    tx.vout.emplace_back(3 * COIN, dummyP2PKH);
    SignColdStake(tx, 0, scriptP2CS, stakerKey, true);
    BOOST_CHECK(!CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_CHECKCOLDSTAKEVERIFY, ScriptErrorString(err));
    // -- but the owner can
    SignColdStake(tx, 0, scriptP2CS, ownerKey, false);
    BOOST_CHECK(CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err));

    // Add a dummy P2PKH out at the beginning
    tx = good_tx;
    tx.vout[1] = CTxOut(3 * COIN, dummyP2PKH);
    tx.vout.emplace_back(3 * COIN, scriptP2CS);
    SignColdStake(tx, 0, scriptP2CS, stakerKey, true);
    BOOST_CHECK(!CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_CHECKCOLDSTAKEVERIFY, ScriptErrorString(err));
    // -- but the owner can
    SignColdStake(tx, 0, scriptP2CS, ownerKey, false);
    BOOST_CHECK(CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err));

    // Replace with new p2cs
    tx = good_tx;
    tx.vout[1].scriptPubKey = GetDummyP2CS(dummyKeyID);
    SignColdStake(tx, 0, scriptP2CS, stakerKey, true);
    BOOST_CHECK(!CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_CHECKCOLDSTAKEVERIFY, ScriptErrorString(err));
    // -- but the owner can
    SignColdStake(tx, 0, scriptP2CS, ownerKey, false);
    BOOST_CHECK(CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err));

    // Replace with single dummy out
    tx = good_tx;
    tx.vout[1] = CTxOut(COIN, dummyP2PKH);
    SignColdStake(tx, 0, scriptP2CS, stakerKey, true);
    BOOST_CHECK(!CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_CHECKCOLDSTAKEVERIFY, ScriptErrorString(err));
    // -- but the owner can
    SignColdStake(tx, 0, scriptP2CS, ownerKey, false);
    BOOST_CHECK(CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err));
}

// Check that it's not possible to "fake" a P2CS script for the owner by splitting the locking
// and unlocking parts. This particular script can be spent by any key, with a
// unlocking script composed like: <sig> <pk> <DUP> <HASH160> <pkh>
static CScript GetFakeLockingScript(const CKeyID staker, const CKeyID& owner)
{
    CScript script;
    script << opcodetype(0x2F) << opcodetype(0x01) << OP_ROT <<
            OP_IF << OP_CHECKCOLDSTAKEVERIFY << ToByteVector(staker) <<
            OP_ELSE << ToByteVector(owner) << OP_DROP <<
            OP_EQUALVERIFY << OP_CHECKSIG;

    return script;
}

void FakeUnlockColdStake(CMutableTransaction& tx, const CScript& prevScript, const CKey& key)
{
    // sign the first input
    tx.vin[0].scriptSig.clear();
    const CTransaction _tx(tx);
    SigVersion sv = _tx.GetRequiredSigVersion();
    const uint256& hash = SignatureHash(prevScript, _tx, 0, SIGHASH_ALL, amtIn, sv);
    std::vector<unsigned char> vchSig;
    BOOST_CHECK(key.Sign(hash, vchSig));
    vchSig.push_back((unsigned char)SIGHASH_ALL);
    tx.vin[0].scriptSig << vchSig << ToByteVector(key.GetPubKey()) << OP_DUP << OP_HASH160 << ToByteVector(key.GetPubKey().GetID());
}

static void setupWallet(CWallet& wallet)
{
    wallet.SetMinVersion(FEATURE_SAPLING);
    wallet.SetupSPKM(false);
}

BOOST_AUTO_TEST_CASE(fake_script_test)
{
    BOOST_ASSERT(!g_newP2CSRules);

    CWallet& wallet = m_wallet;
    LOCK(wallet.cs_wallet);
    setupWallet(wallet);
    CKey stakerKey;         // dummy staker key (not in the wallet)
    stakerKey.MakeNewKey(true);
    CKeyID stakerId = stakerKey.GetPubKey().GetID();
    CPubKey ownerPubKey;
    BOOST_ASSERT(wallet.GetKeyFromPool(ownerPubKey));
    const CKeyID& ownerId = ownerPubKey.GetID();
    CKey ownerKey;          // owner key (in the wallet)
    BOOST_ASSERT(wallet.GetKey(ownerId, ownerKey));

    const CScript& scriptP2CS = GetFakeLockingScript(stakerId, ownerId);

    // Create prev transaction:
    // It has two outputs. The first one is spent before v5.2.
    // The second one is tested after v6 enforcement.
    CMutableTransaction txFrom;
    txFrom.vout.resize(2);
    for (size_t i = 0; i < 2; i++) {
        txFrom.vout[i].nValue = amtIn;
        txFrom.vout[i].scriptPubKey = scriptP2CS;
    }

    // passes IsPayToColdStaking
    BOOST_CHECK(scriptP2CS.IsPayToColdStaking());

    // the output amount is credited to the owner wallet
    wallet.AddToWallet({&wallet, MakeTransactionRef(CTransaction(txFrom))});
    BOOST_CHECK_EQUAL(wallet.GetWalletTx(txFrom.GetHash())->GetAvailableCredit(false, ISMINE_SPENDABLE_TRANSPARENT), 2 * amtIn);

    // create spend tx
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vout.resize(1);
    tx.vin[0].prevout.n = 0;
    tx.vin[0].prevout.hash = txFrom.GetHash();
    tx.vout[0].nValue = amtIn - 10000;
    tx.vout[0].scriptPubKey = GetScriptForDestination(stakerId);

    // it cannot be spent with the owner key, using the P2CS unlocking script
    SignColdStake(tx, 0, scriptP2CS, ownerKey, false);
    ScriptError err = SCRIPT_ERR_OK;
    BOOST_CHECK(!CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_EQUALVERIFY, ScriptErrorString(err));

    // ... but it can be spent by the staker (or any) key, with the fake unlocking script
    FakeUnlockColdStake(tx, scriptP2CS, stakerKey);
    if (!CheckP2CSScript(tx.vin[0].scriptSig, scriptP2CS, tx, err)) {
        BOOST_ERROR(strprintf("P2CS verification failed: %s", ScriptErrorString(err)));
    }
    wallet.AddToWallet({&wallet, MakeTransactionRef(CTransaction(tx))});
    BOOST_CHECK_EQUAL(wallet.GetWalletTx(txFrom.GetHash())->GetAvailableCredit(false, ISMINE_SPENDABLE_TRANSPARENT), amtIn);

    // Now let's activate new rules
    g_newP2CSRules = true;

    // it does NOT pass IsPayToColdStaking
    BOOST_CHECK_MESSAGE(!scriptP2CS.IsPayToColdStaking(), "Fake script passes as P2CS");

    // the output amount is NOT credited to the owner wallet
    BOOST_CHECK_EQUAL(wallet.GetWalletTx(txFrom.GetHash())->GetAvailableCredit(false, ISMINE_SPENDABLE_TRANSPARENT), 0);
}

BOOST_AUTO_TEST_SUITE_END()
