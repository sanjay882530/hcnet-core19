// Copyright 2015 Hcnet Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "LedgerTestUtils.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "main/Config.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/types.h"
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
#include "xdr/Hcnet-contract.h"
#endif
#include "xdr/Hcnet-ledger-entries.h"
#include <autocheck/generator.hpp>
#include <locale>
#include <string>
#include <xdrpp/autocheck.h>
#include <xdrpp/xdrpp/marshal.h>

namespace hcnet
{
namespace LedgerTestUtils
{

template <typename T>
void
clampLow(T low, T& v)
{
    if (v < low)
    {
        v = low;
    }
}

template <typename T>
void
clampHigh(T high, T& v)
{
    if (v > high)
    {
        v = high;
    }
}

// mutate string such that it doesn't contain control characters
// and is at least minSize characters long
template <typename T>
void
replaceControlCharacters(T& s, int minSize)
{
    auto& loc = std::locale::classic();
    if (static_cast<int>(s.size()) < minSize)
    {
        s.resize(minSize);
    }
    for (auto it = s.begin(); it != s.end(); it++)
    {
        char c = static_cast<char>(*it);
        if (c < 0 || std::iscntrl(c, loc))
        {
            auto b = autocheck::generator<char>{}(autocheck::detail::nalnums);
            *it = b;
        }
    }
}

static bool
signerEqual(Signer const& s1, Signer const& s2)
{
    return s1.key == s2.key;
}

void
randomlyModifyEntry(LedgerEntry& e)
{
    switch (e.data.type())
    {
    case TRUSTLINE:
        e.data.trustLine().limit = autocheck::generator<int64>{}();
        e.data.trustLine().balance = e.data.trustLine().limit - 10;
        makeValid(e.data.trustLine());
        break;
    case OFFER:
        e.data.offer().amount = autocheck::generator<int64>{}();
        makeValid(e.data.offer());
        break;
    case ACCOUNT:
        e.data.account().balance = autocheck::generator<int64>{}();
        e.data.account().seqNum++;
        makeValid(e.data.account());
        break;
    case DATA:
        e.data.data().dataValue = autocheck::generator<DataValue>{}(8);
        makeValid(e.data.data());
        break;
    case CLAIMABLE_BALANCE:
        e.data.claimableBalance().amount = autocheck::generator<int64>{}();
        makeValid(e.data.claimableBalance());
        break;
    case LIQUIDITY_POOL:
        e.data.liquidityPool().body.constantProduct().reserveA =
            autocheck::generator<int64>{}();
        e.data.liquidityPool().body.constantProduct().reserveB =
            autocheck::generator<int64>{}();
        e.data.liquidityPool().body.constantProduct().totalPoolShares =
            autocheck::generator<int64>{}();
        e.data.liquidityPool().body.constantProduct().poolSharesTrustLineCount =
            autocheck::generator<int64>{}();
        makeValid(e.data.liquidityPool());
        break;
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    case CONFIG_SETTING:
    {
        e.data.configSetting().setting.type(CONFIG_SETTING_TYPE_UINT32);
        e.data.configSetting().setting.uint32Val() =
            autocheck::generator<uint32_t>{}();
        makeValid(e.data.configSetting());
        break;
    }
    case CONTRACT_DATA:
        e.data.contractData().val.type(SCV_I32);
        e.data.contractData().val.i32() = autocheck::generator<int32_t>{}();
        makeValid(e.data.contractData());
        break;
#endif
    }
}

void
makeValid(AccountEntry& a)
{
    if (a.balance < 0)
    {
        a.balance = -a.balance;
    }

    replaceControlCharacters(a.homeDomain, 0);

    if (a.inflationDest)
    {
        *a.inflationDest = PubKeyUtils::pseudoRandomForTesting();
    }

    std::sort(
        a.signers.begin(), a.signers.end(),
        [](Signer const& lhs, Signer const& rhs) { return lhs.key < rhs.key; });
    a.signers.erase(
        std::unique(a.signers.begin(), a.signers.end(), signerEqual),
        a.signers.end());
    for (auto& s : a.signers)
    {
        s.weight = s.weight & UINT8_MAX;
        if (s.weight == 0)
        {
            s.weight = 100;
        }
    }
    a.numSubEntries = (uint32)a.signers.size();
    if (a.seqNum < 0)
    {
        a.seqNum = -a.seqNum;
    }
    a.flags = a.flags & MASK_ACCOUNT_FLAGS_V17;

    if (a.ext.v() == 1)
    {
        a.ext.v1().liabilities.buying = std::abs(a.ext.v1().liabilities.buying);
        a.ext.v1().liabilities.selling =
            std::abs(a.ext.v1().liabilities.selling);

        if (a.ext.v1().ext.v() == 2)
        {
            auto& extV2 = a.ext.v1().ext.v2();

            int64_t effEntries = 2LL;
            effEntries += a.numSubEntries;
            effEntries += extV2.numSponsoring;
            effEntries -= extV2.numSponsored;
            if (effEntries < 0)
            {
                // This condition implies (in arbitrary precision)
                //      2 + numSubentries + numSponsoring - numSponsored < 0
                // which can be rearranged as
                //      numSponsored > 2 + numSubentries + numSponsoring .
                // Substituting this inequality yields
                //      2 + numSubentries + numSponsored
                //          > 4 + 2 * numSubentries + numSponsoring
                //          > numSponsoring
                // which can be rearranged as
                //      2 + numSubentries + numSponsored - numSponsoring > 0 .
                // In summary, swapping numSponsored and numSponsoring fixes the
                // account state.
                std::swap(extV2.numSponsored, extV2.numSponsoring);
            }

            extV2.signerSponsoringIDs.resize(
                static_cast<uint32_t>(a.signers.size()));
        }
    }
}

void
makeValid(TrustLineEntry& tl)
{
    if (tl.balance < 0)
    {
        tl.balance = -tl.balance;
    }
    tl.limit = std::abs(tl.limit);
    clampLow<int64>(1, tl.limit);

    switch (tl.asset.type())
    {
    case ASSET_TYPE_NATIVE:
        // ASSET_TYPE_NATIVE is not a valid trustline asset type, so change the
        // type to a valid one
        tl.asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
        strToAssetCode(tl.asset.alphaNum4().assetCode, "USD");
        break;
    case ASSET_TYPE_CREDIT_ALPHANUM4:
        strToAssetCode(tl.asset.alphaNum4().assetCode, "USD");
        break;
    case ASSET_TYPE_CREDIT_ALPHANUM12:
        strToAssetCode(tl.asset.alphaNum12().assetCode, "USD12");
        break;
    default:
        break;
    }

    clampHigh<int64_t>(tl.limit, tl.balance);
    tl.flags = tl.flags & MASK_TRUSTLINE_FLAGS_V17;

    if (tl.ext.v() == 1)
    {
        tl.ext.v1().liabilities.buying =
            std::abs(tl.ext.v1().liabilities.buying);
        tl.ext.v1().liabilities.selling =
            std::abs(tl.ext.v1().liabilities.selling);
    }
}
void
makeValid(OfferEntry& o)
{
    o.offerID = o.offerID & INT64_MAX;
    o.selling.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(o.selling.alphaNum4().assetCode, "CAD");

    o.buying.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(o.buying.alphaNum4().assetCode, "EUR");

    o.amount = std::abs(o.amount);
    clampLow<int64>(1, o.amount);

    o.price.n = std::abs(o.price.n);
    o.price.d = std::abs(o.price.d);
    clampLow(1, o.price.n);
    clampLow(1, o.price.d);

    o.flags = o.flags & MASK_OFFERENTRY_FLAGS;
}

void
makeValid(DataEntry& d)
{
    replaceControlCharacters(d.dataName, 1);
}

void
makeValid(ClaimableBalanceEntry& c)
{
    c.amount = std::abs(c.amount);
    clampLow<int64>(1, c.amount);

    // It is not valid for claimants to be empty, so if this occurs we default
    // to a single claimant for the zero account with
    // CLAIM_PREDICATE_UNCONDITIONAL.
    if (c.claimants.empty())
    {
        c.claimants.resize(1);
    }

    c.asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(c.asset.alphaNum4().assetCode, "CAD");

    if (c.ext.v() == 1)
    {
        c.ext.v1().flags = MASK_CLAIMABLE_BALANCE_FLAGS;
    }
}

void
makeValid(LiquidityPoolEntry& lp)
{
    auto& cp = lp.body.constantProduct();
    cp.params.assetA.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(cp.params.assetA.alphaNum4().assetCode, "CAD");

    cp.params.assetB.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(cp.params.assetB.alphaNum4().assetCode, "USD");

    cp.params.fee = 30;
    cp.reserveA = std::abs(cp.reserveA);
    cp.reserveB = std::abs(cp.reserveB);
    cp.totalPoolShares = std::abs(cp.totalPoolShares);
    cp.poolSharesTrustLineCount = std::abs(cp.poolSharesTrustLineCount);
}

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
void
makeValid(ConfigSettingEntry& ce)
{
    auto ids = xdr::xdr_traits<ConfigSettingID>::enum_values();
    ce.configSettingID =
        static_cast<ConfigSettingID>(ids.at(ce.configSettingID % ids.size()));
}

void
makeValid(ContractDataEntry& cde)
{
}
#endif

void
makeValid(std::vector<LedgerHeaderHistoryEntry>& lhv,
          LedgerHeaderHistoryEntry firstLedger,
          HistoryManager::LedgerVerificationStatus state)
{
    auto randomIndex = rand_uniform<size_t>(1, lhv.size() - 1);
    auto prevHash = firstLedger.header.previousLedgerHash;
    auto ledgerSeq = firstLedger.header.ledgerSeq;

    for (size_t i = 0; i < lhv.size(); i++)
    {
        auto& lh = lhv[i];

        lh.header.ledgerVersion = Config::CURRENT_LEDGER_PROTOCOL_VERSION;
        lh.header.previousLedgerHash = prevHash;
        lh.header.ledgerSeq = ledgerSeq;

        if (i == randomIndex && state != HistoryManager::VERIFY_STATUS_OK)
        {
            switch (state)
            {
            case HistoryManager::VERIFY_STATUS_ERR_BAD_LEDGER_VERSION:
                lh.header.ledgerVersion += 1;
                break;
            case HistoryManager::VERIFY_STATUS_ERR_BAD_HASH:
                lh.header.previousLedgerHash =
                    HashUtils::pseudoRandomForTesting();
                break;
            case HistoryManager::VERIFY_STATUS_ERR_UNDERSHOT:
                lh.header.ledgerSeq -= 1;
                break;
            case HistoryManager::VERIFY_STATUS_ERR_OVERSHOT:
                lh.header.ledgerSeq += 1;
                break;
            default:
                break;
            }
        }
        // On a coin flip, corrupt header content rather than previous link
        if (i == randomIndex &&
            state == HistoryManager::VERIFY_STATUS_ERR_BAD_HASH && rand_flip())
        {
            lh.hash = HashUtils::pseudoRandomForTesting();
        }
        else
        {
            lh.hash = sha256(xdr::xdr_to_opaque(lh.header));
        }

        prevHash = lh.hash;
        ledgerSeq = lh.header.ledgerSeq + 1;
    }

    if (state == HistoryManager::VERIFY_STATUS_ERR_MISSING_ENTRIES)
    {
        // Delete last element
        lhv.erase(lhv.begin() + lhv.size() - 1);
    }
}

static auto validLedgerEntryGeneratorMaybeIncludingConfig = autocheck::map(
    [](LedgerEntry&& le, size_t s) {
        auto& led = le.data;
        le.lastModifiedLedgerSeq = le.lastModifiedLedgerSeq & INT32_MAX;
        switch (led.type())
        {
        case ACCOUNT:
            makeValid(led.account());
            break;
        case TRUSTLINE:
            makeValid(led.trustLine());
            break;
        case OFFER:
            makeValid(led.offer());
            break;
        case DATA:
            makeValid(led.data());
            break;
        case CLAIMABLE_BALANCE:
            makeValid(led.claimableBalance());
            break;
        case LIQUIDITY_POOL:
            makeValid(led.liquidityPool());
            break;
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
        case CONFIG_SETTING:
            makeValid(led.configSetting());
            break;
        case CONTRACT_DATA:
            makeValid(led.contractData());
            break;
#endif
        }

        return std::move(le);
    },
    autocheck::generator<LedgerEntry>());

// When compiling the next protocol version we might be able to generate
// CONFIG_SETTING entries, but we explicitly exclude them from the default
// generator here because they violate an assumption that the rest of the
// machinery here makes: that randomly generated keys are (statistically)
// guaranteed to be disjoint.
static auto validLedgerEntryGenerator =
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    autocheck::such_that(
        [](LedgerEntry const& le) { return le.data.type() != CONFIG_SETTING; },
        validLedgerEntryGeneratorMaybeIncludingConfig);
#else
    validLedgerEntryGeneratorMaybeIncludingConfig;
#endif

static auto ledgerKeyGenerator =
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    autocheck::such_that(
        [](LedgerKey const& k) { return k.type() != CONFIG_SETTING; },
        autocheck::generator<LedgerKey>());
#else
    autocheck::generator<LedgerKey>();
#endif

static auto validAccountEntryGenerator = autocheck::map(
    [](AccountEntry&& ae, size_t s) {
        makeValid(ae);
        return std::move(ae);
    },
    autocheck::generator<AccountEntry>());

static auto validTrustLineEntryGenerator = autocheck::map(
    [](TrustLineEntry&& tl, size_t s) {
        makeValid(tl);
        return std::move(tl);
    },
    autocheck::generator<TrustLineEntry>());

static auto validOfferEntryGenerator = autocheck::map(
    [](OfferEntry&& o, size_t s) {
        makeValid(o);
        return std::move(o);
    },
    autocheck::generator<OfferEntry>());

static auto validDataEntryGenerator = autocheck::map(
    [](DataEntry&& d, size_t s) {
        makeValid(d);
        return std::move(d);
    },
    autocheck::generator<DataEntry>());

static auto validClaimableBalanceEntryGenerator = autocheck::map(
    [](ClaimableBalanceEntry&& c, size_t s) {
        makeValid(c);
        return std::move(c);
    },
    autocheck::generator<ClaimableBalanceEntry>());

static auto validLiquidityPoolEntryGenerator = autocheck::map(
    [](LiquidityPoolEntry&& c, size_t s) {
        makeValid(c);
        return std::move(c);
    },
    autocheck::generator<LiquidityPoolEntry>());

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
static auto validConfigSettingEntryGenerator = autocheck::map(
    [](ConfigSettingEntry&& c, size_t s) {
        makeValid(c);
        return std::move(c);
    },
    autocheck::generator<ConfigSettingEntry>());

static auto validContractDataEntryGenerator = autocheck::map(
    [](ContractDataEntry&& c, size_t s) {
        makeValid(c);
        return std::move(c);
    },
    autocheck::generator<ContractDataEntry>());
#endif

LedgerEntry
generateValidLedgerEntry(size_t b)
{
    return validLedgerEntryGenerator(b);
}

std::vector<LedgerEntry>
generateValidLedgerEntries(size_t n)
{
    static auto vecgen = autocheck::list_of(validLedgerEntryGenerator);
    return vecgen(n);
}

LedgerKey
generateLedgerKey(size_t n)
{
    return ledgerKeyGenerator(n);
}

std::vector<LedgerKey>
generateLedgerKeys(size_t n)
{
    static auto vecgen = autocheck::list_of(ledgerKeyGenerator);
    return vecgen(n);
}

AccountEntry
generateValidAccountEntry(size_t b)
{
    return validAccountEntryGenerator(b);
}

std::vector<AccountEntry>
generateValidAccountEntries(size_t n)
{
    static auto vecgen = autocheck::list_of(validAccountEntryGenerator);
    return vecgen(n);
}

TrustLineEntry
generateNonPoolShareValidTrustLineEntry(size_t b)
{
    auto tl = validTrustLineEntryGenerator(b);
    if (tl.asset.type() == ASSET_TYPE_POOL_SHARE)
    {
        tl.asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
        strToAssetCode(tl.asset.alphaNum4().assetCode, "USD");
    }

    return tl;
}

TrustLineEntry
generateValidTrustLineEntry(size_t b)
{
    return validTrustLineEntryGenerator(b);
}

std::vector<TrustLineEntry>
generateValidTrustLineEntries(size_t n)
{
    static auto vecgen = autocheck::list_of(validTrustLineEntryGenerator);
    return vecgen(n);
}

OfferEntry
generateValidOfferEntry(size_t b)
{
    return validOfferEntryGenerator(b);
}

std::vector<OfferEntry>
generateValidOfferEntries(size_t n)
{
    static auto vecgen = autocheck::list_of(validOfferEntryGenerator);
    return vecgen(n);
}

DataEntry
generateValidDataEntry(size_t b)
{
    return validDataEntryGenerator(b);
}

std::vector<DataEntry>
generateValidDataEntries(size_t n)
{
    static auto vecgen = autocheck::list_of(validDataEntryGenerator);
    return vecgen(n);
}

ClaimableBalanceEntry
generateValidClaimableBalanceEntry(size_t b)
{
    return validClaimableBalanceEntryGenerator(b);
}

std::vector<ClaimableBalanceEntry>
generateValidClaimableBalanceEntries(size_t n)
{
    static auto vecgen =
        autocheck::list_of(validClaimableBalanceEntryGenerator);
    return vecgen(n);
}

LiquidityPoolEntry
generateValidLiquidityPoolEntry(size_t b)
{
    return validLiquidityPoolEntryGenerator(b);
}

std::vector<LiquidityPoolEntry>
generateValidLiquidityPoolEntries(size_t n)
{
    static auto vecgen = autocheck::list_of(validLiquidityPoolEntryGenerator);
    return vecgen(n);
}

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
ConfigSettingEntry
generateValidConfigSettingEntry(size_t b)
{
    return validConfigSettingEntryGenerator(b);
}

std::vector<ConfigSettingEntry>
generateValidConfigSettingEntries(size_t n)
{
    static auto vecgen = autocheck::list_of(validConfigSettingEntryGenerator);
    return vecgen(n);
}

ContractDataEntry
generateValidContractDataEntry(size_t b)
{
    return validContractDataEntryGenerator(b);
}

std::vector<ContractDataEntry>
generateValidContractDataEntries(size_t n)
{
    static auto vecgen = autocheck::list_of(validContractDataEntryGenerator);
    return vecgen(n);
}
#endif

std::vector<LedgerHeaderHistoryEntry>
generateLedgerHeadersForCheckpoint(
    LedgerHeaderHistoryEntry firstLedger, uint32_t size,
    HistoryManager::LedgerVerificationStatus state)
{
    static auto vecgen =
        autocheck::list_of(autocheck::generator<LedgerHeaderHistoryEntry>());
    auto res = vecgen(size);
    makeValid(res, firstLedger, state);
    return res;
}

UpgradeType
toUpgradeType(LedgerUpgrade const& upgrade)
{
    auto v = xdr::xdr_to_opaque(upgrade);
    auto result = UpgradeType{v.begin(), v.end()};
    return result;
}
}
}
