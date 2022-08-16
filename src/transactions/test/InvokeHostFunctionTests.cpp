// Copyright 2022 Hcnet Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <iterator>
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION

#include "crypto/SecretKey.h"
#include "ledger/LedgerTxn.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "rust/RustBridge.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionUtils.h"
#include "xdr/Hcnet-contract.h"
#include "xdr/Hcnet-ledger-entries.h"
#include <autocheck/autocheck.hpp>
#include <fmt/format.h>
#include <limits>
#include <type_traits>
#include <variant>

using namespace hcnet;
using namespace hcnet::txtest;

std::vector<uint8_t>
get_testdata(std::string const& filename)
{
    std::filesystem::path fnPath("testdata");
    fnPath /= filename;
    std::ifstream in(fnPath);
    REQUIRE(in);
    in.exceptions(std::ios::badbit);
    return std::vector<uint8_t>{std::istreambuf_iterator<char>(in), {}};
}

// Example WASM files are, for the time being, compiled from the SDK repo
// and then copied into this repo. They need to be regenerated anytime
// the environment interface version number changes, at minimum.
//
// To regenerate, check out the SDK, install a nightly toolchain with
// the rust-src component (to enable the 'tiny' build) using the following:
//
//  $ rustup component add rust-src --toolchain nightly
//
// clang-format off
// then do:
// $ make -C $SDK build
// $ cp $SDK/target-tiny/wasm32-unknown-unknown/release/example_*.wasm $CORE/src/testdata/
// clang-format on

std::vector<uint8_t>
get_example_i32_wasm()
{
    return get_testdata("example_add_i32.wasm");
}

std::vector<uint8_t>
get_example_contract_data_wasm()
{
    return get_testdata("example_contract_data.wasm");
}

template <typename T>
SCVal
makeBinary(T begin, T end)
{
    SCVal val(SCValType::SCV_OBJECT);
    val.obj().activate().type(SCO_BINARY);
    val.obj()->bin().assign(begin, end);
    return val;
}

static SCVal
makeI32(int32_t i32)
{
    SCVal val(SCV_I32);
    val.i32() = i32;
    return val;
}

static SCVal
makeSymbol(std::string const& str)
{
    SCVal val(SCV_SYMBOL);
    val.sym().assign(str.begin(), str.end());
    return val;
}

static LedgerKey
createContract(Application& app, std::vector<uint8_t> const& contract,
               uint256 const& salt, PublicKey const& pub, Signature const& sig,
               bool expectSuccess, bool expectEntry)
{
    HashIDPreimage preImage;
    preImage.type(ENVELOPE_TYPE_CONTRACT_ID_FROM_ED25519);
    preImage.contractID().ed25519 = pub.ed25519();
    preImage.contractID().salt = salt;
    auto contractID = xdrSha256(preImage);

    // Create operation
    Operation op;
    op.body.type(INVOKE_HOST_FUNCTION);
    auto& ihf = op.body.invokeHostFunctionOp();
    ihf.function = HOST_FN_CREATE_CONTRACT;

    auto contractBin = makeBinary(contract.begin(), contract.end());
    ihf.parameters = {contractBin, makeBinary(salt.begin(), salt.end()),
                      makeBinary(pub.ed25519().begin(), pub.ed25519().end()),
                      makeBinary(sig.begin(), sig.end())};

    SCVal wasmKey(SCValType::SCV_STATIC);
    wasmKey.ic() = SCStatic::SCS_LEDGER_KEY_CONTRACT_CODE_WASM;

    LedgerKey lk;
    lk.type(CONTRACT_DATA);
    lk.contractData().contractID = contractID;
    lk.contractData().key = wasmKey;

    ihf.footprint.readWrite = {lk};

    // submit operation
    auto root = TestAccount::createRoot(app);
    auto tx = transactionFrameFromOps(app.getNetworkID(), root, {op}, {});
    LedgerTxn ltx(app.getLedgerTxnRoot());
    TransactionMeta txm(2);
    REQUIRE(tx->checkValid(ltx, 0, 0, 0));
    REQUIRE(tx->apply(app, ltx, txm) == expectSuccess);
    ltx.commit();

    // verify contract code is correct
    LedgerTxn ltx2(app.getLedgerTxnRoot());
    auto ltxe2 = loadContractData(ltx2, contractID, wasmKey);
    REQUIRE((bool)ltxe2 == expectEntry);
    REQUIRE((!expectEntry ||
             ltxe2.current().data.contractData().val == contractBin));

    return lk;
}

static LedgerKey
deployContract(Application& app, std::vector<uint8_t> const& contract)
{
    uint256 salt = sha256("salt");
    auto key = SecretKey::fromSeed(sha256("a1"));

    // create signature
    auto const& separator = "create_contract_from_ed25519(contract: Vec<u8>, "
                            "salt: u256, key: u256, sig: Vec<u8>)";
    SHA256 hasher;
    hasher.add(separator);
    hasher.add(salt);
    hasher.add(contract);

    auto sig = SignatureUtils::sign(key, hasher.finish()).signature;
    return createContract(app, contract, salt, key.getPublicKey(), sig, true,
                          true);
}

TEST_CASE("invoke host function", "[tx][contract]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto root = TestAccount::createRoot(*app);

    auto const addI32Wasm = get_example_i32_wasm();
    auto const contractDataWasm = get_example_contract_data_wasm();

    SECTION("add i32")
    {
        auto contract = deployContract(*app, addI32Wasm);
        auto const& contractID = contract.contractData().contractID;

        auto call = [&](SCVec const& parameters, bool success) {
            Operation op;
            op.body.type(INVOKE_HOST_FUNCTION);
            auto& ihf = op.body.invokeHostFunctionOp();
            ihf.function = HOST_FN_CALL;
            ihf.parameters = parameters;
            ihf.footprint.readOnly = {contract};

            auto tx =
                transactionFrameFromOps(app->getNetworkID(), root, {op}, {});
            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMeta txm(2);
            REQUIRE(tx->checkValid(ltx, 0, 0, 0));
            if (success)
            {
                REQUIRE(tx->apply(*app, ltx, txm));
            }
            else
            {
                REQUIRE(!tx->apply(*app, ltx, txm));
            }
            ltx.commit();
        };

        auto scContractID = makeBinary(contractID.begin(), contractID.end());
        auto scFunc = makeSymbol("add");
        auto sc7 = makeI32(7);
        auto sc16 = makeI32(16);

        // Too few parameters for call
        call({}, false);
        call({scContractID}, false);

        // To few parameters for "add"
        call({scContractID, scFunc}, false);
        call({scContractID, scFunc, sc7}, false);

        // Correct function call
        call({scContractID, scFunc, sc7, sc16}, true);

        // Too many parameters for "add"
        call({scContractID, scFunc, sc7, sc16, makeI32(0)}, false);
    }

    SECTION("contract data")
    {
        auto contract = deployContract(*app, contractDataWasm);
        auto const& contractID = contract.contractData().contractID;

        auto checkContractData = [&](SCVal const& key, SCVal const* val) {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto ltxe = loadContractData(ltx, contractID, key);
            if (val)
            {
                REQUIRE(ltxe);
                REQUIRE(ltxe.current().data.contractData().val == *val);
            }
            else
            {
                REQUIRE(!ltxe);
            }
        };

        auto putWithFootprint = [&](std::string const& key,
                                    std::string const& val,
                                    xdr::xvector<LedgerKey> const& readOnly,
                                    xdr::xvector<LedgerKey> const& readWrite,
                                    bool success) {
            auto keySymbol = makeSymbol(key);
            auto valSymbol = makeSymbol(val);

            Operation op;
            op.body.type(INVOKE_HOST_FUNCTION);
            auto& ihf = op.body.invokeHostFunctionOp();
            ihf.function = HOST_FN_CALL;
            ihf.parameters.emplace_back(
                makeBinary(contractID.begin(), contractID.end()));
            ihf.parameters.emplace_back(makeSymbol("put"));
            ihf.parameters.emplace_back(keySymbol);
            ihf.parameters.emplace_back(valSymbol);
            ihf.footprint.readOnly = readOnly;
            ihf.footprint.readWrite = readWrite;

            auto tx =
                transactionFrameFromOps(app->getNetworkID(), root, {op}, {});
            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMeta txm(2);
            REQUIRE(tx->checkValid(ltx, 0, 0, 0));
            if (success)
            {
                REQUIRE(tx->apply(*app, ltx, txm));
                ltx.commit();
                checkContractData(keySymbol, &valSymbol);
            }
            else
            {
                REQUIRE(!tx->apply(*app, ltx, txm));
                ltx.commit();
            }
        };

        auto put = [&](std::string const& key, std::string const& val) {
            putWithFootprint(key, val, {contract},
                             {contractDataKey(contractID, makeSymbol(key))},
                             true);
        };

        auto delWithFootprint = [&](std::string const& key,
                                    xdr::xvector<LedgerKey> const& readOnly,
                                    xdr::xvector<LedgerKey> const& readWrite,
                                    bool success) {
            auto keySymbol = makeSymbol(key);

            Operation op;
            op.body.type(INVOKE_HOST_FUNCTION);
            auto& ihf = op.body.invokeHostFunctionOp();
            ihf.function = HOST_FN_CALL;
            ihf.parameters.emplace_back(
                makeBinary(contractID.begin(), contractID.end()));
            ihf.parameters.emplace_back(makeSymbol("del"));
            ihf.parameters.emplace_back(keySymbol);
            ihf.footprint.readOnly = readOnly;
            ihf.footprint.readWrite = readWrite;

            auto tx =
                transactionFrameFromOps(app->getNetworkID(), root, {op}, {});
            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMeta txm(2);
            REQUIRE(tx->checkValid(ltx, 0, 0, 0));
            if (success)
            {
                REQUIRE(tx->apply(*app, ltx, txm));
                ltx.commit();
                checkContractData(keySymbol, nullptr);
            }
            else
            {
                REQUIRE(!tx->apply(*app, ltx, txm));
                ltx.commit();
            }
        };

        auto del = [&](std::string const& key) {
            delWithFootprint(key, {contract},
                             {contractDataKey(contractID, makeSymbol(key))},
                             true);
        };

        put("key1", "val1a");
        put("key2", "val2a");

        // Failure: contract data isn't in footprint
        putWithFootprint("key1", "val1b", {contract}, {}, false);
        delWithFootprint("key1", {contract}, {}, false);

        // Failure: contract data is read only
        auto cdk = contractDataKey(contractID, makeSymbol("key2"));
        putWithFootprint("key2", "val2b", {contract, cdk}, {}, false);
        delWithFootprint("key2", {contract, cdk}, {}, false);

        put("key1", "val1c");
        put("key2", "val2c");

        del("key1");
        del("key2");
    }

    SECTION("create contract failures")
    {
        // create signature
        auto const& separator =
            "create_contract_from_ed25519(contract: Vec<u8>, "
            "salt: u256, key: u256, sig: Vec<u8>)";
        uint256 salt = sha256("salt");
        auto key = SecretKey::fromSeed(sha256("a1"));

        {
            SHA256 hasher;
            hasher.add(separator);
            hasher.add(salt);
            hasher.add(addI32Wasm);
            auto sig = SignatureUtils::sign(key, hasher.finish()).signature;

            // public key is different than the one that created the signature
            auto new_pub = SecretKey::fromSeed(sha256("a2"));
            createContract(*app, addI32Wasm, salt, new_pub.getPublicKey(), sig,
                           false, false);
        }

        {
            // bad separator
            SHA256 hasher;
            hasher.add("bad_separator");
            hasher.add(salt);
            hasher.add(addI32Wasm);
            auto sig = SignatureUtils::sign(key, hasher.finish()).signature;

            createContract(*app, addI32Wasm, salt, key.getPublicKey(), sig,
                           false, false);
        }

        {
            // Incorrect salt was hashed
            SHA256 hasher;
            hasher.add(separator);
            hasher.add(sha256("wrong_salt"));
            hasher.add(addI32Wasm);
            auto sig = SignatureUtils::sign(key, hasher.finish()).signature;

            createContract(*app, addI32Wasm, salt, key.getPublicKey(), sig,
                           false, false);
        }

        {
            // Incorrect contract was hashed
            SHA256 hasher;
            hasher.add(separator);
            hasher.add(salt);
            hasher.add(contractDataWasm);
            auto sig = SignatureUtils::sign(key, hasher.finish()).signature;

            createContract(*app, addI32Wasm, salt, key.getPublicKey(), sig,
                           false, false);
        }

        {
            // duplicate contract
            SHA256 hasher;
            hasher.add(separator);
            hasher.add(salt);
            hasher.add(addI32Wasm);

            auto sig = SignatureUtils::sign(key, hasher.finish()).signature;
            createContract(*app, addI32Wasm, salt, key.getPublicKey(), sig,
                           true, true);
            createContract(*app, addI32Wasm, salt, key.getPublicKey(), sig,
                           false, true);
        }
    }
}

#endif
