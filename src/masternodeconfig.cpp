// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2019 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.
#include "masternodeconfig.h"

#include "fs.h"
#include "netbase.h"
#include "util/system.h"
#include "guiinterface.h"
#include <base58.h>

CMasternodeConfig masternodeConfig;

CMasternodeConfig::CMasternodeEntry* CMasternodeConfig::add(std::string alias, std::string ip, std::string privKey, std::string txHash, std::string outputIndex)
{
    CMasternodeEntry cme(alias, ip, privKey, txHash, outputIndex);
    entries.push_back(cme);
    return &(entries[entries.size()-1]);
}

void CMasternodeConfig::remove(std::string alias) {
    LOCK(cs_entries);
    int pos = -1;
    for (int i = 0; i < ((int) entries.size()); ++i) {
        CMasternodeEntry e = entries[i];
        if (e.getAlias() == alias) {
            pos = i;
            break;
        }
    }
    entries.erase(entries.begin() + pos);
}

bool CMasternodeConfig::read(std::string& strErr)
{
    LOCK(cs_entries);
    int linenumber = 1;
    fs::path pathMasternodeConfigFile = GetMasternodeConfigFile();
    fsbridge::ifstream streamConfig(pathMasternodeConfigFile);

    if (!streamConfig.good()) {
        FILE* configFile = fsbridge::fopen(pathMasternodeConfigFile, "a");
        if (configFile != NULL) {
            std::string strHeader = "# Masternode config file\n"
                                    "# Format: alias IP:port masternodeprivkey collateral_output_txid collateral_output_index\n"
                                    "# Example: mn1 127.0.0.2:51472 93HaYBVUCYjEMeeH1Y4sBGLALQZE1Yc1K64xiqgX37tGBDQL8Xg 2bcd3c84c84f87eaa86e4e56834c92927a07f9e18718810b92e0d0324456a67c 0"
                                    "#\n";
            fwrite(strHeader.c_str(), std::strlen(strHeader.c_str()), 1, configFile);
            fclose(configFile);
        }
        return true; // Nothing to read, so just return
    }

    for (std::string line; std::getline(streamConfig, line); linenumber++) {
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string comment, alias, ip, privKey, txHash, outputIndex;

        if (iss >> comment) {
            if (comment.at(0) == '#') continue;
            iss.str(line);
            iss.clear();
        }

        if (!(iss >> alias >> ip >> privKey >> txHash >> outputIndex)) {
            iss.str(line);
            iss.clear();
            if (!(iss >> alias >> ip >> privKey >> txHash >> outputIndex)) {
                strErr = _("Could not parse masternode.conf") + "\n" +
                         strprintf(_("Line: %d"), linenumber) + "\n\"" + line + "\"";
                streamConfig.close();
                return false;
            }
        }

        int port = 0;
        int nDefaultPort = Params().GetDefaultPort();
        std::string hostname = "";
        SplitHostPort(ip, port, hostname);
        if(port == 0 || hostname == "") {
            strErr = _("Failed to parse host:port string") + "\n"+
                     strprintf(_("Line: %d"), linenumber) + "\n\"" + line + "\"";
            streamConfig.close();
            return false;
        }

        if (port != nDefaultPort && !Params().IsRegTestNet()) {
            strErr = strprintf(_("Invalid port %d detected in masternode.conf"), port) + "\n" +
                     strprintf(_("Line: %d"), linenumber) + "\n\"" + ip + "\"" + "\n" +
                     strprintf(_("(must be %d for %s-net)"), nDefaultPort, Params().NetworkIDString());
            streamConfig.close();
            return false;
        }


        add(alias, ip, privKey, txHash, outputIndex);
    }

    streamConfig.close();
    return true;
}

bool CMasternodeConfig::CMasternodeEntry::castOutputIndex(int &n) const
{
    try {
        n = std::stoi(outputIndex);
    } catch (const std::exception& e) {
        LogPrintf("%s: %s on getOutputIndex\n", __func__, e.what());
        return false;
    }

    return true;
}
