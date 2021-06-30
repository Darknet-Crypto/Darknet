// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin developers
// Copyright (c) 2009-2015 The Dash developers
// Copyright (c) 2015-2021 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chainparamsbase.h"
#include "clientversion.h"
#include "fs.h"
#include "rpc/client.h"
#include "rpc/protocol.h"
#include "util/system.h"
#include "utilstrencodings.h"

#include <stdio.h>
#include <tuple>

#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/keyvalq_struct.h>

#include <univalue.h>

static const char DEFAULT_RPCCONNECT[] = "127.0.0.1";
static const bool DEFAULT_NAMED=false;
static const int DEFAULT_HTTP_CLIENT_TIMEOUT=900;

std::string HelpMessageCli()
{
    const auto defaultBaseParams = CreateBaseChainParams(CBaseChainParams::MAIN);
    const auto testnetBaseParams = CreateBaseChainParams(CBaseChainParams::TESTNET);
    std::string strUsage;
    strUsage += HelpMessageGroup(_("Options:"));
    strUsage += HelpMessageOpt("-?", _("This help message"));
    strUsage += HelpMessageOpt("-conf=<file>", strprintf(_("Specify configuration file (default: %s)"), PIVX_CONF_FILENAME));
    strUsage += HelpMessageOpt("-datadir=<dir>", _("Specify data directory"));
    AppendParamsHelpMessages(strUsage);
    strUsage += HelpMessageOpt("-named", strprintf(_("Pass named instead of positional arguments (default: %s)"), DEFAULT_NAMED));
    strUsage += HelpMessageOpt("-rpcconnect=<ip>", strprintf(_("Send commands to node running on <ip> (default: %s)"), DEFAULT_RPCCONNECT));
    strUsage += HelpMessageOpt("-rpcport=<port>", strprintf(_("Listen for JSON-RPC connections on <port> (default: %u or testnet: %u)"), defaultBaseParams->RPCPort(), testnetBaseParams->RPCPort()));
    strUsage += HelpMessageOpt("-rpcwait", _("Wait for RPC server to start"));
    strUsage += HelpMessageOpt("-rpcuser=<user>", _("Username for JSON-RPC connections"));
    strUsage += HelpMessageOpt("-rpcpassword=<pw>", _("Password for JSON-RPC connections"));
    strUsage += HelpMessageOpt("-rpcclienttimeout=<n>", strprintf(_("Timeout in seconds during HTTP requests, or 0 for no timeout. (default: %d)"), DEFAULT_HTTP_CLIENT_TIMEOUT));
    strUsage += HelpMessageOpt("-rpcwallet=<walletname>", _("Send RPC for non-default wallet on RPC server (needs to exactly match corresponding -wallet option passed to pivxd)"));

    return strUsage;
}

//////////////////////////////////////////////////////////////////////////////
//
// Start
//

//
// Exception thrown on connection error.  This error is used to determine
// when to wait if -rpcwait is given.
//
class CConnectionFailed : public std::runtime_error
{
public:
    explicit inline CConnectionFailed(const std::string& msg) : std::runtime_error(msg)
    {
    }
};

static bool AppInitRPC(int argc, char* argv[])
{
    //
    // Parameters
    //
    gArgs.ParseParameters(argc, argv);
    if (argc < 2 || gArgs.IsArgSet("-?") || gArgs.IsArgSet("-h") || gArgs.IsArgSet("-help") || gArgs.IsArgSet("-version")) {
        std::string strUsage = _("PIVX Core RPC client version") + " " + FormatFullVersion() + "\n";
        if (!gArgs.IsArgSet("-version")) {
            strUsage += "\n" + _("Usage:") + "\n" +
                        "  pivx-cli [options] <command> [params]  " + _("Send command to PIVX Core") + "\n" +
                        "  pivx-cli [options] -named <command> [name=value] ... " + _("Send command to PIVX Core (with named arguments)") + "\n" +
                        "  pivx-cli [options] help                " + _("List commands") + "\n" +
                        "  pivx-cli [options] help <command>      " + _("Get help for a command") + "\n";

            strUsage += "\n" + HelpMessageCli();
        }

        fprintf(stdout, "%s", strUsage.c_str());
        return false;
    }
    if (!fs::is_directory(GetDataDir(false))) {
        fprintf(stderr, "Error: Specified data directory \"%s\" does not exist.\n", gArgs.GetArg("-datadir", "").c_str());
        return false;
    }
    try {
        gArgs.ReadConfigFile(gArgs.GetArg("-conf", PIVX_CONF_FILENAME));
    } catch (const std::exception& e) {
        fprintf(stderr, "Error reading configuration file: %s\n", e.what());
        return false;
    }
    // Check for -testnet or -regtest parameter (BaseParams() calls are only valid after this clause)
    try {
        SelectBaseParams(gArgs.GetChainName());
    } catch(const std::exception& e) {
        fprintf(stderr, "Error: %s\n", e.what());
        return false;
    }
    if (gArgs.GetBoolArg("-rpcssl", false))
    {
        fprintf(stderr, "Error: SSL mode for RPC (-rpcssl) is no longer supported.\n");
        return false;
    }
    return true;
}


/** Reply structure for request_done to fill in */
struct HTTPReply
{
    int status;
    std::string body;
};

static void http_request_done(struct evhttp_request *req, void *ctx)
{
    HTTPReply *reply = static_cast<HTTPReply*>(ctx);

    if (req == NULL) {
        /* If req is NULL, it means an error occurred while connecting, but
         * I'm not sure how to find out which one. We also don't really care.
         */
        reply->status = 0;
        return;
    }

    reply->status = evhttp_request_get_response_code(req);

    struct evbuffer *buf = evhttp_request_get_input_buffer(req);
    if (buf)
    {
        size_t size = evbuffer_get_length(buf);
        const char *data = (const char*)evbuffer_pullup(buf, size);
        if (data)
            reply->body = std::string(data, size);
        evbuffer_drain(buf, size);
    }
}

UniValue CallRPC(const std::string& strMethod, const UniValue& params)
{
    std::string host = gArgs.GetArg("-rpcconnect", DEFAULT_RPCCONNECT);
    int port = gArgs.GetArg("-rpcport", BaseParams().RPCPort());

    // Create event base
    struct event_base *base = event_base_new(); // TODO RAII
    if (!base)
        throw std::runtime_error("cannot create event_base");

    // Synchronously look up hostname
    struct evhttp_connection *evcon = evhttp_connection_base_new(base, NULL, host.c_str(), port); // TODO RAII
    if (evcon == NULL)
        throw std::runtime_error("create connection failed");
    evhttp_connection_set_timeout(evcon, gArgs.GetArg("-rpcclienttimeout", DEFAULT_HTTP_CLIENT_TIMEOUT));

    HTTPReply response;
    struct evhttp_request *req = evhttp_request_new(http_request_done, (void*)&response); // TODO RAII
    if (req == NULL)
        throw std::runtime_error("create http request failed");

    // Get credentials
    std::string strRPCUserColonPass;
    if (gArgs.GetArg("-rpcpassword", "") == "") {
        // Try fall back to cookie-based authentication if no password is provided
        if (!GetAuthCookie(&strRPCUserColonPass)) {
            throw std::runtime_error(strprintf(
                 _("Could not locate RPC credentials. No authentication cookie could be found, and no rpcpassword is set in the configuration file (%s)"),
                    GetConfigFile(gArgs.GetArg("-conf", PIVX_CONF_FILENAME)).string().c_str()));

        }
    } else {
        strRPCUserColonPass = gArgs.GetArg("-rpcuser", "") + ":" + gArgs.GetArg("-rpcpassword", "");
    }

    struct evkeyvalq *output_headers = evhttp_request_get_output_headers(req);
    assert(output_headers);
    evhttp_add_header(output_headers, "Host", host.c_str());
    evhttp_add_header(output_headers, "Connection", "close");
    evhttp_add_header(output_headers, "Authorization", (std::string("Basic ") + EncodeBase64(strRPCUserColonPass)).c_str());

    // Attach request data
    std::string strRequest = JSONRPCRequestObj(strMethod, params, 1).write() + "\n";
    struct evbuffer * output_buffer = evhttp_request_get_output_buffer(req);
    assert(output_buffer);
    evbuffer_add(output_buffer, strRequest.data(), strRequest.size());

    // check if we should use a special wallet endpoint
    std::string endpoint = "/";
    if (!gArgs.GetArgs("-rpcwallet").empty()) {
        std::string walletName = gArgs.GetArg("-rpcwallet", "");
        char* encodedURI = evhttp_uriencode(walletName.c_str(), walletName.size(), false);
        if (encodedURI) {
            endpoint = "/wallet/"+ std::string(encodedURI);
            free(encodedURI);
        } else {
            throw CConnectionFailed("uri-encode failed");
        }
    }
    int r = evhttp_make_request(evcon, req, EVHTTP_REQ_POST, endpoint.c_str());
    if (r != 0) {
        evhttp_connection_free(evcon);
        event_base_free(base);
        throw CConnectionFailed("send http request failed");
    }

    event_base_dispatch(base);
    evhttp_connection_free(evcon);
    event_base_free(base);

    if (response.status == 0)
        throw CConnectionFailed("couldn't connect to server");
    else if (response.status == HTTP_UNAUTHORIZED)
        throw std::runtime_error("incorrect rpcuser or rpcpassword (authorization failed)");
    else if (response.status >= 400 && response.status != HTTP_BAD_REQUEST && response.status != HTTP_NOT_FOUND && response.status != HTTP_INTERNAL_SERVER_ERROR)
        throw std::runtime_error(strprintf("server returned HTTP error %d", response.status));
    else if (response.body.empty())
        throw std::runtime_error("no response from server");

    // Parse reply
    UniValue valReply(UniValue::VSTR);
    if (!valReply.read(response.body))
        throw std::runtime_error("couldn't parse reply from server");
    const UniValue& reply = valReply.get_obj();
    if (reply.empty())
        throw std::runtime_error("expected reply to have result, error and id properties");

    return reply;
}

int CommandLineRPC(int argc, char* argv[])
{
    std::string strPrint;
    int nRet = 0;
    try {
        // Skip switches
        while (argc > 1 && IsSwitchChar(argv[1][0])) {
            argc--;
            argv++;
        }

        // Method
        if (argc < 2)
            throw std::runtime_error("too few parameters");
        std::string strMethod = argv[1];

        // Parameters default to strings
        std::vector<std::string> strParams(&argv[2], &argv[argc]);
        UniValue params;
        if(gArgs.GetBoolArg("-named", DEFAULT_NAMED)) {
            params = RPCConvertNamedValues(strMethod, strParams);
        } else {
            params = RPCConvertValues(strMethod, strParams);
        }

        // Execute and handle connection failures with -rpcwait
        const bool fWait = gArgs.GetBoolArg("-rpcwait", false);
        do {
            try {
                const UniValue reply = CallRPC(strMethod, params);

                // Parse reply
                const UniValue& result = find_value(reply, "result");
                const UniValue& error = find_value(reply, "error");

                if (!error.isNull()) {
                    // Error
                    int code = error["code"].get_int();
                    if (fWait && code == RPC_IN_WARMUP)
                        throw CConnectionFailed("server in warmup");
                    strPrint = "error: " + error.write();
                    nRet = abs(code);
                    if (error.isObject()) {
                        UniValue errCode = find_value(error, "code");
                        UniValue errMsg  = find_value(error, "message");
                        strPrint = errCode.isNull() ? "" : "error code: "+errCode.getValStr()+"\n";

                        if (errMsg.isStr())
                            strPrint += "error message:\n"+errMsg.get_str();

                        if (errCode.isNum() && errCode.get_int() == RPC_WALLET_NOT_SPECIFIED) {
                            strPrint += "\nTry adding \"-rpcwallet=<filename>\" option to pivx-cli command line.";
                        }
                    }
                } else {
                    // Result
                    if (result.isNull())
                        strPrint = "";
                    else if (result.isStr())
                        strPrint = result.get_str();
                    else
                        strPrint = result.write(2);
                }
                // Connection succeeded, no need to retry.
                break;
            } catch (const CConnectionFailed& e) {
                if (fWait)
                    MilliSleep(1000);
                else
                    throw;
            }
        } while (fWait);
    } catch (const boost::thread_interrupted&) {
        throw;
    } catch (const std::exception& e) {
        strPrint = std::string("error: ") + e.what();
        nRet = EXIT_FAILURE;
    } catch (...) {
        PrintExceptionContinue(NULL, "CommandLineRPC()");
        throw;
    }

    if (strPrint != "") {
        fprintf((nRet == 0 ? stdout : stderr), "%s\n", strPrint.c_str());
    }
    return nRet;
}

int main(int argc, char* argv[])
{
#ifdef WIN32
    util::WinCmdLineArgs winArgs;
    std::tie(argc, argv) = winArgs.get();
#endif
    SetupEnvironment();
    if (!SetupNetworking()) {
        fprintf(stderr, "Error: Initializing networking failed\n");
        exit(1);
    }

    try {
        if (!AppInitRPC(argc, argv))
            return EXIT_FAILURE;
    } catch (const std::exception& e) {
        PrintExceptionContinue(&e, "AppInitRPC()");
        return EXIT_FAILURE;
    } catch (...) {
        PrintExceptionContinue(NULL, "AppInitRPC()");
        return EXIT_FAILURE;
    }

    int ret = EXIT_FAILURE;
    try {
        ret = CommandLineRPC(argc, argv);
    } catch (const std::exception& e) {
        PrintExceptionContinue(&e, "CommandLineRPC()");
    } catch (...) {
        PrintExceptionContinue(NULL, "CommandLineRPC()");
    }
    return ret;
}
