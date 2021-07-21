// Copyright (c) 2013 The Bitcoin Core developers
// Copyright (c) 2017-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test/test_pivx.h"

#include "test/data/sighash.json.h"

#include "consensus/tx_verify.h"
#include "serialize.h"
#include "script/script.h"
#include "script/interpreter.h"
#include "util/system.h"
#include "validation.h"
#include "version.h"

#include <iostream>

#include <boost/test/unit_test.hpp>

#include <univalue.h>

extern UniValue read_json(const std::string& jsondata);

// Old script.cpp SignatureHash function
uint256 static SignatureHashOld(CScript scriptCode, const CTransaction& txTo, unsigned int nIn, int nHashType)
{
    if (nIn >= txTo.vin.size())
    {
        printf("ERROR: SignatureHash() : nIn=%d out of range\n", nIn);
        return UINT256_ONE;
    }
    CMutableTransaction txTmp(txTo);

    // In case concatenating two scripts ends up with two codeseparators,
    // or an extra one at the end, this prevents all those possible incompatibilities.
    scriptCode.FindAndDelete(CScript(OP_CODESEPARATOR));

    // Blank out other inputs' signatures
    for (unsigned int i = 0; i < txTmp.vin.size(); i++)
        txTmp.vin[i].scriptSig = CScript();
    txTmp.vin[nIn].scriptSig = scriptCode;

    // Blank out some of the outputs
    if ((nHashType & 0x1f) == SIGHASH_NONE)
    {
        // Wildcard payee
        txTmp.vout.clear();

        // Let the others update at will
        for (unsigned int i = 0; i < txTmp.vin.size(); i++)
            if (i != nIn)
                txTmp.vin[i].nSequence = 0;
    }
    else if ((nHashType & 0x1f) == SIGHASH_SINGLE)
    {
        // Only lock-in the txout payee at same index as txin
        unsigned int nOut = nIn;
        if (nOut >= txTmp.vout.size())
        {
            printf("ERROR: SignatureHash() : nOut=%d out of range\n", nOut);
            return UINT256_ONE;
        }
        txTmp.vout.resize(nOut+1);
        for (unsigned int i = 0; i < nOut; i++)
            txTmp.vout[i].SetNull();

        // Let the others update at will
        for (unsigned int i = 0; i < txTmp.vin.size(); i++)
            if (i != nIn)
                txTmp.vin[i].nSequence = 0;
    }

    // Blank out other inputs completely, not recommended for open transactions
    if (nHashType & SIGHASH_ANYONECANPAY)
    {
        txTmp.vin[0] = txTmp.vin[nIn];
        txTmp.vin.resize(1);
    }

    // Serialize and hash
    CHashWriter ss(SER_GETHASH, 0);
    ss << txTmp << nHashType;
    return ss.GetHash();
}

void static RandomScript(CScript &script) {
    static const opcodetype oplist[] = {OP_FALSE, OP_1, OP_2, OP_3, OP_CHECKSIG, OP_IF, OP_VERIF, OP_RETURN, OP_CODESEPARATOR};
    script = CScript();
    int ops = (InsecureRandRange(10));
    for (int i=0; i<ops; i++)
        script << oplist[InsecureRandRange(sizeof(oplist)/sizeof(oplist[0]))];
}

void static RandomTransaction(CMutableTransaction &tx, bool fSingle) {
    bool isSapling = !(InsecureRand32() % 7);
    tx.nVersion = isSapling ? CTransaction::TxVersion::SAPLING : CTransaction::TxVersion::LEGACY;
    tx.vin.clear();
    tx.vout.clear();
    tx.sapData->vShieldedSpend.clear();
    tx.sapData->vShieldedOutput.clear();
    tx.nLockTime = (InsecureRandBool()) ? InsecureRand32() : 0;
    int ins = (InsecureRandBits(2)) + 1;
    int outs = fSingle ? ins : (InsecureRandBits(2)) + 1;
    for (int in = 0; in < ins; in++) {
        tx.vin.emplace_back();
        CTxIn &txin = tx.vin.back();
        txin.prevout.hash = InsecureRand256();
        txin.prevout.n = InsecureRandBits(2);
        RandomScript(txin.scriptSig);
        txin.nSequence = (InsecureRandBool()) ? InsecureRand32() : (unsigned int)-1;
    }
    for (int out = 0; out < outs; out++) {
        tx.vout.emplace_back();
        CTxOut &txout = tx.vout.back();
        txout.nValue = InsecureRandRange(100000000);
        RandomScript(txout.scriptPubKey);
    }

    if (tx.nVersion == 2) {
        int shielded_spends = (InsecureRandBits(2)) + 1;
        int shielded_outs = (InsecureRandBits(2)) + 1;
        tx.sapData->valueBalance = InsecureRandRange(100000000);;
        for (int spend = 0; spend < shielded_spends; spend++) {
            SpendDescription sdesc;
            sdesc.cv = GetRandHash();
            sdesc.anchor = GetRandHash();
            sdesc.nullifier = GetRandHash();
            sdesc.rk = GetRandHash();
            randombytes_buf(sdesc.zkproof.begin(), sdesc.zkproof.size());
            tx.sapData->vShieldedSpend.push_back(sdesc);
        }
        for (int out = 0; out < shielded_outs; out++) {
            OutputDescription odesc;
            odesc.cv = GetRandHash();
            odesc.cmu = GetRandHash();
            odesc.ephemeralKey = GetRandHash();
            randombytes_buf(odesc.encCiphertext.begin(), odesc.encCiphertext.size());
            randombytes_buf(odesc.outCiphertext.begin(), odesc.outCiphertext.size());
            randombytes_buf(odesc.zkproof.begin(), odesc.zkproof.size());
            tx.sapData->vShieldedOutput.push_back(odesc);
        }
    }
}

BOOST_FIXTURE_TEST_SUITE(sighash_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(sighash_test)
{
    SeedInsecureRand(false);

    #if defined(PRINT_SIGHASH_JSON)
    std::cout << "[\n";
    std::cout << "\t[\"raw_transaction, script, input_index, hashType, signature_hash (result)\"],\n";
    #endif
    int nRandomTests = 50000;

    #if defined(PRINT_SIGHASH_JSON)
    nRandomTests = 500;
    #endif
    for (int i=0; i<nRandomTests; i++) {
        int nHashType = InsecureRand32();
        CMutableTransaction txTo;
        RandomTransaction(txTo, (nHashType & 0x1f) == SIGHASH_SINGLE);
        CScript scriptCode;
        RandomScript(scriptCode);
        int nIn = InsecureRandRange(txTo.vin.size());

        uint256 sh, sho;
        sho = SignatureHashOld(scriptCode, txTo, nIn, nHashType);
        sh = SignatureHash(scriptCode, txTo, nIn, nHashType, 0, txTo.GetRequiredSigVersion());
        #if defined(PRINT_SIGHASH_JSON)
        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << txTo;
        std::cout << "\t[\"" ;
        std::cout << HexStr(ss) << "\", \"";
        std::cout << HexStr(scriptCode) << "\", ";
        std::cout << nIn << ", ";
        std::cout << nHashType << ", \"";
        std::cout << sho.GetHex() << "\"]";
        if (i+1 != nRandomTests) {
          std::cout << ",";
        }
        std::cout << "\n";
        #endif
        if (txTo.nVersion < CTransaction::TxVersion::SAPLING) { // Sapling has a different signature.
            BOOST_CHECK(sh == sho);
        }
    }
    #if defined(PRINT_SIGHASH_JSON)
    std::cout << "]\n";
    #endif
}

// Goal: check that SignatureHash generates correct hash
// TODO: Update with Sapling transactions..
BOOST_AUTO_TEST_CASE(sighash_from_data)
{
    UniValue tests = read_json(std::string(json_tests::sighash, json_tests::sighash + sizeof(json_tests::sighash)));

    for (unsigned int idx = 0; idx < tests.size(); idx++) {
        UniValue test = tests[idx];
        std::string strTest = test.write();
        if (test.size() < 1) // Allow for extra stuff (useful for comments)
        {
            BOOST_ERROR("Bad test: " << strTest);
            continue;
        }
        if (test.size() == 1) continue; // comment

        std::string raw_tx, raw_script, sigHashHex;
        int nIn, nHashType;
        uint256 sh;
        CTransactionRef tx;
        CScript scriptCode = CScript();

        try {
          // deserialize test data
          raw_tx = test[0].get_str();
          raw_script = test[1].get_str();
          nIn = test[2].get_int();
          nHashType = test[3].get_int();
          sigHashHex = test[4].get_str();

          uint256 sh;
          CDataStream stream(ParseHex(raw_tx), SER_NETWORK, PROTOCOL_VERSION);
          stream >> tx;

          CValidationState state;
          BOOST_CHECK_MESSAGE(CheckTransaction(*tx, state, false), strTest);
          BOOST_CHECK(state.IsValid());

          std::vector<unsigned char> raw = ParseHex(raw_script);
          scriptCode.insert(scriptCode.end(), raw.begin(), raw.end());
        } catch (...) {
          BOOST_ERROR("Bad test, couldn't deserialize data: " << strTest);
          continue;
        }

        sh = SignatureHash(scriptCode, *tx, nIn, nHashType, 0, tx->GetRequiredSigVersion());
        BOOST_CHECK_MESSAGE(sh.GetHex() == sigHashHex, strTest);

    }
}

BOOST_AUTO_TEST_CASE(malleated_tx)
{
    int nRandomTests = 5000;
    std::vector<uint256> vsh;
    std::vector<CScript> vScriptCode;
    for (int t = 0; t < nRandomTests; t++) {
        vsh.clear();
        vScriptCode.clear();
        // create a random tx and get the signature hashes
        CMutableTransaction _tx;
        RandomTransaction(_tx, false);
        const CTransaction tx(_tx);
        for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
            vScriptCode.emplace_back();
            RandomScript(vScriptCode.back());
            vsh.emplace_back(SignatureHash(vScriptCode.back(), tx, nIn, SIGHASH_ALL, 0, tx.GetRequiredSigVersion()));
        }

        // -- create malleated txes
        CMutableTransaction mtx = _tx;
        for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
            BOOST_CHECK(vsh[nIn] == SignatureHash(vScriptCode[nIn], CTransaction(mtx), nIn, SIGHASH_ALL, 0, tx.GetRequiredSigVersion()));
        }

        // remove one transparent input
        if (mtx.vin.size() > 0) {
            mtx.vin.pop_back();
            CTransaction tx2(mtx);
            for (int nIn = 0; nIn < (int) tx.vin.size() - 1; nIn++) {
                BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], tx2, nIn, SIGHASH_ALL, 0, tx2.GetRequiredSigVersion()));
            }
        }

        // change the prevout of the transparent input being signed
        mtx = _tx;
        for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
            while (mtx.vin[nIn].prevout.hash == tx.vin[nIn].prevout.hash)
                mtx.vin[nIn].prevout.hash = InsecureRand256();
            CTransaction tx2(mtx);
            BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], tx2, nIn, SIGHASH_ALL, 0, tx2.GetRequiredSigVersion()));
        }

        // add one random transparent input
        {
            mtx = _tx;
            CTxIn in(InsecureRand256(), InsecureRandBits(2));
            RandomScript(in.scriptSig);
            in.nSequence = (InsecureRandBool()) ? InsecureRand32() : (unsigned int)-1;
            mtx.vin.emplace_back(in);
            CTransaction tx2(mtx);
            for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
                BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], tx2, nIn, SIGHASH_ALL, 0, tx2.GetRequiredSigVersion()));
            }
        }

        // Shield inputs
        if (tx.sapData && tx.sapData->vShieldedSpend.size() > 0) {
            // remove one spend
            mtx = _tx;
            mtx.sapData->vShieldedSpend.pop_back();
            for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
                BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], CTransaction(mtx), nIn, SIGHASH_ALL, 0, SigVersion::SIGVERSION_SAPLING));
            }

            // modify one spend (cv, anchor, nullifier, rk, zkproof, spendAuthSig)
            int i = InsecureRandRange(tx.sapData->vShieldedSpend.size());

            mtx = _tx;
            while (mtx.sapData->vShieldedSpend[i].cv == tx.sapData->vShieldedSpend[i].cv)
                mtx.sapData->vShieldedSpend[i].cv = GetRandHash();
            for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
                BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], CTransaction(mtx), nIn, SIGHASH_ALL, 0, SigVersion::SIGVERSION_SAPLING));
            }

            mtx = _tx;
            while (mtx.sapData->vShieldedSpend[i].anchor == tx.sapData->vShieldedSpend[i].anchor)
                mtx.sapData->vShieldedSpend[i].anchor = GetRandHash();
            for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
                BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], CTransaction(mtx), nIn, SIGHASH_ALL, 0, SigVersion::SIGVERSION_SAPLING));
            }

            mtx = _tx;
            while (mtx.sapData->vShieldedSpend[i].nullifier == tx.sapData->vShieldedSpend[i].nullifier)
                mtx.sapData->vShieldedSpend[i].nullifier = GetRandHash();
            for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
                BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], CTransaction(mtx), nIn, SIGHASH_ALL, 0, SigVersion::SIGVERSION_SAPLING));
            }

            mtx = _tx;
            while (mtx.sapData->vShieldedSpend[i].rk == tx.sapData->vShieldedSpend[i].rk)
                mtx.sapData->vShieldedSpend[i].rk = GetRandHash();
            for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
                BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], CTransaction(mtx), nIn, SIGHASH_ALL, 0, SigVersion::SIGVERSION_SAPLING));
            }

            mtx = _tx;
            auto zkproof = &mtx.sapData->vShieldedSpend[i].zkproof;
            randombytes_buf(zkproof->begin(), zkproof->size());
            for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
                BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], CTransaction(mtx), nIn, SIGHASH_ALL, 0, SigVersion::SIGVERSION_SAPLING));
            }

            mtx = _tx;
            auto spendAuthSig = &mtx.sapData->vShieldedSpend[i].spendAuthSig;
            randombytes_buf(spendAuthSig->begin(), spendAuthSig->size());
            for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
                BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], CTransaction(mtx), nIn, SIGHASH_ALL, 0, SigVersion::SIGVERSION_SAPLING));
            }
        }

        // add one random spend
        mtx = _tx;
        SpendDescription sdesc;
        sdesc.cv = GetRandHash();
        sdesc.anchor = GetRandHash();
        sdesc.nullifier = GetRandHash();
        sdesc.rk = GetRandHash();
        randombytes_buf(sdesc.zkproof.begin(), sdesc.zkproof.size());
        mtx.sapData->vShieldedSpend.push_back(sdesc);
        for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
            BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], CTransaction(mtx), nIn, SIGHASH_ALL, 0, SigVersion::SIGVERSION_SAPLING));
        }

        // Transparent outputs
        if (mtx.vout.size() > 0) {
            // remove one
            mtx = _tx;
            mtx.vout.pop_back();
            CTransaction tx2(mtx);
            for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
                BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], tx2, nIn, SIGHASH_ALL, 0, tx2.GetRequiredSigVersion()));
            }

            // modify one (amount, script)
            int i = InsecureRandRange(tx.vout.size());
            mtx = _tx;
            while (mtx.vout[i].nValue == tx.vout[i].nValue)
                mtx.vout[i].nValue = InsecureRandRange(100000000);
            CTransaction tx3 = CTransaction(mtx);
            for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
                BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], tx3, nIn, SIGHASH_ALL, 0, tx3.GetRequiredSigVersion()));
            }

            mtx = _tx;
            while (mtx.vout[i].scriptPubKey == tx.vout[i].scriptPubKey)
                RandomScript(mtx.vout[i].scriptPubKey);
            CTransaction tx4 = CTransaction(mtx);
            for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
                BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], tx4, nIn, SIGHASH_ALL, 0, tx4.GetRequiredSigVersion()));
            }
        }

        // add a random trasparent output
        {
            mtx = _tx;
            CTxOut out;
            out.nValue = InsecureRandRange(100000000);
            RandomScript(out.scriptPubKey);
            mtx.vout.emplace_back(out);
            CTransaction tx2(mtx);
            for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
                BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], tx2, nIn, SIGHASH_ALL, 0, tx2.GetRequiredSigVersion()));
            }
        }

        // Shield outputs
        if (tx.sapData && tx.sapData->vShieldedOutput.size() > 0) {
            // remove one output
            mtx = _tx;
            mtx.sapData->vShieldedOutput.pop_back();
            for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
                BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], CTransaction(mtx), nIn, SIGHASH_ALL, 0, SigVersion::SIGVERSION_SAPLING));
            }

            // modify one output (cv, cmu, nullifier, ephemeralKey, encCiphertext, outCiphertext, zkproof)
            int i = InsecureRandRange(tx.sapData->vShieldedOutput.size());

            mtx = _tx;
            while (mtx.sapData->vShieldedOutput[i].cv == tx.sapData->vShieldedOutput[i].cv)
                mtx.sapData->vShieldedOutput[i].cv = GetRandHash();
            for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
                BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], CTransaction(mtx), nIn, SIGHASH_ALL, 0, SigVersion::SIGVERSION_SAPLING));
            }

            mtx = _tx;
            while (mtx.sapData->vShieldedOutput[i].cmu == tx.sapData->vShieldedOutput[i].cmu)
                mtx.sapData->vShieldedOutput[i].cmu = GetRandHash();
            for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
                BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], CTransaction(mtx), nIn, SIGHASH_ALL, 0, SigVersion::SIGVERSION_SAPLING));
            }

            mtx = _tx;
            while (mtx.sapData->vShieldedOutput[i].ephemeralKey == tx.sapData->vShieldedOutput[i].ephemeralKey)
                mtx.sapData->vShieldedOutput[i].ephemeralKey = GetRandHash();
            for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
                BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], CTransaction(mtx), nIn, SIGHASH_ALL, 0, SigVersion::SIGVERSION_SAPLING));
            }

            mtx = _tx;
            auto encCiphertext = &mtx.sapData->vShieldedOutput[i].encCiphertext;
            randombytes_buf(encCiphertext->begin(), encCiphertext->size());
            for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
                BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], CTransaction(mtx), nIn, SIGHASH_ALL, 0, SigVersion::SIGVERSION_SAPLING));
            }

            mtx = _tx;
            auto outCiphertext = &mtx.sapData->vShieldedOutput[i].outCiphertext;
            randombytes_buf(outCiphertext->begin(), outCiphertext->size());
            for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
                BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], CTransaction(mtx), nIn, SIGHASH_ALL, 0, SigVersion::SIGVERSION_SAPLING));
            }

            mtx = _tx;
            auto zkproof = &mtx.sapData->vShieldedOutput[i].zkproof;
            randombytes_buf(zkproof->begin(), zkproof->size());
            for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
                BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], CTransaction(mtx), nIn, SIGHASH_ALL, 0, SigVersion::SIGVERSION_SAPLING));
            }
        }

        // add one random output
        mtx = _tx;
        OutputDescription odesc;
        odesc.cv = GetRandHash();
        odesc.cmu = GetRandHash();
        odesc.ephemeralKey = GetRandHash();
        randombytes_buf(odesc.encCiphertext.begin(), odesc.encCiphertext.size());
        randombytes_buf(odesc.outCiphertext.begin(), odesc.outCiphertext.size());
        randombytes_buf(odesc.zkproof.begin(), odesc.zkproof.size());
        mtx.sapData->vShieldedOutput.push_back(odesc);
        for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
            BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], CTransaction(mtx), nIn, SIGHASH_ALL, 0, SigVersion::SIGVERSION_SAPLING));
        }

        // modify the value balance
        if (tx.sapData &&
                (!tx.sapData->vShieldedOutput.empty() || !tx.sapData->vShieldedSpend.empty())) {
            mtx = _tx;
            while (mtx.sapData->valueBalance == tx.sapData->valueBalance)
                mtx.sapData->valueBalance = InsecureRandRange(100000000);
            for (int nIn = 0; nIn < (int) tx.vin.size(); nIn++) {
                BOOST_CHECK(vsh[nIn] != SignatureHash(vScriptCode[nIn], CTransaction(mtx), nIn, SIGHASH_ALL, 0, SigVersion::SIGVERSION_SAPLING));
            }
        }

    }
}

BOOST_AUTO_TEST_SUITE_END()
