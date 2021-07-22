// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "budget/budgetvote.h"

#include "net.h"
#include "streams.h"

CBudgetVote::CBudgetVote() :
        CSignedMessage(),
        fValid(true),
        fSynced(false),
        nProposalHash(UINT256_ZERO),
        nVote(VOTE_ABSTAIN),
        nTime(0),
        vin()
{ }

CBudgetVote::CBudgetVote(const CTxIn& vinIn, const uint256& nProposalHashIn, VoteDirection nVoteIn) :
        CSignedMessage(),
        fValid(true),
        fSynced(false),
        nProposalHash(nProposalHashIn),
        nVote(nVoteIn),
        vin(vinIn)
{
    nTime = GetAdjustedTime();
}

void CBudgetVote::Relay() const
{
    CInv inv(MSG_BUDGET_VOTE, GetHash());
    g_connman->RelayInv(inv);
}

uint256 CBudgetVote::GetHash() const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << vin;
    ss << nProposalHash;
    ss << (int) nVote;
    ss << nTime;
    return ss.GetHash();
}

std::string CBudgetVote::GetStrMessage() const
{
    return strprintf("%s %s %d %d", vin.prevout.ToStringShort(), nProposalHash.ToString(), (int)nVote, nTime);
}

UniValue CBudgetVote::ToJSON() const
{
    UniValue bObj(UniValue::VOBJ);
    bObj.pushKV("mnId", vin.prevout.ToStringShort());
    bObj.pushKV("nHash", GetHash().ToString());
    bObj.pushKV("Vote", GetVoteString());
    bObj.pushKV("nTime", nTime);
    bObj.pushKV("fValid", fValid);
    return bObj;
}
