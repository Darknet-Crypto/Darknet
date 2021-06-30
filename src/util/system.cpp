// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include "config/pivx-config.h"
#endif

#include "util/system.h"

#include "chainparamsbase.h"
#include "random.h"
#include "support/allocators/zeroafterfree.h"
#include "utilstrencodings.h"
#include "utiltime.h"

#include <librustzcash.h>

#include <stdarg.h>
#include <thread>

#ifndef WIN32
// for posix_fallocate
#ifdef __linux__

#ifdef _POSIX_C_SOURCE
#undef _POSIX_C_SOURCE
#endif

#define _POSIX_C_SOURCE 200112L

#endif // __linux__

#include <algorithm>
#include <fcntl.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>

#else

#ifdef _MSC_VER
#pragma warning(disable : 4786)
#pragma warning(disable : 4804)
#pragma warning(disable : 4805)
#pragma warning(disable : 4717)
#endif

#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0501

#ifdef _WIN32_IE
#undef _WIN32_IE
#endif
#define _WIN32_IE 0x0501

#define WIN32_LEAN_AND_MEAN 1
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <codecvt>

#include <io.h> /* for _commit */
#include <shellapi.h>
#include <shlobj.h>
#endif

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#ifdef MAC_OSX
#include <CoreFoundation/CoreFoundation.h>
#endif

const char * const PIVX_CONF_FILENAME = "pivx.conf";
const char * const PIVX_PID_FILENAME = "pivx.pid";
const char * const PIVX_MASTERNODE_CONF_FILENAME = "masternode.conf";


// PIVX only features
// Masternode
std::atomic<bool> fMasterNode{false};
std::string strMasterNodeAddr = "";
bool fLiteMode = false;
// budget finalization
std::string strBudgetMode = "";

ArgsManager gArgs;

bool fDaemon = false;
CTranslationInterface translationInterface;

bool CheckDiskSpace(const fs::path& dir, uint64_t additional_bytes)
{
    constexpr uint64_t min_disk_space = 52428800; // 50 MiB

    uint64_t free_bytes_available = fs::space(dir).available;
    return free_bytes_available >= min_disk_space + additional_bytes;
}

/** A map that contains all the currently held directory locks. After
 * successful locking, these will be held here until the global destructor
 * cleans them up and thus automatically unlocks them, or ReleaseDirectoryLocks
 * is called.
 */
static std::map<std::string, std::unique_ptr<fsbridge::FileLock>> dir_locks;
/** Mutex to protect dir_locks. */
static std::mutex cs_dir_locks;

bool LockDirectory(const fs::path& directory, const std::string& lockfile_name, bool probe_only)
{
    std::lock_guard<std::mutex> ulock(cs_dir_locks);
    fs::path pathLockFile = directory / lockfile_name;

    // If a lock for this directory already exists in the map, don't try to re-lock it
    if (dir_locks.count(pathLockFile.string())) {
        return true;
    }

    // Create empty lock file if it doesn't exist.
    FILE* file = fsbridge::fopen(pathLockFile, "a");
    if (file) fclose(file);
    auto lock = std::make_unique<fsbridge::FileLock>(pathLockFile);
    if (!lock->TryLock()) {
        return error("Error while attempting to lock directory %s: %s", directory.string(), lock->GetReason());
    }
    if (!probe_only) {
        // Lock successful and we're not just probing, put it into the map
        dir_locks.emplace(pathLockFile.string(), std::move(lock));
    }
    return true;
}

void ReleaseDirectoryLocks()
{
    std::lock_guard<std::mutex> ulock(cs_dir_locks);
    dir_locks.clear();
}

bool DirIsWritable(const fs::path& directory)
{
    fs::path tmpFile = directory / fs::unique_path();

    FILE* file = fsbridge::fopen(tmpFile, "a");
    if (!file) return false;

    fclose(file);
    remove(tmpFile);

    return true;
}

/**
 * Interpret a string argument as a boolean.
 *
 * The definition of atoi() requires that non-numeric string values like "foo",
 * return 0. This means that if a user unintentionally supplies a non-integer
 * argument here, the return value is always false. This means that -foo=false
 * does what the user probably expects, but -foo=true is well defined but does
 * not do what they probably expected.
 *
 * The return value of atoi() is undefined when given input not representable as
 * an int. On most systems this means string value between "-2147483648" and
 * "2147483647" are well defined (this method will return true). Setting
 * -txindex=2147483648 on most systems, however, is probably undefined.
 *
 * For a more extensive discussion of this topic (and a wide range of opinions
 * on the Right Way to change this code), see upstream PR12713.
 */
static bool InterpretBool(const std::string& strValue)
{
    if (strValue.empty())
        return true;
    return (atoi(strValue) != 0);
}

/** Internal helper functions for ArgsManager */
class ArgsManagerHelper {
public:
    typedef std::map<std::string, std::vector<std::string>> MapArgs;

    /** Determine whether to use config settings in the default section,
     *  See also comments around ArgsManager::ArgsManager() below. */
    static inline bool UseDefaultSection(const ArgsManager& am, const std::string& arg)
    {
        return (am.m_network == CBaseChainParams::MAIN || am.m_network_only_args.count(arg) == 0);
    }

    /** Convert regular argument into the network-specific setting */
    static inline std::string NetworkArg(const ArgsManager& am, const std::string& arg)
    {
        assert(arg.length() > 1 && arg[0] == '-');
        return "-" + am.m_network + "." + arg.substr(1);
    }

    /** Find arguments in a map and add them to a vector */
    static inline void AddArgs(std::vector<std::string>& res, const MapArgs& map_args, const std::string& arg)
    {
        auto it = map_args.find(arg);
        if (it != map_args.end()) {
            res.insert(res.end(), it->second.begin(), it->second.end());
        }
    }

    /** Return true/false if an argument is set in a map, and also
     *  return the first (or last) of the possibly multiple values it has
     */
    static inline std::pair<bool,std::string> GetArgHelper(const MapArgs& map_args, const std::string& arg, bool getLast = false)
    {
        auto it = map_args.find(arg);

        if (it == map_args.end() || it->second.empty()) {
            return std::make_pair(false, std::string());
        }

        if (getLast) {
            return std::make_pair(true, it->second.back());
        } else {
            return std::make_pair(true, it->second.front());
        }
    }

    /* Get the string value of an argument, returning a pair of a boolean
     * indicating the argument was found, and the value for the argument
     * if it was found (or the empty string if not found).
     */
    static inline std::pair<bool,std::string> GetArg(const ArgsManager& am, const std::string& arg)
    {
        LOCK(am.cs_args);
        std::pair<bool,std::string> found_result(false, std::string());

        // We pass "true" to GetArgHelper in order to return the last
        // argument value seen from the command line (so "pivxd -foo=bar
        // -foo=baz" gives GetArg(am,"foo")=={true,"baz"}
        found_result = GetArgHelper(am.m_override_args, arg, true);
        if (found_result.first) {
            return found_result;
        }

        // But in contrast we return the first argument seen in a config file,
        // so "foo=bar \n foo=baz" in the config file gives
        // GetArg(am,"foo")={true,"bar"}
        if (!am.m_network.empty()) {
            found_result = GetArgHelper(am.m_config_args, NetworkArg(am, arg));
            if (found_result.first) {
                return found_result;
            }
        }

        if (UseDefaultSection(am, arg)) {
            found_result = GetArgHelper(am.m_config_args, arg);
            if (found_result.first) {
                return found_result;
            }
        }

        return found_result;
    }

    /* Special test for -testnet and -regtest args, because we
     * don't want to be confused by craziness like "[regtest] testnet=1"
     */
    static inline bool GetNetBoolArg(const ArgsManager &am, const std::string& net_arg)
    {
        std::pair<bool,std::string> found_result(false,std::string());
        found_result = GetArgHelper(am.m_override_args, net_arg, true);
        if (!found_result.first) {
            found_result = GetArgHelper(am.m_config_args, net_arg, true);
            if (!found_result.first) {
                return false; // not set
            }
        }
        return InterpretBool(found_result.second); // is set, so evaluate
    }
};

/**
 * Interpret -nofoo as if the user supplied -foo=0.
 *
 * This method also tracks when the -no form was supplied, and if so,
 * checks whether there was a double-negative (-nofoo=0 -> -foo=1).
 *
 * If there was not a double negative, it removes the "no" from the key,
 * and returns true, indicating the caller should clear the args vector
 * to indicate a negated option.
 *
 * If there was a double negative, it removes "no" from the key, sets the
 * value to "1" and returns false.
 *
 * If there was no "no", it leaves key and value untouched and returns
 * false.
 *
 * Where an option was negated can be later checked using the
 * IsArgNegated() method. One use case for this is to have a way to disable
 * options that are not normally boolean (e.g. using -nodebuglogfile to request
 * that debug log output is not sent to any file at all).
 */
static bool InterpretNegatedOption(std::string& key, std::string& val)
{
    assert(key[0] == '-');

    size_t option_index = key.find('.');
    if (option_index == std::string::npos) {
        option_index = 1;
    } else {
        ++option_index;
    }
    if (key.substr(option_index, 2) == "no") {
        bool bool_val = InterpretBool(val);
        key.erase(option_index, 2);
        if (!bool_val ) {
            // Double negatives like -nofoo=0 are supported (but discouraged)
            LogPrintf("Warning: parsed potentially confusing double-negative %s=%s\n", key, val);
            val = "1";
        } else {
            return true;
        }
    }
    return false;
}

ArgsManager::ArgsManager() :
    /* These options would cause cross-contamination if values for
     * mainnet were used while running on regtest/testnet (or vice-versa).
     * Setting them as section_only_args ensures that sharing a config file
     * between mainnet and regtest/testnet won't cause problems due to these
     * parameters by accident. */
    m_network_only_args{
      "-addnode", "-connect",
      "-port", "-bind",
      "-rpcport", "-rpcbind",
      "-wallet",
    }
{
    // nothing to do
}

void ArgsManager::WarnForSectionOnlyArgs()
{
    // if there's no section selected, don't worry
    if (m_network.empty()) return;

    // if it's okay to use the default section for this network, don't worry
    if (m_network == CBaseChainParams::MAIN) return;

    for (const auto& arg : m_network_only_args) {
        std::pair<bool, std::string> found_result;

        // if this option is overridden it's fine
        found_result = ArgsManagerHelper::GetArgHelper(m_override_args, arg);
        if (found_result.first) continue;

        // if there's a network-specific value for this option, it's fine
        found_result = ArgsManagerHelper::GetArgHelper(m_config_args, ArgsManagerHelper::NetworkArg(*this, arg));
        if (found_result.first) continue;

        // if there isn't a default value for this option, it's fine
        found_result = ArgsManagerHelper::GetArgHelper(m_config_args, arg);
        if (!found_result.first) continue;

        // otherwise, issue a warning
        LogPrintf("Warning: Config setting for %s only applied on %s network when in [%s] section.\n", arg, m_network, m_network);
    }
}

void ArgsManager::SelectConfigNetwork(const std::string& network)
{
    m_network = network;
}

void ArgsManager::ParseParameters(int argc, const char* const argv[])
{
    LOCK(cs_args);
    m_override_args.clear();

    for (int i = 1; i < argc; i++) {
        std::string key(argv[i]);
        std::string val;
        size_t is_index = key.find('=');
        if (is_index != std::string::npos) {
            val = key.substr(is_index + 1);
            key.erase(is_index);
        }
#ifdef WIN32
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        if (key[0] == '/')
            key[0] = '-';
#endif

        if (key[0] != '-')
            break;

        // Transform --foo to -foo
        if (key.length() > 1 && key[1] == '-')
            key.erase(0, 1);

        // Check for -nofoo
        if (InterpretNegatedOption(key, val)) {
            m_override_args[key].clear();
        } else {
            m_override_args[key].push_back(val);
        }
    }
}

std::vector<std::string> ArgsManager::GetArgs(const std::string& strArg) const
{
    std::vector<std::string> result = {};
    if (IsArgNegated(strArg)) return result; // special case

    LOCK(cs_args);

    ArgsManagerHelper::AddArgs(result, m_override_args, strArg);
    if (!m_network.empty()) {
        ArgsManagerHelper::AddArgs(result, m_config_args, ArgsManagerHelper::NetworkArg(*this, strArg));
    }

    if (ArgsManagerHelper::UseDefaultSection(*this, strArg)) {
        ArgsManagerHelper::AddArgs(result, m_config_args, strArg);
    }

    return result;
}

bool ArgsManager::IsArgSet(const std::string& strArg) const
{
    if (IsArgNegated(strArg)) return true; // special case
    return ArgsManagerHelper::GetArg(*this, strArg).first;
}

bool ArgsManager::IsArgNegated(const std::string& strArg) const
{
    LOCK(cs_args);

    const auto& ov = m_override_args.find(strArg);
    if (ov != m_override_args.end()) return ov->second.empty();

    if (!m_network.empty()) {
        const auto& cfs = m_config_args.find(ArgsManagerHelper::NetworkArg(*this, strArg));
        if (cfs != m_config_args.end()) return cfs->second.empty();
    }

    const auto& cf = m_config_args.find(strArg);
    if (cf != m_config_args.end()) return cf->second.empty();

    return false;
}

std::string ArgsManager::GetArg(const std::string& strArg, const std::string& strDefault) const
{
    if (IsArgNegated(strArg)) return "0";
    std::pair<bool,std::string> found_res = ArgsManagerHelper::GetArg(*this, strArg);
    if (found_res.first) return found_res.second;
    return strDefault;
}

int64_t ArgsManager::GetArg(const std::string& strArg, int64_t nDefault) const
{
    if (IsArgNegated(strArg)) return 0;
    std::pair<bool,std::string> found_res = ArgsManagerHelper::GetArg(*this, strArg);
    if (found_res.first) return atoi64(found_res.second);
    return nDefault;
}

bool ArgsManager::GetBoolArg(const std::string& strArg, bool fDefault) const
{
    if (IsArgNegated(strArg)) return false;
    std::pair<bool,std::string> found_res = ArgsManagerHelper::GetArg(*this, strArg);
    if (found_res.first) return InterpretBool(found_res.second);
    return fDefault;
}

bool ArgsManager::SoftSetArg(const std::string& strArg, const std::string& strValue)
{
    LOCK(cs_args);
    if (IsArgSet(strArg)) return false;;
    ForceSetArg(strArg, strValue);
    return true;
}

bool ArgsManager::SoftSetBoolArg(const std::string& strArg, bool fValue)
{
    if (fValue)
        return SoftSetArg(strArg, std::string("1"));
    else
        return SoftSetArg(strArg, std::string("0"));
}

void ArgsManager::ForceSetArg(const std::string& strArg, const std::string& strValue)
{
    LOCK(cs_args);
    m_override_args[strArg] = {strValue};
}

static const int screenWidth = 79;
static const int optIndent = 2;
static const int msgIndent = 7;

std::string HelpMessageGroup(const std::string &message) {
    return std::string(message) + std::string("\n\n");
}

std::string HelpMessageOpt(const std::string &option, const std::string &message) {
    return std::string(optIndent,' ') + std::string(option) +
           std::string("\n") + std::string(msgIndent,' ') +
           FormatParagraph(message, screenWidth - msgIndent, msgIndent) +
           std::string("\n\n");
}

static std::string FormatException(const std::exception* pex, const char* pszThread)
{
#ifdef WIN32
    char pszModule[MAX_PATH] = "";
    GetModuleFileNameA(NULL, pszModule, sizeof(pszModule));
#else
    const char* pszModule = "pivx";
#endif
    if (pex)
        return strprintf(
            "EXCEPTION: %s       \n%s       \n%s in %s       \n", typeid(*pex).name(), pex->what(), pszModule, pszThread);
    else
        return strprintf(
            "UNKNOWN EXCEPTION       \n%s in %s       \n", pszModule, pszThread);
}

void PrintExceptionContinue(const std::exception* pex, const char* pszThread)
{
    std::string message = FormatException(pex, pszThread);
    LogPrintf("\n\n************************\n%s\n", message);
    fprintf(stderr, "\n\n************************\n%s\n", message.c_str());
}

fs::path GetDefaultDataDir()
{
// Windows < Vista: C:\Documents and Settings\Username\Application Data\PIVX
// Windows >= Vista: C:\Users\Username\AppData\Roaming\PIVX
// Mac: ~/Library/Application Support/PIVX
// Unix: ~/.pivx
#ifdef WIN32
    // Windows
    return GetSpecialFolderPath(CSIDL_APPDATA) / "PIVX";
#else
    fs::path pathRet;
    char* pszHome = getenv("HOME");
    if (pszHome == NULL || strlen(pszHome) == 0)
        pathRet = fs::path("/");
    else
        pathRet = fs::path(pszHome);
#ifdef MAC_OSX
    // Mac
    pathRet /= "Library/Application Support";
    TryCreateDirectories(pathRet);
    return pathRet / "PIVX";
#else
    // Unix
    return pathRet / ".pivx";
#endif
#endif
}

static fs::path g_blocks_path_cache_net_specific;
static fs::path pathCached;
static fs::path pathCachedNetSpecific;
static fs::path zc_paramsPathCached;
static RecursiveMutex csPathCached;

static fs::path ZC_GetBaseParamsDir()
{
    // Copied from GetDefaultDataDir and adapter for zcash params.
    // Windows < Vista: C:\Documents and Settings\Username\Application Data\PIVXParams
    // Windows >= Vista: C:\Users\Username\AppData\Roaming\PIVXParams
    // Mac: ~/Library/Application Support/PIVXParams
    // Unix: ~/.pivx-params
#ifdef WIN32
    // Windows
    return GetSpecialFolderPath(CSIDL_APPDATA) / "PIVXParams";
#else
    fs::path pathRet;
    char* pszHome = getenv("HOME");
    if (pszHome == NULL || strlen(pszHome) == 0)
        pathRet = fs::path("/");
    else
        pathRet = fs::path(pszHome);
#ifdef MAC_OSX
    // Mac
    pathRet /= "Library/Application Support";
    TryCreateDirectories(pathRet);
    return pathRet / "PIVXParams";
#else
    // Unix
    return pathRet / ".pivx-params";
#endif
#endif
}

const fs::path &ZC_GetParamsDir()
{
    LOCK(csPathCached); // Reuse the same lock as upstream.

    fs::path &path = zc_paramsPathCached;

    // This can be called during exceptions by LogPrintf(), so we cache the
    // value so we don't have to do memory allocations after that.
    if (!path.empty())
        return path;

#ifdef USE_CUSTOM_PARAMS
    path = fs::system_complete(PARAMS_DIR);
#else
    if (gArgs.IsArgSet("-paramsdir")) {
        path = fs::system_complete(gArgs.GetArg("-paramsdir", ""));
        if (!fs::is_directory(path)) {
            path = "";
            return path;
        }
    } else {
        path = ZC_GetBaseParamsDir();
    }
#endif

    return path;
}

void initZKSNARKS()
{
    const fs::path& path = ZC_GetParamsDir();
    fs::path sapling_spend = path / "sapling-spend.params";
    fs::path sapling_output = path / "sapling-output.params";

    bool fParamsFound = false;
    if (fs::exists(sapling_spend) && fs::exists(sapling_output)) {
        fParamsFound = true;
    } else {
#ifdef MAC_OSX
        // macOS fallback path for params located within the app bundle
        // This is a somewhat convoluted series of CoreFoundation calls
        // that will result in the full path to the app bundle's "Resources"
        // directory, which will contain the sapling params.
        LogPrintf("Attempting to find params in app bundle...\n");
        CFBundleRef mainBundle = CFBundleGetMainBundle();
        CFURLRef bundleURL = CFBundleCopyBundleURL(mainBundle);

        CFStringRef strBundlePath = CFURLCopyFileSystemPath(bundleURL, kCFURLPOSIXPathStyle);
        const char* pathBundle = CFStringGetCStringPtr(strBundlePath, CFStringGetSystemEncoding());

        fs::path bundle_path = fs::path(pathBundle);
        LogPrintf("App bundle Resources path: %s\n", bundle_path);
        sapling_spend = bundle_path / "Contents/Resources/sapling-spend.params";
        sapling_output = bundle_path / "Contents/Resources/sapling-output.params";

        // Release the CF objects
        CFRelease(strBundlePath);
        CFRelease(bundleURL);
        CFRelease(mainBundle);
#else
        // Linux fallback path for debuild/ppa based installs
        sapling_spend = "/usr/share/pivx/sapling-spend.params";
        sapling_output = "/usr/share/pivx/sapling-output.params";
        if (fs::exists(sapling_spend) && fs::exists(sapling_output)) {
            fParamsFound = true;
        } else {
            // Linux fallback for local installs
            sapling_spend = "/usr/local/share/pivx/sapling-spend.params";
            sapling_output = "/usr/local/share/pivx/sapling-output.params";
        }
#endif
        if (fs::exists(sapling_spend) && fs::exists(sapling_output))
            fParamsFound = true;
    }
    if (!fParamsFound)
        throw std::runtime_error("Sapling params don't exist");

    static_assert(
        sizeof(fs::path::value_type) == sizeof(codeunit),
        "librustzcash not configured correctly");
    auto sapling_spend_str = sapling_spend.native();
    auto sapling_output_str = sapling_output.native();

    //LogPrintf("Loading Sapling (Spend) parameters from %s\n", sapling_spend.string().c_str());

    librustzcash_init_zksnark_params(
        reinterpret_cast<const codeunit*>(sapling_spend_str.c_str()),
        sapling_spend_str.length(),
        "8270785a1a0d0bc77196f000ee6d221c9c9894f55307bd9357c3f0105d31ca63991ab91324160d8f53e2bbd3c2633a6eb8bdf5205d822e7f3f73edac51b2b70c",
        reinterpret_cast<const codeunit*>(sapling_output_str.c_str()),
        sapling_output_str.length(),
        "657e3d38dbb5cb5e7dd2970e8b03d69b4787dd907285b5a7f0790dcc8072f60bf593b32cc2d1c030e00ff5ae64bf84c5c3beb84ddc841d48264b4a171744d028",
        nullptr,    // sprout_path
        0,          // sprout_path_len
        ""          // sprout_hash
    );

    //std::cout << "### Sapling params initialized ###" << std::endl;
}

const fs::path &GetBlocksDir()
{

    LOCK(csPathCached);

    fs::path &path = g_blocks_path_cache_net_specific;

    // This can be called during exceptions by LogPrintf(), so we cache the
    // value so we don't have to do memory allocations after that.
    if (!path.empty())
        return path;

    if (gArgs.IsArgSet("-blocksdir")) {
        path = fs::system_complete(gArgs.GetArg("-blocksdir", ""));
        if (!fs::is_directory(path)) {
            path = "";
            return path;
        }
    } else {
        path = GetDataDir(false);
    }

    path /= BaseParams().DataDir();
    path /= "blocks";
    fs::create_directories(path);
    return path;
}

const fs::path& GetDataDir(bool fNetSpecific)
{
    LOCK(csPathCached);

    fs::path& path = fNetSpecific ? pathCachedNetSpecific : pathCached;

    // This can be called during exceptions by LogPrintf(), so we cache the
    // value so we don't have to do memory allocations after that.
    if (!path.empty())
        return path;

    if (gArgs.IsArgSet("-datadir")) {
        path = fs::system_complete(gArgs.GetArg("-datadir", ""));
        if (!fs::is_directory(path)) {
            path = "";
            return path;
        }
    } else {
        path = GetDefaultDataDir();
    }
    if (fNetSpecific)
        path /= BaseParams().DataDir();

    if (fs::create_directories(path)) {
        // This is the first run, create wallets subdirectory too
        fs::create_directories(path / "wallets");
    }

    return path;
}

void ClearDatadirCache()
{
    LOCK(csPathCached);

    pathCached = fs::path();
    pathCachedNetSpecific = fs::path();
    g_blocks_path_cache_net_specific = fs::path();
}

fs::path GetConfigFile(const std::string& confPath)
{
    fs::path pathConfigFile(confPath);
    return AbsPathForConfigVal(pathConfigFile, false);
}

fs::path GetMasternodeConfigFile()
{
    fs::path pathConfigFile(gArgs.GetArg("-mnconf", PIVX_MASTERNODE_CONF_FILENAME));
    return AbsPathForConfigVal(pathConfigFile);
}

static std::string TrimString(const std::string& str, const std::string& pattern)
{
    std::string::size_type front = str.find_first_not_of(pattern);
    if (front == std::string::npos) {
        return std::string();
    }
    std::string::size_type end = str.find_last_not_of(pattern);
    return str.substr(front, end - front + 1);
}

static std::vector<std::pair<std::string, std::string>> GetConfigOptions(std::istream& stream)
{
    std::vector<std::pair<std::string, std::string>> options;
    std::string str, prefix;
    std::string::size_type pos;
    while (std::getline(stream, str)) {
        if ((pos = str.find('#')) != std::string::npos) {
            str = str.substr(0, pos);
        }
        const static std::string pattern = " \t\r\n";
        str = TrimString(str, pattern);
        if (!str.empty()) {
            if (*str.begin() == '[' && *str.rbegin() == ']') {
                prefix = str.substr(1, str.size() - 2) + '.';
            } else if ((pos = str.find('=')) != std::string::npos) {
                std::string name = prefix + TrimString(str.substr(0, pos), pattern);
                std::string value = TrimString(str.substr(pos + 1), pattern);
                options.emplace_back(name, value);
            }
        }
    }
    return options;
}

void ArgsManager::ReadConfigStream(std::istream& stream)
{
    LOCK(cs_args);

    for (const std::pair<std::string, std::string>& option : GetConfigOptions(stream)) {
        std::string strKey = std::string("-") + option.first;
        std::string strValue = option.second;

        if (InterpretNegatedOption(strKey, strValue)) {
            m_config_args[strKey].clear();
        } else {
            m_config_args[strKey].push_back(strValue);
        }
    }
}

void ArgsManager::ReadConfigFile(const std::string& confPath)
{
    {
        LOCK(cs_args);
        m_config_args.clear();
    }

    fsbridge::ifstream stream(GetConfigFile(confPath));

    // ok to not have a config file
    if (stream.good()) {
        ReadConfigStream(stream);
    }

    // If datadir is changed in .conf file:
    ClearDatadirCache();
    if (!fs::is_directory(GetDataDir(false))) {
        throw std::runtime_error(strprintf("specified data directory \"%s\" does not exist.", gArgs.GetArg("-datadir", "").c_str()));
    }
}

fs::path AbsPathForConfigVal(const fs::path& path, bool net_specific)
{
    if (path.is_absolute()) {
        return path;
    }
    return fs::absolute(path, GetDataDir(net_specific));
}

std::string ArgsManager::GetChainName() const
{
    bool fRegTest = ArgsManagerHelper::GetNetBoolArg(*this, "-regtest");
    bool fTestNet = ArgsManagerHelper::GetNetBoolArg(*this, "-testnet");

    if (fTestNet && fRegTest)
        throw std::runtime_error("Invalid combination of -regtest and -testnet.");
    if (fRegTest)
        return CBaseChainParams::REGTEST;
    if (fTestNet)
        return CBaseChainParams::TESTNET;
    return CBaseChainParams::MAIN;
}

#ifndef WIN32
fs::path GetPidFile()
{
    fs::path pathPidFile(gArgs.GetArg("-pid", PIVX_PID_FILENAME));
    return AbsPathForConfigVal(pathPidFile);
}

void CreatePidFile(const fs::path& path, pid_t pid)
{
    FILE* file = fsbridge::fopen(path, "w");
    if (file) {
        fprintf(file, "%d\n", pid);
        fclose(file);
    }
}
#endif

bool RenameOver(fs::path src, fs::path dest)
{
#ifdef WIN32
    return MoveFileExW(src.wstring().c_str(), dest.wstring().c_str(),
                       MOVEFILE_REPLACE_EXISTING) != 0;
#else
    int rc = std::rename(src.string().c_str(), dest.string().c_str());
    return (rc == 0);
#endif /* WIN32 */
}

/**
 * Ignores exceptions thrown by Boost's create_directories if the requested directory exists.
 * Specifically handles case where path p exists, but it wasn't possible for the user to
 * write to the parent directory.
 */
bool TryCreateDirectories(const fs::path& p)
{
    try {
        return fs::create_directories(p);
    } catch (const fs::filesystem_error&) {
        if (!fs::exists(p) || !fs::is_directory(p))
            throw;
    }

    // create_directories didn't create the directory, it had to have existed already
    return false;
}

bool FileCommit(FILE* file)
{
    if (fflush(file) != 0) { // harmless if redundantly called
        LogPrintf("%s: fflush failed: %d\n", __func__, errno);
        return false;
    }
#ifdef WIN32
    HANDLE hFile = (HANDLE)_get_osfhandle(_fileno(file));
    if (FlushFileBuffers(hFile) == 0) {
        LogPrintf("%s: FlushFileBuffers failed: %d\n", __func__, GetLastError());
        return false;
    }
#else
    #if defined(__linux__) || defined(__NetBSD__)
    if (fdatasync(fileno(file)) != 0 && errno != EINVAL) { // Ignore EINVAL for filesystems that don't support sync
        LogPrintf("%s: fdatasync failed: %d\n", __func__, errno);
        return false;
    }
    #elif defined(__APPLE__) && defined(F_FULLFSYNC)
    if (fcntl(fileno(file), F_FULLFSYNC, 0) == -1) { // Manpage says "value other than -1" is returned on success
        LogPrintf("%s: fcntl F_FULLFSYNC failed: %d\n", __func__, errno);
        return false;
    }
    #else
    if (fsync(fileno(file)) != 0 && errno != EINVAL) {
        LogPrintf("%s: fsync failed: %d\n", __func__, errno);
        return false;
    }
    #endif
#endif
    return true;
}

bool TruncateFile(FILE* file, unsigned int length)
{
#if defined(WIN32)
    return _chsize(_fileno(file), length) == 0;
#else
    return ftruncate(fileno(file), length) == 0;
#endif
}

/**
 * this function tries to raise the file descriptor limit to the requested number.
 * It returns the actual file descriptor limit (which may be more or less than nMinFD)
 */
int RaiseFileDescriptorLimit(int nMinFD)
{
#if defined(WIN32)
    return 2048;
#else
    struct rlimit limitFD;
    if (getrlimit(RLIMIT_NOFILE, &limitFD) != -1) {
        if (limitFD.rlim_cur < (rlim_t)nMinFD) {
            limitFD.rlim_cur = nMinFD;
            if (limitFD.rlim_cur > limitFD.rlim_max)
                limitFD.rlim_cur = limitFD.rlim_max;
            setrlimit(RLIMIT_NOFILE, &limitFD);
            getrlimit(RLIMIT_NOFILE, &limitFD);
        }
        return limitFD.rlim_cur;
    }
    return nMinFD; // getrlimit failed, assume it's fine
#endif
}

/**
 * this function tries to make a particular range of a file allocated (corresponding to disk space)
 * it is advisory, and the range specified in the arguments will never contain live data
 */
void AllocateFileRange(FILE* file, unsigned int offset, unsigned int length)
{
#if defined(WIN32)
    // Windows-specific version
    HANDLE hFile = (HANDLE)_get_osfhandle(_fileno(file));
    LARGE_INTEGER nFileSize;
    int64_t nEndPos = (int64_t)offset + length;
    nFileSize.u.LowPart = nEndPos & 0xFFFFFFFF;
    nFileSize.u.HighPart = nEndPos >> 32;
    SetFilePointerEx(hFile, nFileSize, 0, FILE_BEGIN);
    SetEndOfFile(hFile);
#elif defined(MAC_OSX)
    // OSX specific version
    fstore_t fst;
    fst.fst_flags = F_ALLOCATECONTIG;
    fst.fst_posmode = F_PEOFPOSMODE;
    fst.fst_offset = 0;
    fst.fst_length = (off_t)offset + length;
    fst.fst_bytesalloc = 0;
    if (fcntl(fileno(file), F_PREALLOCATE, &fst) == -1) {
        fst.fst_flags = F_ALLOCATEALL;
        fcntl(fileno(file), F_PREALLOCATE, &fst);
    }
    ftruncate(fileno(file), fst.fst_length);
#elif defined(__linux__)
    // Version using posix_fallocate
    off_t nEndPos = (off_t)offset + length;
    posix_fallocate(fileno(file), 0, nEndPos);
#else
    // Fallback version
    // TODO: just write one byte per block
    static const char buf[65536] = {};
    fseek(file, offset, SEEK_SET);
    while (length > 0) {
        unsigned int now = 65536;
        if (length < now)
            now = length;
        fwrite(buf, 1, now, file); // allowed to fail; this function is advisory anyway
        length -= now;
    }
#endif
}

#ifdef WIN32
fs::path GetSpecialFolderPath(int nFolder, bool fCreate)
{
    WCHAR pszPath[MAX_PATH] = L"";

    if (SHGetSpecialFolderPathW(nullptr, pszPath, nFolder, fCreate)) {
        return fs::path(pszPath);
    }

    LogPrintf("SHGetSpecialFolderPathW() failed, could not obtain requested path.\n");
    return fs::path("");
}
#endif

void runCommand(std::string strCommand)
{
    if (strCommand.empty()) return;
#ifndef WIN32
    int nErr = ::system(strCommand.c_str());
#else
    int nErr = ::_wsystem(std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>,wchar_t>().from_bytes(strCommand).c_str());
#endif
    if (nErr)
        LogPrintf("runCommand error: system(%s) returned %d\n", strCommand, nErr);
}

void SetupEnvironment()
{
// On most POSIX systems (e.g. Linux, but not BSD) the environment's locale
// may be invalid, in which case the "C" locale is used as fallback.
#if !defined(WIN32) && !defined(MAC_OSX) && !defined(__FreeBSD__) && !defined(__OpenBSD__)
    try {
        std::locale(""); // Raises a runtime error if current locale is invalid
    } catch (const std::runtime_error&) {
        setenv("LC_ALL", "C", 1);
    }
#elif defined(WIN32)
    // Set the default input/output charset is utf-8
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif
    // The path locale is lazy initialized and to avoid deinitialization errors
    // in multithreading environments, it is set explicitly by the main thread.
    // A dummy locale is used to extract the internal default locale, used by
    // fs::path, which is then used to explicitly imbue the path.
    std::locale loc = fs::path::imbue(std::locale::classic());
#ifndef WIN32
    fs::path::imbue(loc);
#else
    fs::path::imbue(std::locale(loc, new std::codecvt_utf8_utf16<wchar_t>()));
#endif
}

bool SetupNetworking()
{
#ifdef WIN32
    // Initialize Windows Sockets
    WSADATA wsadata;
    int ret = WSAStartup(MAKEWORD(2,2), &wsadata);
    if (ret != NO_ERROR || LOBYTE(wsadata.wVersion ) != 2 || HIBYTE(wsadata.wVersion) != 2)
        return false;
#endif
    return true;
}

void SetThreadPriority(int nPriority)
{
#ifdef WIN32
    SetThreadPriority(GetCurrentThread(), nPriority);
#else // WIN32
#ifdef PRIO_THREAD
    setpriority(PRIO_THREAD, 0, nPriority);
#else  // PRIO_THREAD
    setpriority(PRIO_PROCESS, 0, nPriority);
#endif // PRIO_THREAD
#endif // WIN32
}

int GetNumCores()
{
    return std::thread::hardware_concurrency();
}


int ScheduleBatchPriority(void)
{
#ifdef SCHED_BATCH
    const static sched_param param{0};
    if (int ret = pthread_setschedparam(pthread_self(), SCHED_BATCH, &param)) {
        LogPrintf("Failed to pthread_setschedparam: %s\n", strerror(errno));
        return ret;
    }
    return 0;
#else
    return 1;
#endif
}

namespace util {
#ifdef WIN32
    WinCmdLineArgs::WinCmdLineArgs()
{
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>, wchar_t> utf8_cvt;
    argv = new char*[argc];
    args.resize(argc);
    for (int i = 0; i < argc; i++) {
        args[i] = utf8_cvt.to_bytes(wargv[i]);
        argv[i] = &*args[i].begin();
    }
    LocalFree(wargv);
}

WinCmdLineArgs::~WinCmdLineArgs()
{
    delete[] argv;
}

std::pair<int, char**> WinCmdLineArgs::get()
{
    return std::make_pair(argc, argv);
}
#endif
} // namespace util

