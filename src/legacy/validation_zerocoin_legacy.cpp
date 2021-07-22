// Copyright (c) 2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.
#include "legacy/validation_zerocoin_legacy.h"

#include "libzerocoin/CoinSpend.h"
#include "wallet/wallet.h"
#include "zpivchain.h"
#include "zpiv/zpivmodule.h"

bool DisconnectZerocoinTx(const CTransaction& tx)
{
    /** UNDO ZEROCOIN DATABASING
         * note we only undo zerocoin databasing in the following statement, value to and from PIVX
         * addresses should still be handled by the typical bitcoin based undo code
         * */
    if (tx.ContainsZerocoins()) {
        libzerocoin::ZerocoinParams *params = Params().GetConsensus().Zerocoin_Params(false);
        if (tx.HasZerocoinSpendInputs()) {
            //erase all zerocoinspends in this transaction
            for (const CTxIn &txin : tx.vin) {
                bool isPublicSpend = txin.IsZerocoinPublicSpend();
                if (txin.scriptSig.IsZerocoinSpend() || isPublicSpend) {
                    CBigNum serial;
                    if (isPublicSpend) {
                        PublicCoinSpend publicSpend(params);
                        CValidationState state;
                        if (!ZPIVModule::ParseZerocoinPublicSpend(txin, tx, state, publicSpend)) {
                            return error("Failed to parse public spend");
                        }
                        serial = publicSpend.getCoinSerialNumber();
                    } else {
                        libzerocoin::CoinSpend spend = TxInToZerocoinSpend(txin);
                        serial = spend.getCoinSerialNumber();
                    }

                    if (!zerocoinDB->EraseCoinSpend(serial))
                        return error("failed to erase spent zerocoin in block");
                }

            }
        }
    }
    return true;
}

// Legacy Zerocoin DB: used for performance during IBD
// (between Zerocoin_Block_V2_Start and Zerocoin_Block_Last_Checkpoint)
void DataBaseAccChecksum(const CBlockIndex* pindex, bool fWrite)
{
    const Consensus::Params& consensus = Params().GetConsensus();
    if (!pindex ||
        !consensus.NetworkUpgradeActive(pindex->nHeight, Consensus::UPGRADE_ZC_V2) ||
        pindex->nHeight > consensus.height_last_ZC_AccumCheckpoint ||
        pindex->nAccumulatorCheckpoint == pindex->pprev->nAccumulatorCheckpoint)
        return;

    arith_uint256 accCurr = UintToArith256(pindex->nAccumulatorCheckpoint);
    arith_uint256 accPrev = UintToArith256(pindex->pprev->nAccumulatorCheckpoint);
    // add/remove changed checksums to/from DB
    for (int i = (int)libzerocoin::zerocoinDenomList.size()-1; i >= 0; i--) {
        const uint32_t nChecksum = accCurr.Get32();
        if (nChecksum != accPrev.Get32()) {
            fWrite ?
            zerocoinDB->WriteAccChecksum(nChecksum, libzerocoin::zerocoinDenomList[i], pindex->nHeight) :
            zerocoinDB->EraseAccChecksum(nChecksum, libzerocoin::zerocoinDenomList[i]);
        }
        accCurr >>= 32;
        accPrev >>= 32;
    }
}
