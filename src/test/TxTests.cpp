// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TxTests.h"

#include "crypto/ByteSlice.h"
#include "invariant/Invariants.h"
#include "ledger/DataFrame.h"
#include "ledger/LedgerDelta.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "test/TestExceptions.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "transactions/AllowTrustOpFrame.h"
#include "transactions/ChangeTrustOpFrame.h"
#include "transactions/CreateAccountOpFrame.h"
#include "transactions/InflationOpFrame.h"
#include "transactions/ManageDataOpFrame.h"
#include "transactions/ManageOfferOpFrame.h"
#include "transactions/MergeOpFrame.h"
#include "transactions/PathPaymentOpFrame.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/SetOptionsOpFrame.h"
#include "transactions/TransactionFrame.h"
#include "util/Logging.h"
#include "util/make_unique.h"
#include "util/types.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;
namespace stellar
{
using xdr::operator==;

namespace txtest
{

bool
applyCheck(TransactionFramePtr tx, LedgerDelta& delta, Application& app)
{
    auto txSet = std::make_shared<TxSetFrame>(
        app.getLedgerManager().getLastClosedLedgerHeader().hash);
    txSet->add(tx);

    // TODO: maybe we should just close ledger with tx instead of checking all
    // of
    // that manually?

    bool check = tx->checkValid(app, 0);
    TransactionResult checkResult = tx->getResult();

    REQUIRE((!check || checkResult.result.code() == txSUCCESS));

    bool doApply;
    // valid transaction sets ensure that
    {
        auto code = checkResult.result.code();
        if (code != txNO_ACCOUNT && code != txBAD_SEQ)
        {
            tx->processFeeSeqNum(delta, app.getLedgerManager());
        }
        doApply = (code != txBAD_SEQ);
    }

    bool res = false;

    if (doApply)
    {
        try
        {
            res = tx->apply(delta, app);
        }
        catch (...)
        {
            tx->getResult().result.code(txINTERNAL_ERROR);
        }

        REQUIRE((!res || tx->getResultCode() == txSUCCESS));

        if (!check)
        {
            REQUIRE(checkResult == tx->getResult());
        }
    }
    else
    {
        res = check;
    }

    // validates db state
    app.getLedgerManager().checkDbState();
    app.getInvariants().check(txSet, delta);

    return res;
}

void
checkTransaction(TransactionFrame& txFrame)
{
    REQUIRE(txFrame.getResult().feeCharged == 100); // default fee
    REQUIRE((txFrame.getResultCode() == txSUCCESS ||
             txFrame.getResultCode() == txFAILED));
}

void
applyTx(TransactionFramePtr const& tx, Application& app)
{
    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    applyCheck(tx, delta, app);
    throwIf(tx->getResult());
    checkTransaction(*tx);
    delta.commit();
}

TxSetResultMeta
closeLedgerOn(Application& app, uint32 ledgerSeq, int day, int month, int year,
              std::vector<TransactionFramePtr> const& txs)
{

    auto txSet = std::make_shared<TxSetFrame>(
        app.getLedgerManager().getLastClosedLedgerHeader().hash);

    for (auto const& tx : txs)
    {
        txSet->add(tx);
    }

    txSet->sortForHash();
    REQUIRE(txSet->checkValid(app));

    StellarValue sv(txSet->getContentsHash(), getTestDate(day, month, year),
                    emptyUpgradeSteps, 0);
    LedgerCloseData ledgerData(ledgerSeq, txSet, sv);
    app.getLedgerManager().closeLedger(ledgerData);

    auto z1 = TransactionFrame::getTransactionHistoryResults(app.getDatabase(),
                                                             ledgerSeq);
    auto z2 =
        TransactionFrame::getTransactionFeeMeta(app.getDatabase(), ledgerSeq);

    REQUIRE(app.getLedgerManager().getLedgerNum() == (ledgerSeq + 1));

    TxSetResultMeta res;
    std::transform(z1.results.begin(), z1.results.end(), z2.begin(),
                   std::back_inserter(res), [](TransactionResultPair const& r1,
                                               LedgerEntryChanges const& r2) {
                       return std::make_pair(r1, r2);
                   });

    return res;
}

SecretKey
getRoot(Hash const& networkID)
{
    return SecretKey::fromSeed(networkID);
}

SecretKey
getAccount(const char* n)
{
    // stretch seed to 32 bytes
    std::string seed(n);
    while (seed.size() < 32)
        seed += '.';
    return SecretKey::fromSeed(seed);
}

AccountFrame::pointer
loadAccount(PublicKey const& k, Application& app, bool mustExist)
{
    auto res = AccountFrame::loadAccount(k, app.getDatabase());
    if (mustExist)
    {
        REQUIRE(res);
    }
    return res;
}

void
requireNoAccount(PublicKey const& k, Application& app)
{
    AccountFrame::pointer res = loadAccount(k, app, false);
    REQUIRE(!res);
}

OfferFrame::pointer
loadOffer(PublicKey const& k, uint64 offerID, Application& app, bool mustExist)
{
    OfferFrame::pointer res =
        OfferFrame::loadOffer(k, offerID, app.getDatabase());
    if (mustExist)
    {
        REQUIRE(res);
    }
    return res;
}

TrustFrame::pointer
loadTrustLine(SecretKey const& k, Asset const& asset, Application& app,
              bool mustExist)
{
    TrustFrame::pointer res =
        TrustFrame::loadTrustLine(k.getPublicKey(), asset, app.getDatabase());
    if (mustExist)
    {
        REQUIRE(res);
    }
    return res;
}

xdr::xvector<Signer, 20>
getAccountSigners(PublicKey const& k, Application& app)
{
    AccountFrame::pointer account;
    account = loadAccount(k, app);
    return account->getAccount().signers;
}

TransactionFramePtr
transactionFromOperation(Hash const& networkID, SecretKey const& from,
                         SequenceNumber seq, Operation const& op)
{
    TransactionEnvelope e;

    e.tx.sourceAccount = from.getPublicKey();
    e.tx.fee = 100;
    e.tx.seqNum = seq;
    e.tx.operations.push_back(op);

    TransactionFramePtr res =
        TransactionFrame::makeTransactionFromWire(networkID, e);

    res->addSignature(from);

    return res;
}

TransactionFramePtr
transactionFromOperations(Hash const& networkID, SecretKey const& from,
                          SequenceNumber seq, const std::vector<Operation>& ops)
{
    TransactionEnvelope e;

    e.tx.sourceAccount = from.getPublicKey();
    e.tx.fee = ops.size() * 100;
    e.tx.seqNum = seq;
    std::copy(std::begin(ops), std::end(ops),
              std::back_inserter(e.tx.operations));

    TransactionFramePtr res =
        TransactionFrame::makeTransactionFromWire(networkID, e);

    res->addSignature(from);

    return res;
}

Operation
createChangeTrustOp(Asset const& asset, int64_t limit)
{
    Operation op;

    op.body.type(CHANGE_TRUST);
    op.body.changeTrustOp().limit = limit;
    op.body.changeTrustOp().line = asset;

    return op;
}

Operation
createAllowTrustOp(PublicKey const& trustor, Asset const& asset,
                   bool authorize)
{
    Operation op;

    op.body.type(ALLOW_TRUST);
    op.body.allowTrustOp().trustor = trustor;
    op.body.allowTrustOp().asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    op.body.allowTrustOp().asset.assetCode4() = asset.alphaNum4().assetCode;
    op.body.allowTrustOp().authorize = authorize;

    return op;
}

Operation
createCreateAccountOp(PublicKey const& dest,
                      int64_t amount)
{
    Operation op;
    op.body.type(CREATE_ACCOUNT);
    op.body.createAccountOp().startingBalance = amount;
    op.body.createAccountOp().destination = dest;
    return op;
}

Operation
createPaymentOp(PublicKey const& to, int64_t amount)
{
    Operation op;
    op.body.type(PAYMENT);
    op.body.paymentOp().amount = amount;
    op.body.paymentOp().destination = to;
    op.body.paymentOp().asset.type(ASSET_TYPE_NATIVE);
    return op;
}

TransactionFramePtr
createPaymentTx(Hash const& networkID, SecretKey const& from,
                PublicKey const& to, SequenceNumber seq, int64_t amount)
{
    return transactionFromOperation(networkID, from, seq,
                                    createPaymentOp(to, amount));
}

void
applyPaymentTx(Application& app, SecretKey const& from, PublicKey const& to,
               SequenceNumber seq, int64_t amount)
{
    auto toAccount = loadAccount(to, app, false);
    auto fromAccount = loadAccount(from.getPublicKey(), app);
    auto tx = createPaymentTx(app.getNetworkID(), from, to, seq, amount);

    try
    {
        applyTx(tx, app);
    }
    catch (...)
    {
        auto toAccountAfter = loadAccount(to, app, false);
        // check that the target account didn't change
        REQUIRE(!!toAccount == !!toAccountAfter);
        if (toAccount && toAccountAfter)
        {
            REQUIRE(toAccount->getAccount() == toAccountAfter->getAccount());
        }
        throw;
    }

    auto toAccountAfter = loadAccount(to, app, false);
    REQUIRE(toAccount);
    REQUIRE(toAccountAfter);
}

TransactionFramePtr
createCreditPaymentTx(Hash const& networkID, SecretKey const& from,
                      PublicKey const& to, Asset const& asset,
                      SequenceNumber seq, int64_t amount)
{
    Operation op;
    op.body.type(PAYMENT);
    op.body.paymentOp().amount = amount;
    op.body.paymentOp().asset = asset;
    op.body.paymentOp().destination = to;

    return transactionFromOperation(networkID, from, seq, op);
}

Asset
makeAsset(SecretKey const& issuer, std::string const& code)
{
    Asset asset;
    asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    asset.alphaNum4().issuer = issuer.getPublicKey();
    strToAssetCode(asset.alphaNum4().assetCode, code);
    return asset;
}

void
applyCreditPaymentTx(Application& app, SecretKey const& from,
                     PublicKey const& to, Asset const& ci, SequenceNumber seq,
                     int64_t amount)
{
    auto tx = createCreditPaymentTx(app.getNetworkID(), from, to, ci, seq, amount);
    applyTx(tx, app);
}

Operation
createPathPaymentOp(PublicKey const& to, Asset const& sendCur, int64_t sendMax,
                    Asset const& destCur, int64_t destAmount,
                    std::vector<Asset> const& path)
{
    Operation op;
    op.body.type(PATH_PAYMENT);
    PathPaymentOp& ppop = op.body.pathPaymentOp();
    ppop.sendAsset = sendCur;
    ppop.sendMax = sendMax;
    ppop.destAsset = destCur;
    ppop.destAmount = destAmount;
    ppop.destination = to;
    std::copy(std::begin(path), std::end(path), std::back_inserter(ppop.path));

    return op;
}

TransactionFramePtr
createPassiveOfferOp(Hash const& networkID, SecretKey const& source,
                     Asset const& selling, Asset const& buying,
                     Price const& price, int64_t amount, SequenceNumber seq)
{
    Operation op;
    op.body.type(CREATE_PASSIVE_OFFER);
    op.body.createPassiveOfferOp().amount = amount;
    op.body.createPassiveOfferOp().selling = selling;
    op.body.createPassiveOfferOp().buying = buying;
    op.body.createPassiveOfferOp().price = price;

    return transactionFromOperation(networkID, source, seq, op);
}

TransactionFramePtr
manageOfferOp(Hash const& networkID, uint64 offerId, SecretKey const& source,
              Asset const& selling, Asset const& buying, Price const& price,
              int64_t amount, SequenceNumber seq)
{
    Operation op;
    op.body.type(MANAGE_OFFER);
    op.body.manageOfferOp().amount = amount;
    op.body.manageOfferOp().selling = selling;
    op.body.manageOfferOp().buying = buying;
    op.body.manageOfferOp().offerID = offerId;
    op.body.manageOfferOp().price = price;

    return transactionFromOperation(networkID, source, seq, op);
}

static ManageOfferResult
applyCreateOfferHelper(Application& app, uint64 offerId,
                       SecretKey const& source, Asset const& selling,
                       Asset const& buying, Price const& price, int64_t amount,
                       SequenceNumber seq)
{
    auto lastGeneratedID = app.getLedgerManager().getCurrentLedgerHeader().idPool;
    auto expectedOfferID = lastGeneratedID + 1;
    if (offerId != 0)
    {
        expectedOfferID = offerId;
    }

    auto tx = manageOfferOp(app.getNetworkID(), offerId, source, selling,
                            buying, price, amount, seq);

    try
    {
        applyTx(tx, app);
    }
    catch (...)
    {
        REQUIRE(app.getLedgerManager().getCurrentLedgerHeader().idPool == lastGeneratedID);
        throw;
    }

    auto& results = tx->getResult().result.results();

    REQUIRE(results.size() == 1);

    auto& manageOfferResult = results[0].tr().manageOfferResult();

    OfferFrame::pointer offer;

    auto& offerResult = manageOfferResult.success().offer;

    switch (offerResult.effect())
    {
    case MANAGE_OFFER_CREATED:
    case MANAGE_OFFER_UPDATED:
    {
        offer =
            loadOffer(source.getPublicKey(), expectedOfferID, app, true);
        auto& offerEntry = offer->getOffer();
        REQUIRE(offerEntry == offerResult.offer());
        REQUIRE(offerEntry.price == price);
        REQUIRE(offerEntry.selling == selling);
        REQUIRE(offerEntry.buying == buying);
    }
    break;
    case MANAGE_OFFER_DELETED:
        REQUIRE(
            !loadOffer(source.getPublicKey(), expectedOfferID, app, false));
        break;
    default:
        abort();
    }

    return manageOfferResult;
}

uint64_t
applyManageOffer(Application& app, uint64 offerId, SecretKey const& source,
                 Asset const& selling, Asset const& buying, Price const& price,
                 int64_t amount, SequenceNumber seq,
                 ManageOfferEffect expectedEffect)
{
    ManageOfferResult const& createOfferRes = applyCreateOfferHelper(
        app, offerId, source, selling, buying, price, amount, seq);

    auto& success = createOfferRes.success().offer;
    REQUIRE(success.effect() == expectedEffect);
    return success.effect() == MANAGE_OFFER_CREATED ? success.offer().offerID
                                                    : 0;
}

uint64_t
applyCreatePassiveOffer(Application& app, SecretKey const& source,
                        Asset const& selling, Asset const& buying,
                        Price const& price, int64_t amount, SequenceNumber seq,
                        ManageOfferEffect expectedEffect)
{
    auto lastGeneratedID = app.getLedgerManager().getCurrentLedgerHeader().idPool;
    auto expectedOfferID = lastGeneratedID + 1;

    auto tx = createPassiveOfferOp(app.getNetworkID(), source, selling, buying,
                                   price, amount, seq);

    try
    {
        applyTx(tx, app);
    }
    catch (...)
    {
        REQUIRE(app.getLedgerManager().getCurrentLedgerHeader().idPool == lastGeneratedID);
        throw;
    }

    auto& results = tx->getResult().result.results();

    REQUIRE(results.size() == 1);

    auto& createPassiveOfferResult = results[0].tr().manageOfferResult();

    if (createPassiveOfferResult.code() == MANAGE_OFFER_SUCCESS)
    {
        OfferFrame::pointer offer;

        auto& offerResult = createPassiveOfferResult.success().offer;

        switch (offerResult.effect())
        {
        case MANAGE_OFFER_CREATED:
        case MANAGE_OFFER_UPDATED:
        {
            offer =
                loadOffer(source.getPublicKey(), expectedOfferID, app, true);
            auto& offerEntry = offer->getOffer();
            REQUIRE(offerEntry == offerResult.offer());
            REQUIRE(offerEntry.price == price);
            REQUIRE(offerEntry.selling == selling);
            REQUIRE(offerEntry.buying == buying);
            REQUIRE((offerEntry.flags & PASSIVE_FLAG) != 0);
        }
        break;
        case MANAGE_OFFER_DELETED:
            REQUIRE(
                !loadOffer(source.getPublicKey(), expectedOfferID, app, false));
            break;
        default:
            abort();
        }
    }

    auto& success = createPassiveOfferResult.success().offer;

    REQUIRE(success.effect() == expectedEffect);

    return success.effect() == MANAGE_OFFER_CREATED ? success.offer().offerID
                                                    : 0;
}

Operation
createSetOptionsOp(AccountID* inflationDest, uint32_t* setFlags,
                   uint32_t* clearFlags, ThresholdSetter* thrs, Signer* signer,
                   std::string* homeDomain)
{
    Operation op;
    op.body.type(SET_OPTIONS);

    SetOptionsOp& setOp = op.body.setOptionsOp();

    if (inflationDest)
    {
        setOp.inflationDest.activate() = *inflationDest;
    }

    if (setFlags)
    {
        setOp.setFlags.activate() = *setFlags;
    }

    if (clearFlags)
    {
        setOp.clearFlags.activate() = *clearFlags;
    }

    if (thrs)
    {
        if (thrs->masterWeight)
        {
            setOp.masterWeight.activate() = *thrs->masterWeight;
        }
        if (thrs->lowThreshold)
        {
            setOp.lowThreshold.activate() = *thrs->lowThreshold;
        }
        if (thrs->medThreshold)
        {
            setOp.medThreshold.activate() = *thrs->medThreshold;
        }
        if (thrs->highThreshold)
        {
            setOp.highThreshold.activate() = *thrs->highThreshold;
        }
    }

    if (signer)
    {
        setOp.signer.activate() = *signer;
    }

    if (homeDomain)
    {
        setOp.homeDomain.activate() = *homeDomain;
    }

    return op;
}

Operation
createInflationOp()
{
    Operation op;
    op.body.type(INFLATION);

    return op;
}

Operation
createMergeOp(PublicKey const& dest)
{
    Operation op;
    op.body.type(ACCOUNT_MERGE);
    op.body.destination() = dest;
    return op;
}

Operation
createManageDataOp(std::string const& name, DataValue* value)
{
    Operation op;
    op.body.type(MANAGE_DATA);
    op.body.manageDataOp().dataName = name;
    if (value)
        op.body.manageDataOp().dataValue.activate() = *value;

    return op;
}

OperationFrame const&
getFirstOperationFrame(TransactionFrame const& tx)
{
    return *(tx.getOperations()[0]);
}

OperationResult const&
getFirstResult(TransactionFrame const& tx)
{
    return getFirstOperationFrame(tx).getResult();
}

OperationResultCode
getFirstResultCode(TransactionFrame const& tx)
{
    return getFirstOperationFrame(tx).getResultCode();
}

void
checkAmounts(int64_t a, int64_t b, int64_t maxd)
{
    int64_t d = b - maxd;
    REQUIRE(a >= d);
    REQUIRE(a <= b);
}

void
checkTx(int index, TxSetResultMeta& r, TransactionResultCode expected)
{
    REQUIRE(r[index].first.result.result.code() == expected);
};

void
checkTx(int index, TxSetResultMeta& r, TransactionResultCode expected,
        OperationResultCode code)
{
    checkTx(index, r, expected);
    REQUIRE(r[index].first.result.result.results()[0].code() == code);
};
}
}
