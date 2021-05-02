// Copyright (c) 2016-2020 The ZCash developers
// Copyright (c) 2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "sapling/sapling_validation.h"

#include "consensus/consensus.h" // for MAX_BLOCK_SIZE_CURRENT
#include "script/interpreter.h" // for SigHash
#include "consensus/validation.h" // for CValidationState
#include "util.h" // for error()
#include "consensus/upgrades.h" // for CurrentEpochBranchId()

#include <librustzcash.h>

namespace SaplingValidation {

// Verifies that Shielded txs are properly formed and performs content-independent checks
bool CheckTransaction(const CTransaction& tx, CValidationState& state, CAmount& nValueOut)
{
    bool hasSaplingData = tx.hasSaplingData();

    // v1 must not have shielded data.
    if (!tx.isSaplingVersion() && hasSaplingData) {
        return state.DoS(100, error("%s: Not Sapling version with Sapling data", __func__ ),
                         REJECT_INVALID, "bad-txns-form-not-sapling");
    }

    // if the tx has no shielded data, return true. No check needed.
    if (!hasSaplingData) {
        return true;
    }

    // From here, all of the checks are done in v3+ transactions.

    // if the tx has shielded data, cannot be a coinstake, coinbase, zcspend and zcmint
    if (tx.IsCoinStake() || tx.IsCoinBase() || tx.HasZerocoinSpendInputs() || tx.HasZerocoinMintOutputs())
        return state.DoS(100, error("%s: Sapling version with invalid data", __func__),
                         REJECT_INVALID, "bad-txns-invalid-sapling");

    // Upgrade enforced, basic version rules passing, let's check it
    return CheckTransactionWithoutProofVerification(tx, state, nValueOut);
}

bool CheckTransactionWithoutProofVerification(const CTransaction& tx, CValidationState& state, CAmount& nValueOut)
{
    // Basic checks that don't depend on any context
    const Consensus::Params& consensus = Params().GetConsensus();
    // If the tx got to this point, must be v3+.
    assert(tx.isSaplingVersion());

    // Check for non-zero valueBalance when there are no Sapling inputs or outputs
    if (tx.sapData->vShieldedSpend.empty() && tx.sapData->vShieldedOutput.empty() && tx.sapData->valueBalance != 0) {
        return state.DoS(100, error("%s: tx.sapData->valueBalance has no sources or sinks", __func__ ),
                         REJECT_INVALID, "bad-txns-valuebalance-nonzero");
    }

    // Check for overflow valueBalance
    if (tx.sapData->valueBalance > MAX_MONEY || tx.sapData->valueBalance < -MAX_MONEY) {
        return state.DoS(100, error("%s: abs(tx.sapData->valueBalance) too large", __func__),
                         REJECT_INVALID, "bad-txns-valuebalance-toolarge");
    }

    if (tx.sapData->valueBalance < 0) {
        // NB: negative valueBalance "takes" money from the transparent value pool just as outputs do
        nValueOut += -tx.sapData->valueBalance;

        if (!MoneyRange(nValueOut)) {
            return state.DoS(100, error("%s: txout total out of range", __func__ ),
                             REJECT_INVALID, "bad-txns-txouttotal-toolarge");
        }
    }

    // Ensure input values do not exceed MAX_MONEY
    // We have not resolved the txin values at this stage,
    // but we do know what the shielded tx claim to add
    // to the value pool.

    // NB: positive valueBalance "adds" money to the transparent value pool, just as inputs do
    if (tx.sapData->valueBalance > 0 && !MoneyRange(tx.sapData->valueBalance)) {
        return state.DoS(100, error("%s: txin total out of range", __func__ ),
                         REJECT_INVALID, "bad-txns-txintotal-toolarge");
    }

    // Check for duplicate sapling nullifiers in this transaction
    {
        std::set<uint256> vSaplingNullifiers;
        for (const SpendDescription& spend_desc : tx.sapData->vShieldedSpend) {
            if (vSaplingNullifiers.count(spend_desc.nullifier))
                return state.DoS(100, error("%s: duplicate nullifiers", __func__ ),
                                 REJECT_INVALID, "bad-spend-description-nullifiers-duplicate");

            vSaplingNullifiers.insert(spend_desc.nullifier);
        }
    }

    return true;
}

/**
* Check a transaction contextually against a set of consensus rules valid at a given block height.
*
* Notes:
* 1. AcceptToMemoryPool calls CheckTransaction and this function.
* 2. ProcessNewBlock calls AcceptBlock, which calls CheckBlock (which calls CheckTransaction)
*    and ContextualCheckBlock (which calls this function).
* 3. For consensus rules that relax restrictions (where a transaction that is invalid at
*    nHeight can become valid at a later height), we make the bans conditional on not
*    being in Initial Block Download mode.
* 4. The isInitBlockDownload argument is a function parameter to assist with testing.
*
*/
bool ContextualCheckTransaction(
        const CTransaction& tx,
        CValidationState &state,
        const CChainParams& chainparams,
        const int nHeight,
        const bool isMined,
        bool isInitBlockDownload)
{
    const int DOS_LEVEL_BLOCK = 100;
    // DoS level set to 10 to be more forgiving.
    const int DOS_LEVEL_MEMPOOL = 10;

    // For constricting rules, we don't need to account for IBD mode.
    auto dosLevelConstricting = isMined ? DOS_LEVEL_BLOCK : DOS_LEVEL_MEMPOOL;
    // For rules that are relaxing (or might become relaxing when a future
    // network upgrade is implemented), we need to account for IBD mode.
    auto dosLevelPotentiallyRelaxing = isMined ? DOS_LEVEL_BLOCK : (
            isInitBlockDownload ? 0 : DOS_LEVEL_MEMPOOL);

    // If Sapling is not active return quickly and don't perform any check here.
    // basic data checks are performed in CheckTransaction which is ALWAYS called before ContextualCheckTransaction.
    if (!chainparams.GetConsensus().NetworkUpgradeActive(nHeight, Consensus::UPGRADE_V5_0)) {
        // If the v5 upgrade was not enforced, then let's not perform any check
        if (tx.IsShieldedTx()) {
            return state.DoS(dosLevelConstricting, error("%s: Sapling not activated", __func__),
                             REJECT_INVALID, "bad-txns-invalid-sapling-act");
        }
        return true;
    }

    // Reject transactions with invalid version
    if (!tx.isSaplingVersion() && tx.hasSaplingData()) {
        return state.DoS(
                dosLevelConstricting,
                error("%s: Sapling version too low", __func__ ),
                REJECT_INVALID, "bad-tx-sapling-version-too-low");
    }

    bool hasShieldedData = tx.hasSaplingData();
    // A coinbase/coinstake transaction cannot have output descriptions nor shielded spends
    if (tx.IsCoinBase() || tx.IsCoinStake()) {
        if (hasShieldedData)
            return state.DoS(
                    dosLevelPotentiallyRelaxing,
                    error("%s: coinbase/coinstake has output/spend descriptions", __func__ ),
                    REJECT_INVALID, "bad-cs-has-shielded-data");
    }

    if (hasShieldedData) {
        uint256 dataToBeSigned;
        // Empty output script.
        CScript scriptCode;
        try {
            dataToBeSigned = SignatureHash(scriptCode, tx, NOT_AN_INPUT, SIGHASH_ALL, 0, SIGVERSION_SAPLING);
        } catch (const std::logic_error& ex) {
            // A logic error should never occur because we pass NOT_AN_INPUT and
            // SIGHASH_ALL to SignatureHash().
            return state.DoS(100, error("%s: error computing signature hash", __func__ ),
                             REJECT_INVALID, "error-computing-signature-hash");
        }

        // Sapling verification process
        auto ctx = librustzcash_sapling_verification_ctx_init();

        for (const SpendDescription &spend : tx.sapData->vShieldedSpend) {
            if (!librustzcash_sapling_check_spend(
                    ctx,
                    spend.cv.begin(),
                    spend.anchor.begin(),
                    spend.nullifier.begin(),
                    spend.rk.begin(),
                    spend.zkproof.begin(),
                    spend.spendAuthSig.begin(),
                    dataToBeSigned.begin())) {
                librustzcash_sapling_verification_ctx_free(ctx);
                return state.DoS(
                        dosLevelPotentiallyRelaxing,
                        error("%s: Sapling spend description invalid", __func__ ),
                        REJECT_INVALID, "bad-txns-sapling-spend-description-invalid");
            }
        }

        for (const OutputDescription &output : tx.sapData->vShieldedOutput) {
            if (!librustzcash_sapling_check_output(
                    ctx,
                    output.cv.begin(),
                    output.cmu.begin(),
                    output.ephemeralKey.begin(),
                    output.zkproof.begin())) {
                librustzcash_sapling_verification_ctx_free(ctx);
                // This should be a non-contextual check, but we check it here
                // as we need to pass over the outputs anyway in order to then
                // call librustzcash_sapling_final_check().
                return state.DoS(100, error("%s: Sapling output description invalid", __func__ ),
                                 REJECT_INVALID, "bad-txns-sapling-output-description-invalid");
            }
        }

        if (!librustzcash_sapling_final_check(
                ctx,
                tx.sapData->valueBalance,
                tx.sapData->bindingSig.begin(),
                dataToBeSigned.begin())) {
            librustzcash_sapling_verification_ctx_free(ctx);
            return state.DoS(
                    dosLevelPotentiallyRelaxing,
                    error("%s: Sapling binding signature invalid", __func__ ),
                    REJECT_INVALID, "bad-txns-sapling-binding-signature-invalid");
        }

        librustzcash_sapling_verification_ctx_free(ctx);
    }
    return true;
}


} // End SaplingValidation namespace
