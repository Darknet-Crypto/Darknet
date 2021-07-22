// Copyright (c) 2019 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "addressbook.h"

namespace AddressBook {

    namespace AddressBookPurpose {
        const std::string UNKNOWN{"unknown"};
        const std::string RECEIVE{"receive"};
        const std::string SEND{"send"};
        const std::string DELEGABLE{"delegable"};
        const std::string DELEGATOR{"delegator"};
        const std::string COLD_STAKING{"coldstaking"};
        const std::string COLD_STAKING_SEND{"coldstaking_send"};
        const std::string SHIELDED_RECEIVE{"shielded_receive"};
        const std::string SHIELDED_SEND{"shielded_spend"};
    }

    bool IsColdStakingPurpose(const std::string& purpose) {
        return purpose == AddressBookPurpose::COLD_STAKING
               || purpose == AddressBookPurpose::COLD_STAKING_SEND;
    }

    bool IsShieldedPurpose(const std::string& purpose) {
        return purpose == AddressBookPurpose::SHIELDED_RECEIVE
               || purpose == AddressBookPurpose::SHIELDED_SEND;
    }

    bool CAddressBookData::isSendColdStakingPurpose() const {
        return purpose == AddressBookPurpose::COLD_STAKING_SEND;
    }

    bool CAddressBookData::isSendPurpose() const {
        return purpose == AddressBookPurpose::SEND;
    }

    bool CAddressBookData::isReceivePurpose() const {
        return purpose == AddressBookPurpose::RECEIVE;
    }

    bool CAddressBookData::isShieldedReceivePurpose() const {
        return purpose == AddressBookPurpose::SHIELDED_RECEIVE;
    }

    bool CAddressBookData::isShielded() const {
        return IsShieldedPurpose(purpose);
    }


}

