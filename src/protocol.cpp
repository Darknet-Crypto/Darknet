// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2017-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "protocol.h"

#include "util/system.h"
#include "utilstrencodings.h"

#ifndef WIN32
#include <arpa/inet.h>
#endif

namespace NetMsgType
{
const char* VERSION = "version";
const char* VERACK = "verack";
const char* ADDR = "addr";
const char* INV = "inv";
const char* GETDATA = "getdata";
const char* MERKLEBLOCK = "merkleblock";
const char* GETBLOCKS = "getblocks";
const char* GETHEADERS = "getheaders";
const char* TX = "tx";
const char* HEADERS = "headers";
const char* BLOCK = "block";
const char* GETADDR = "getaddr";
const char* MEMPOOL = "mempool";
const char* PING = "ping";
const char* PONG = "pong";
const char* ALERT = "alert";
const char* NOTFOUND = "notfound";
const char* FILTERLOAD = "filterload";
const char* FILTERADD = "filteradd";
const char* FILTERCLEAR = "filterclear";
const char* SENDHEADERS = "sendheaders";
const char* SPORK = "spork";
const char* GETSPORKS = "getsporks";
const char* MNBROADCAST = "mnb";
const char* MNPING = "mnp";
const char* MNWINNER = "mnw";
const char* GETMNWINNERS = "mnget";
const char* BUDGETPROPOSAL = "mprop";
const char* BUDGETVOTE = "mvote";
const char* BUDGETVOTESYNC = "mnvs";
const char* FINALBUDGET = "fbs";
const char* FINALBUDGETVOTE = "fbvote";
const char* SYNCSTATUSCOUNT = "ssc";
const char* GETMNLIST = "dseg";
}; // namespace NetMsgType

/** All known message types. Keep this in the same order as the list of
 * messages above and in protocol.h.
 */
const static std::string allNetMessageTypes[] = {
    NetMsgType::VERSION,
    NetMsgType::VERACK,
    NetMsgType::ADDR,
    NetMsgType::INV,
    NetMsgType::GETDATA,
    NetMsgType::MERKLEBLOCK,
    NetMsgType::GETBLOCKS,
    NetMsgType::GETHEADERS,
    NetMsgType::TX,
    NetMsgType::HEADERS,
    NetMsgType::BLOCK,
    NetMsgType::GETADDR,
    NetMsgType::MEMPOOL,
    NetMsgType::PING,
    NetMsgType::PONG,
    NetMsgType::ALERT,
    NetMsgType::NOTFOUND,
    NetMsgType::FILTERLOAD,
    NetMsgType::FILTERADD,
    NetMsgType::FILTERCLEAR,
    NetMsgType::SENDHEADERS,
    "filtered block", // Should never occur
    "ix",   // deprecated
    "txlvote", // deprecated
    NetMsgType::SPORK,           // --- tiertwoNetMessageTypes start here ---
    NetMsgType::MNWINNER,
    "mnodescanerr",
    NetMsgType::BUDGETVOTE,
    NetMsgType::BUDGETPROPOSAL,
    NetMsgType::FINALBUDGET,
    NetMsgType::FINALBUDGETVOTE,
    "mnq",
    NetMsgType::MNBROADCAST,
    NetMsgType::MNPING,
    "dstx",  // deprecated
    NetMsgType::GETMNWINNERS,
    NetMsgType::GETMNLIST,
    NetMsgType::BUDGETVOTESYNC,
    NetMsgType::GETSPORKS,
    NetMsgType::SYNCSTATUSCOUNT
};
const static std::vector<std::string> allNetMessageTypesVec(allNetMessageTypes, allNetMessageTypes + ARRAYLEN(allNetMessageTypes));
const static std::vector<std::string> tiertwoNetMessageTypesVec(std::find(allNetMessageTypesVec.begin(), allNetMessageTypesVec.end(), NetMsgType::SPORK), allNetMessageTypesVec.end());

CMessageHeader::CMessageHeader(const MessageStartChars& pchMessageStartIn)
{
    memcpy(pchMessageStart, pchMessageStartIn, MESSAGE_START_SIZE);
    memset(pchCommand, 0, sizeof(pchCommand));
    nMessageSize = -1;
    memset(pchChecksum, 0, CHECKSUM_SIZE);
}

CMessageHeader::CMessageHeader(const MessageStartChars& pchMessageStartIn, const char* pszCommand, unsigned int nMessageSizeIn)
{
    memcpy(pchMessageStart, pchMessageStartIn, MESSAGE_START_SIZE);

    // Copy the command name, zero-padding to COMMAND_SIZE bytes
    size_t i = 0;
    for (; i < COMMAND_SIZE && pszCommand[i] != 0; ++i) pchCommand[i] = pszCommand[i];
    assert(pszCommand[i] == 0); // Assert that the command name passed in is not longer than COMMAND_SIZE
    for (; i < COMMAND_SIZE; ++i) pchCommand[i] = 0;

    nMessageSize = nMessageSizeIn;
    memset(pchChecksum, 0, CHECKSUM_SIZE);
}

std::string CMessageHeader::GetCommand() const
{
    return std::string(pchCommand, pchCommand + strnlen(pchCommand, COMMAND_SIZE));
}

bool CMessageHeader::IsValid(const MessageStartChars& pchMessageStartIn) const
{
    // Check start string
    if (memcmp(pchMessageStart, pchMessageStartIn, MESSAGE_START_SIZE) != 0)
        return false;

    // Check the command string for errors
    for (const char* p1 = pchCommand; p1 < pchCommand + COMMAND_SIZE; p1++) {
        if (*p1 == 0) {
            // Must be all zeros after the first zero
            for (; p1 < pchCommand + COMMAND_SIZE; p1++)
                if (*p1 != 0)
                    return false;
        } else if (*p1 < ' ' || *p1 > 0x7E)
            return false;
    }

    // Message size
    if (nMessageSize > MAX_SIZE) {
        LogPrintf("CMessageHeader::IsValid() : (%s, %u bytes) nMessageSize > MAX_SIZE\n", GetCommand(), nMessageSize);
        return false;
    }

    return true;
}


CAddress::CAddress() : CService()
{
    Init();
}

CAddress::CAddress(CService ipIn, ServiceFlags nServicesIn) : CService(ipIn)
{
    Init();
    nServices = nServicesIn;
}

void CAddress::Init()
{
    nServices = NODE_NONE;
    nTime = 100000000;
}

CInv::CInv()
{
    type = 0;
    hash.SetNull();
}

CInv::CInv(int typeIn, const uint256& hashIn)
{
    type = typeIn;
    hash = hashIn;
}

bool operator<(const CInv& a, const CInv& b)
{
    return (a.type < b.type || (a.type == b.type && a.hash < b.hash));
}

bool CInv::IsMasterNodeType() const{
     return type > 2;
}

std::string CInv::GetCommand() const
{
    std::string cmd;
    switch (type) {
        case MSG_TX:                return cmd.append(NetMsgType::TX);
        case MSG_BLOCK:             return cmd.append(NetMsgType::BLOCK);
        case MSG_FILTERED_BLOCK:    return cmd.append(NetMsgType::MERKLEBLOCK);
        case MSG_TXLOCK_REQUEST:    return cmd.append("ix");       // Deprecated
        case MSG_TXLOCK_VOTE:       return cmd.append("txlvote");  // Deprecated
        case MSG_SPORK:             return cmd.append(NetMsgType::SPORK);
        case MSG_MASTERNODE_WINNER: return cmd.append(NetMsgType::MNWINNER);
        case MSG_MASTERNODE_SCANNING_ERROR: return cmd.append("mnodescanerr"); // Deprecated
        case MSG_BUDGET_VOTE: return cmd.append(NetMsgType::BUDGETVOTE);
        case MSG_BUDGET_PROPOSAL: return cmd.append(NetMsgType::BUDGETPROPOSAL);
        case MSG_BUDGET_FINALIZED: return cmd.append(NetMsgType::FINALBUDGET);
        case MSG_BUDGET_FINALIZED_VOTE: return cmd.append(NetMsgType::FINALBUDGETVOTE);
        case MSG_MASTERNODE_QUORUM: return cmd.append("mnq"); // Unused
        case MSG_MASTERNODE_ANNOUNCE: return cmd.append(NetMsgType::MNBROADCAST);
        case MSG_MASTERNODE_PING: return cmd.append(NetMsgType::MNPING);
        case MSG_DSTX: return cmd.append("dstx"); // Deprecated
        default:
            throw std::out_of_range(strprintf("%s: type=%d unknown type", __func__, type));
    }
}


std::string CInv::ToString() const
{
    return strprintf("%s %s", GetCommand(), hash.ToString());
}

const std::vector<std::string>& getAllNetMessageTypes()
{
    return allNetMessageTypesVec;
}

const std::vector<std::string>& getTierTwoNetMessageTypes()
{
    return tiertwoNetMessageTypesVec;
}
