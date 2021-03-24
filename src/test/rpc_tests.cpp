// Copyright (c) 2012-2013 The Bitcoin Core developers
// Copyright (c) 2017-2019 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "rpc/server.h"
#include "rpc/client.h"

#include "base58.h"
#include "netbase.h"
#include "util.h"

#include "test/test_pivx.h"

#include <boost/algorithm/string.hpp>
#include <boost/test/unit_test.hpp>

#include <univalue.h>


UniValue
createArgs(int nRequired, const char* address1=nullptr, const char* address2=nullptr)
{
    UniValue result(UniValue::VARR);
    result.push_back(nRequired);
    UniValue addresses(UniValue::VARR);
    if (address1) addresses.push_back(address1);
    if (address2) addresses.push_back(address2);
    result.push_back(addresses);
    return result;
}

UniValue CallRPC(std::string args)
{
    std::vector<std::string> vArgs;
    boost::split(vArgs, args, boost::is_any_of(" \t"));
    std::string strMethod = vArgs[0];
    vArgs.erase(vArgs.begin());
    JSONRPCRequest request;
    request.strMethod = strMethod;
    request.params = RPCConvertValues(strMethod, vArgs);
    request.fHelp = false;
    BOOST_CHECK(tableRPC[strMethod]);
    rpcfn_type method = tableRPC[strMethod]->actor;
    try {
        UniValue result = (*method)(request);
        return result;
    }
    catch (const UniValue& objError) {
        throw std::runtime_error(find_value(objError, "message").get_str());
    }
}


BOOST_FIXTURE_TEST_SUITE(rpc_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(rpc_rawparams)
{
    // Test raw transaction API argument handling
    UniValue r;

    BOOST_CHECK_THROW(CallRPC("getrawtransaction"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("getrawtransaction not_hex"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("getrawtransaction a3b807410df0b60fcb9736768df5823938b2f838694939ba45f3c0a1bff150ed not_int"), std::runtime_error);

    BOOST_CHECK_THROW(CallRPC("createrawtransaction"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("createrawtransaction null null"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("createrawtransaction not_array"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("createrawtransaction [] []"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("createrawtransaction {} {}"), std::runtime_error);
    BOOST_CHECK_NO_THROW(CallRPC("createrawtransaction [] {}"));
    BOOST_CHECK_THROW(CallRPC("createrawtransaction [] {} extra"), std::runtime_error);

    BOOST_CHECK_THROW(CallRPC("decoderawtransaction"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("decoderawtransaction null"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("decoderawtransaction DEADBEEF"), std::runtime_error);
    std::string rawtx = "0100000001a15d57094aa7a21a28cb20b59aab8fc7d1149a3bdbcddba9c622e4f5f6a99ece010000006c493046022100f93bb0e7d8db7bd46e40132d1f8242026e045f03a0efe71bbb8e3f475e970d790221009337cd7f1f929f00cc6ff01f03729b069a7c21b59b1736ddfee5db5946c5da8c0121033b9b137ee87d5a812d6f506efdd37f0affa7ffc310711c06c7f3e097c9447c52ffffffff0100e1f505000000001976a9140389035a9225b3839e2bbf32d826a1e222031fd888ac00000000";
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("decoderawtransaction ")+rawtx));
    BOOST_CHECK_EQUAL(find_value(r.get_obj(), "version").get_int(), 1);
    BOOST_CHECK_EQUAL(find_value(r.get_obj(), "locktime").get_int(), 0);
    BOOST_CHECK_THROW(r = CallRPC(std::string("decoderawtransaction ")+rawtx+" extra"), std::runtime_error);

    BOOST_CHECK_THROW(CallRPC("signrawtransaction"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("signrawtransaction null"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("signrawtransaction ff00"), std::runtime_error);
    BOOST_CHECK_NO_THROW(CallRPC(std::string("signrawtransaction ")+rawtx));
    BOOST_CHECK_NO_THROW(CallRPC(std::string("signrawtransaction ")+rawtx+" null null NONE|ANYONECANPAY"));
    BOOST_CHECK_NO_THROW(CallRPC(std::string("signrawtransaction ")+rawtx+" [] [] NONE|ANYONECANPAY"));
    BOOST_CHECK_THROW(CallRPC(std::string("signrawtransaction ")+rawtx+" null null badenum"), std::runtime_error);

    // Only check failure cases for sendrawtransaction, there's no network to send to...
    BOOST_CHECK_THROW(CallRPC("sendrawtransaction"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("sendrawtransaction null"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("sendrawtransaction DEADBEEF"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC(std::string("sendrawtransaction ")+rawtx+" extra"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(rpc_rawsign)
{
    UniValue r;
    // input is a 1-of-2 multisig (so is output):
    std::string prevout =
      "[{\"txid\":\"dd2888870cdc3f6e92661f6b0829667ee4bb07ed086c44205e726bbf3338f726\","
      "\"vout\":1,\"scriptPubKey\":\"a914f5404a39a4799d8710e15db4c4512c5e06f97fed87\","
      "\"redeemScript\":\"5121021431a18c7039660cd9e3612a2a47dc53b69cb38ea4ad743b7df8245fd0438f8e21029bbeff390ce736bd396af43b52a1c14ed52c086b1e5585c15931f68725772bac52ae\"}]";
    r = CallRPC(std::string("createrawtransaction ")+prevout+" "+
      "{\"6ckcNMWRYgTnPcrTXCdwhDnMLwj3zwseej\":1}");
    std::string notsigned = r.get_str();
    std::string privkey1 = "\"YVobcS47fr6kceZy9LzLJR8WQ6YRpUwYKoJhrnEXepebMxaSpbnn\"";
    std::string privkey2 = "\"YRyMjG8hbm8jHeDMAfrzSeHq5GgAj7kuHFvJtMudCUH3sCkq1WtA\"";
    r = CallRPC(std::string("signrawtransaction ")+notsigned+" "+prevout+" "+"[]");
    BOOST_CHECK(find_value(r.get_obj(), "complete").get_bool() == false);
    r = CallRPC(std::string("signrawtransaction ")+notsigned+" "+prevout+" "+"["+privkey1+","+privkey2+"]");
    BOOST_CHECK(find_value(r.get_obj(), "complete").get_bool() == true);
}

BOOST_AUTO_TEST_CASE(rpc_format_monetary_values)
{
    BOOST_CHECK(ValueFromAmount(0LL).write() == "0.00000000");
    BOOST_CHECK(ValueFromAmount(1LL).write() == "0.00000001");
    BOOST_CHECK(ValueFromAmount(17622195LL).write() == "0.17622195");
    BOOST_CHECK(ValueFromAmount(50000000LL).write() == "0.50000000");
    BOOST_CHECK(ValueFromAmount(89898989LL).write() == "0.89898989");
    BOOST_CHECK(ValueFromAmount(100000000LL).write() == "1.00000000");
    BOOST_CHECK(ValueFromAmount(2099999999999990LL).write() == "20999999.99999990");
    BOOST_CHECK(ValueFromAmount(2099999999999999LL).write() == "20999999.99999999");

    BOOST_CHECK_EQUAL(ValueFromAmount(0).write(), "0.00000000");
    BOOST_CHECK_EQUAL(ValueFromAmount((COIN/10000)*123456789).write(), "12345.67890000");
    BOOST_CHECK_EQUAL(ValueFromAmount(-COIN).write(), "-1.00000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(-COIN/10).write(), "-0.10000000");

    BOOST_CHECK_EQUAL(ValueFromAmount(COIN*100000000).write(), "100000000.00000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN*10000000).write(), "10000000.00000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN*1000000).write(), "1000000.00000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN*100000).write(), "100000.00000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN*10000).write(), "10000.00000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN*1000).write(), "1000.00000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN*100).write(), "100.00000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN*10).write(), "10.00000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN).write(), "1.00000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN/10).write(), "0.10000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN/100).write(), "0.01000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN/1000).write(), "0.00100000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN/10000).write(), "0.00010000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN/100000).write(), "0.00001000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN/1000000).write(), "0.00000100");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN/10000000).write(), "0.00000010");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN/100000000).write(), "0.00000001");
}

static UniValue ValueFromString(const std::string &str)
{
    UniValue value;
    BOOST_CHECK(value.setNumStr(str));
    return value;
}

BOOST_AUTO_TEST_CASE(rpc_parse_monetary_values)
{
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0.00000001")), 1LL);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0.17622195")), 17622195LL);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0.5")), 50000000LL);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0.50000000")), 50000000LL);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0.89898989")), 89898989LL);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("1.00000000")), 100000000LL);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("20999999.9999999")), 2099999999999990LL);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("20999999.99999999")), 2099999999999999LL);

    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("1e-8")), COIN/100000000);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0.1e-7")), COIN/100000000);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0.01e-6")), COIN/100000000);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0.0000000000000000000000000000000000000000000000000000000000000000000000000001e+68")), COIN/100000000);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("10000000000000000000000000000000000000000000000000000000000000000e-64")), COIN);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0.000000000000000000000000000000000000000000000000000000000000000100000000000000000000000000000000000000000000000000000e64")), COIN);

    BOOST_CHECK_THROW(AmountFromValue(ValueFromString("1e-9")), UniValue); //should fail
    BOOST_CHECK_THROW(AmountFromValue(ValueFromString("0.000000019")), UniValue); //should fail
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0.00000001000000")), 1LL); //should pass, cut trailing 0
    BOOST_CHECK_THROW(AmountFromValue(ValueFromString("19e-9")), UniValue); //should fail
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0.19e-6")), 19); //should pass, leading 0 is present

    BOOST_CHECK_THROW(AmountFromValue(ValueFromString("92233720368.54775808")), UniValue); //overflow error
    BOOST_CHECK_THROW(AmountFromValue(ValueFromString("1e+11")), UniValue); //overflow error
    BOOST_CHECK_THROW(AmountFromValue(ValueFromString("1e11")), UniValue); //overflow error signless
    BOOST_CHECK_THROW(AmountFromValue(ValueFromString("93e+9")), UniValue); //overflow error
}

BOOST_AUTO_TEST_CASE(json_parse_errors)
{
    // Valid
    BOOST_CHECK_EQUAL(ParseNonRFCJSONValue("1.0").get_real(), 1.0);
    // Valid, with leading or trailing whitespace
    BOOST_CHECK_EQUAL(ParseNonRFCJSONValue(" 1.0").get_real(), 1.0);
    BOOST_CHECK_EQUAL(ParseNonRFCJSONValue("1.0 ").get_real(), 1.0);
    // Invalid, initial garbage
    BOOST_CHECK_THROW(ParseNonRFCJSONValue("[1.0"), std::runtime_error);
    BOOST_CHECK_THROW(ParseNonRFCJSONValue("a1.0"), std::runtime_error);
    // Invalid, trailing garbage
    BOOST_CHECK_THROW(ParseNonRFCJSONValue("1.0sds"), std::runtime_error);
    BOOST_CHECK_THROW(ParseNonRFCJSONValue("1.0]"), std::runtime_error);
    // BTC addresses should fail parsing
    BOOST_CHECK_THROW(ParseNonRFCJSONValue("175tWpb8K1S7NmH4Zx6rewF9WQrcZv245W"), std::runtime_error);
    BOOST_CHECK_THROW(ParseNonRFCJSONValue("3J98t1WpEZ73CNmQviecrnyiWrnqRhWNL"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(rpc_ban)
{
    BOOST_CHECK_NO_THROW(CallRPC(std::string("clearbanned")));

    UniValue r;
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("setban 127.0.0.0 add")));
    BOOST_CHECK_THROW(r = CallRPC(std::string("setban 127.0.0.0:8334")), std::runtime_error); //portnumber for setban not allowed
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("listbanned")));
    UniValue ar = r.get_array();
    UniValue o1 = ar[0].get_obj();
    UniValue adr = find_value(o1, "address");
    BOOST_CHECK_EQUAL(adr.get_str(), "127.0.0.0/32");
    BOOST_CHECK_NO_THROW(CallRPC(std::string("setban 127.0.0.0 remove")));;
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("listbanned")));
    ar = r.get_array();
    BOOST_CHECK_EQUAL(ar.size(), 0);

    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("setban 127.0.0.0/24 add 9907731200 true")));
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("listbanned")));
    ar = r.get_array();
    o1 = ar[0].get_obj();
    adr = find_value(o1, "address");
    UniValue banned_until = find_value(o1, "banned_until");
    BOOST_CHECK_EQUAL(adr.get_str(), "127.0.0.0/24");
    BOOST_CHECK_EQUAL(banned_until.get_int64(), 9907731200); // absolute time check

    BOOST_CHECK_NO_THROW(CallRPC(std::string("clearbanned")));

    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("setban 127.0.0.0/24 add 200")));
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("listbanned")));
    ar = r.get_array();
    o1 = ar[0].get_obj();
    adr = find_value(o1, "address");
    banned_until = find_value(o1, "banned_until");
    BOOST_CHECK_EQUAL(adr.get_str(), "127.0.0.0/24");
    int64_t now = GetTime();
    BOOST_CHECK(banned_until.get_int64() > now);
    BOOST_CHECK(banned_until.get_int64()-now <= 200);

    // must throw an exception because 127.0.0.1 is in already banned suubnet range
    BOOST_CHECK_THROW(r = CallRPC(std::string("setban 127.0.0.1 add")), std::runtime_error);

    BOOST_CHECK_NO_THROW(CallRPC(std::string("setban 127.0.0.0/24 remove")));;
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("listbanned")));
    ar = r.get_array();
    BOOST_CHECK_EQUAL(ar.size(), 0);

    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("setban 127.0.0.0/255.255.0.0 add")));
    BOOST_CHECK_THROW(r = CallRPC(std::string("setban 127.0.1.1 add")), std::runtime_error);

    BOOST_CHECK_NO_THROW(CallRPC(std::string("clearbanned")));
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("listbanned")));
    ar = r.get_array();
    BOOST_CHECK_EQUAL(ar.size(), 0);

    BOOST_CHECK_THROW(r = CallRPC(std::string("setban test add")), std::runtime_error); //invalid IP

    //IPv6 tests
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("setban FE80:0000:0000:0000:0202:B3FF:FE1E:8329 add")));
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("listbanned")));
    ar = r.get_array();
    o1 = ar[0].get_obj();
    adr = find_value(o1, "address");
    BOOST_CHECK_EQUAL(adr.get_str(), "fe80::202:b3ff:fe1e:8329/128");

    BOOST_CHECK_NO_THROW(CallRPC(std::string("clearbanned")));
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("setban 2001:db8::/ffff:fffc:0:0:0:0:0:0 add")));
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("listbanned")));
    ar = r.get_array();
    o1 = ar[0].get_obj();
    adr = find_value(o1, "address");
    BOOST_CHECK_EQUAL(adr.get_str(), "2001:db8::/30");

    BOOST_CHECK_NO_THROW(CallRPC(std::string("clearbanned")));
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("setban 2001:4d48:ac57:400:cacf:e9ff:fe1d:9c63/128 add")));
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("listbanned")));
    ar = r.get_array();
    o1 = ar[0].get_obj();
    adr = find_value(o1, "address");
    BOOST_CHECK_EQUAL(adr.get_str(), "2001:4d48:ac57:400:cacf:e9ff:fe1d:9c63/128");
}

BOOST_AUTO_TEST_SUITE_END()
