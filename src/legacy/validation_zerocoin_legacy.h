// Copyright (c) 2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef VALIDATION_ZEROCOIN_LEGACY_H
#define VALIDATION_ZEROCOIN_LEGACY_H

#include "amount.h"
#include "primitives/transaction.h"
#include "txdb.h" // for the zerocoinDB implementation.
#include "validationinterface.h"

bool DisconnectZerocoinTx(const CTransaction& tx);
void DataBaseAccChecksum(const CBlockIndex* pindex, bool fWrite);

#endif //VALIDATION_ZEROCOIN_LEGACY_H
