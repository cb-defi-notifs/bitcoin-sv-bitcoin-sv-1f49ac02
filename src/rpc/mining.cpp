// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2019 Bitcoin Association
// Distributed under the Open BSV software license, see the accompanying file LICENSE.

#include "rpc/mining.h"
#include "amount.h"
#include "block_index_store.h"
#include "chain.h"
#include "chainparams.h"
#include "config.h"
#include "consensus/consensus.h"
#include "consensus/merkle.h"
#include "consensus/params.h"
#include "consensus/validation.h"
#include "mining/factory.h"
#include "core_io.h"
#include "dstencode.h"
#include "mining/factory.h"
#include "net/net.h"
#include "policy/policy.h"
#include "primitives/transaction.h"
#include "pow.h"
#include "rpc/blockchain.h"
#include "rpc/server.h"
#include "script/script_num.h"
#include "txmempool.h"
#include "util.h"
#include "utilstrencodings.h"
#include "validation.h"
#include "validationinterface.h"
#include "invalid_txn_publisher.h"
#include "rpc/http_protocol.h"
#include <univalue.h>
#include <cstdint>
#include <memory>

using mining::CBlockTemplate;

void IncrementExtraNonce(CBlock *pblock,
                         const CBlockIndex *pindexPrev,
                         unsigned int &nExtraNonce) {
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock) {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    // Height first in coinbase required for block.version=2
    unsigned int nHeight = pindexPrev->GetHeight() + 1;
    CMutableTransaction txCoinbase(*pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig =
        (CScript() << nHeight << CScriptNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(txCoinbase.vin[0].scriptSig.size() <= MAX_COINBASE_SCRIPTSIG_SIZE);

    pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
}



/**
 * Return average network hashes per second based on the last 'lookup' blocks,
 * or from the last difficulty change if 'lookup' is nonpositive. If 'height' is
 * nonnegative, compute the estimate at the time when a given block was found.
 */
static UniValue GetNetworkHashPS(int lookup, int32_t height) {
    CBlockIndex *pb = chainActive.Tip();

    if (height >= 0 && height < chainActive.Height()) {
        pb = chainActive[height];
    }

    if (pb == nullptr || !pb->GetHeight()) {
        return 0;
    }

    // If lookup is -1, then use blocks since last difficulty change.
    if (lookup <= 0) {
        lookup = pb->GetHeight() %
                     Params().GetConsensus().DifficultyAdjustmentInterval() +
                 1;
    }

    // If lookup is larger than chain, then set it to chain length.
    if (lookup > pb->GetHeight()) {
        lookup = pb->GetHeight();
    }

    const CBlockIndex* pb0 = pb;
    int64_t minTime = pb0->GetBlockTime();
    int64_t maxTime = minTime;
    for (int i = 0; i < lookup; i++) {
        pb0 = pb0->GetPrev();
        int64_t time = pb0->GetBlockTime();
        minTime = std::min(time, minTime);
        maxTime = std::max(time, maxTime);
    }

    // In case there's a situation where minTime == maxTime, we don't want a
    // divide by zero exception.
    if (minTime == maxTime) {
        return 0;
    }

    arith_uint256 workDiff = pb->GetChainWork() - pb0->GetChainWork();
    int64_t timeDiff = maxTime - minTime;

    return workDiff.getdouble() / timeDiff;
}

static UniValue getnetworkhashps(const Config &config,
                                 const JSONRPCRequest &request) {
    if (request.fHelp || request.params.size() > 2) {
        throw std::runtime_error(
            "getnetworkhashps ( nblocks height )\n"
            "\nReturns the estimated network hashes per second based on the "
            "last n blocks.\n"
            "Pass in [blocks] to override # of blocks, -1 specifies since last "
            "difficulty change.\n"
            "Pass in [height] to estimate the network speed at the time when a "
            "certain block was found.\n"
            "\nArguments:\n"
            "1. nblocks     (numeric, optional, default=120) The number of "
            "blocks, or -1 for blocks since last difficulty change.\n"
            "2. height      (numeric, optional, default=-1) To estimate at the "
            "time of the given height.\n"
            "\nResult:\n"
            "x             (numeric) Hashes per second estimated\n"
            "\nExamples:\n" +
            HelpExampleCli("getnetworkhashps", "") +
            HelpExampleRpc("getnetworkhashps", ""));
    }

    LOCK(cs_main);
    return GetNetworkHashPS(
        request.params.size() > 0 ? request.params[0].get_int() : 120,
        request.params.size() > 1 ? request.params[1].get_int() : -1);
}

UniValue generateBlocks(const Config &config,
                        std::shared_ptr<CReserveScript> coinbaseScript,
                        int nGenerate, uint64_t nMaxTries, bool keepScript) {
    static const int nInnerLoopCount = 0x100000;
    int32_t nHeightStart = chainActive.Height();
    int32_t nHeightEnd = nHeightStart + nGenerate;
    int32_t nHeight = nHeightStart;

    if(!mining::g_miningFactory)
    {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "No mining factory available");
    }

    unsigned int nExtraNonce = 0;
    UniValue blockHashes(UniValue::VARR);
    CBlockIndex* pindexPrev {nullptr};
    /* Generating blocks in this loop on a busy node can call more than one CreateNewBlock on the same
     * active chain height, causing to overwrite block(s). generateBlocks will thus not create exactly
     * nGenerate blocks.
     * This can happen if there is another asynchronous ActivateBestChain running while the one running
     * in this thread (ProcessNewBlock) returns before chain is updated (for example when
     * CBlockValidationStatus::isAncestorInValidation)
     */
    while (nHeight < nHeightEnd) {
        std::unique_ptr<CBlockTemplate> pblocktemplate(
            mining::g_miningFactory->GetAssembler()->CreateNewBlock(coinbaseScript->reserveScript, pindexPrev));

        if (!pblocktemplate.get()) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Couldn't create new block");
        }

        CBlockRef blockRef = pblocktemplate->GetBlockRef();
        CBlock *pblock = blockRef.get();
        IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);

        while (nMaxTries > 0 && pblock->nNonce < nInnerLoopCount &&
               !CheckProofOfWork(pblock->GetHash(), pblock->nBits, config)) {
            ++pblock->nNonce;
            --nMaxTries;
        }

        if (nMaxTries == 0) {
            break;
        }

        if (pblock->nNonce == nInnerLoopCount) {
            continue;
        }

        std::shared_ptr<const CBlock> shared_pblock =
            std::make_shared<const CBlock>(*pblock);

        if (shared_pblock->vtx[0]->HasP2SHOutput()) {
            throw JSONRPCError(RPC_TRANSACTION_REJECTED, "bad-txns-vout-p2sh");
        }

        // If block size was checked in CheckBlock() during CreateNewBlock() (it depends on chain params testBlockCandidateValidity), 
        // another check during ProcessNewBlock() is not needed. 
        // With setexcessiveblock() RPC method value maxBlockSize may change to lower value
        // during block validation. Thus, block could be rejected because it would exceed the max block size,
        // even though it was accepted when block was created.
        const BlockValidationOptions validationOptions = BlockValidationOptions()
            .withCheckMaxBlockSize(!config.GetTestBlockCandidateValidity());
        if (!ProcessNewBlock(config, shared_pblock, true, nullptr, CBlockSource::MakeRPC(), validationOptions)) {
            throw JSONRPCError(RPC_INTERNAL_ERROR,
                               "ProcessNewBlock, block not accepted");
        }
        ++nHeight;
        blockHashes.push_back(pblock->GetHash().GetHex());

        // Mark script as important because it was used at least for one
        // coinbase output if the script came from the wallet.
        if (keepScript) {
            coinbaseScript->KeepScript();
        }
    }

    return blockHashes;
}

static UniValue generatetoaddress(const Config &config,
                                  const JSONRPCRequest &request) {
    if (request.fHelp || request.params.size() < 2 ||
        request.params.size() > 3) {
        throw std::runtime_error(
            "generatetoaddress nblocks address (maxtries)\n"
            "\nMine blocks immediately to a specified address (before the RPC "
            "call returns)\n"
            "\nArguments:\n"
            "1. nblocks      (numeric, required) How many blocks are generated "
            "immediately.\n"
            "2. address      (string, required) The address to send the newly "
            "generated bitcoin to.\n"
            "3. maxtries     (numeric, optional) How many iterations to try "
            "(default = 1000000).\n"
            "\nResult:\n"
            "[ blockhashes ]     (array) hashes of blocks generated\n"
            "\nExamples:\n"
            "\nGenerate 11 blocks to myaddress\n" +
            HelpExampleCli("generatetoaddress", "11 \"myaddress\"") +
            HelpExampleRpc("generatetoaddress", "11, \"myaddress\""));
    }

    int nGenerate = request.params[0].get_int();
    uint64_t nMaxTries = 1000000;
    if (request.params.size() > 2) {
        nMaxTries = request.params[2].get_int();
    }

    CTxDestination destination =
        DecodeDestination(request.params[1].get_str(), config.GetChainParams());
    if (!IsValidDestination(destination)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                           "Error: Invalid address");
    }

    std::shared_ptr<CReserveScript> coinbaseScript =
        std::make_shared<CReserveScript>();
    coinbaseScript->reserveScript = GetScriptForDestination(destination);

    return generateBlocks(config, coinbaseScript, nGenerate, nMaxTries, false);
}

static UniValue getmininginfo(const Config &config,
                              const JSONRPCRequest &request) {
    if (request.fHelp || request.params.size() != 0) {
        throw std::runtime_error(
            "getmininginfo\n"
            "\nReturns a json object containing mining-related information."
            "\nResult:\n"
            "{\n"
            "  \"blocks\": nnn,             (numeric) The current block\n"
            "  \"currentblocksize\": nnn,   (numeric) The last block size\n"
            "  \"currentblocktx\": nnn,     (numeric) The last block "
            "transaction\n"
            "  \"difficulty\": xxx.xxxxx    (numeric) The current difficulty\n"
            "  \"errors\": \"...\"            (string) Current errors\n"
            "  \"networkhashps\": nnn,      (numeric) The network hashes per "
            "second\n"
            "  \"pooledtx\": n              (numeric) The size of the mempool\n"
            "  \"chain\": \"xxxx\",           (string) current network name as "
            "defined in BIP70 (main, test, regtest)\n"
            "}\n"
            "\nExamples:\n" +
            HelpExampleCli("getmininginfo", "") +
            HelpExampleRpc("getmininginfo", ""));
    }

    LOCK(cs_main);

    UniValue obj(UniValue::VOBJ);
    obj.push_back(Pair("blocks", int(chainActive.Height())));
    auto const stats = mining::g_miningFactory->GetAssembler()->getLastBlockStats();
    obj.push_back(Pair("currentblocksize", uint64_t(stats.blockSize)));
    obj.push_back(Pair("currentblocktx", uint64_t(stats.txCount)));
    obj.push_back(Pair("difficulty", double(GetDifficulty(chainActive.Tip()))));
    obj.push_back(Pair("errors", GetWarnings("statusbar")));
    obj.push_back(Pair("networkhashps", getnetworkhashps(config, request)));
    obj.push_back(Pair("pooledtx", uint64_t(mempool.Size())));
    obj.push_back(Pair("chain", config.GetChainParams().NetworkIDString()));
    return obj;
}

// NOTE: Unlike wallet RPC (which use BSV values), mining RPCs follow GBT (BIP
// 22) in using satoshi amounts
static UniValue prioritisetransaction(const Config &config,
                                      const JSONRPCRequest &request) {
    if (request.fHelp || request.params.size() != 3) {
        throw std::runtime_error(
            "prioritisetransaction <txid> <priority delta> <fee delta>\n"
            "Accepts the transaction into mined blocks at a higher (or lower) "
            "priority\n"
            "\nArguments:\n"
            "1. \"txid\"       (string, required) The transaction id.\n"
            "2. dummy (numeric, required) Unused, must be set to zero.\n"
            "3. fee_delta      (numeric, required) The fee value (in satoshis) "
            "to add (or subtract, if negative).\n"
            "                  The fee is not actually paid, only the "
            "algorithm for selecting transactions into a block\n"
            "                  considers the transaction as it would have paid "
            "a higher (or lower) fee.\n"
            "\nResult:\n"
            "true              (boolean) Returns true\n"
            "\nExamples:\n" +
            HelpExampleCli("prioritisetransaction", "\"txid\" 0.0 10000") +
            HelpExampleRpc("prioritisetransaction", "\"txid\", 0.0, 10000"));
    }

    uint256 hash = ParseHashStr(request.params[0].get_str(), "txid");
    if(request.params[1].get_real() != 0)
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Dummy parameter must be set to zero.");
    }
    Amount nAmount(request.params[2].get_int64());

    mempool.PrioritiseTransaction(hash, request.params[0].get_str(), nAmount);
    return true;
}

// NOTE: Assumes a conclusive result; if result is inconclusive, it must be
// handled by caller
static UniValue BIP22ValidationResult(const Config &config,
                                      const CValidationState &state) {
    if (state.IsValid()) {
        return NullUniValue;
    }

    std::string strRejectReason = state.GetRejectReason();
    if (state.IsError()) {
        throw JSONRPCError(RPC_VERIFY_ERROR, strRejectReason);
    }

    if (state.IsInvalid()) {
        if (strRejectReason.empty()) {
            return "rejected";
        }
        return strRejectReason;
    }

    // Should be impossible.
    return "valid?";
}

void getblocktemplate(const Config& config,
                      const JSONRPCRequest& request,
                      HTTPRequest* httpReq,
                      bool processedInBatch = true)
{
    if (request.fHelp || request.params.size() > 1) {
        throw std::runtime_error(
            "getblocktemplate ( TemplateRequest )\n"
            "\nIf the request parameters include a 'mode' key, that is used to "
            "explicitly select between the default 'template' request or a "
            "'proposal'.\n"
            "It returns data needed to construct a block to work on.\n"
            "For full specification, see BIPs 22, 23, 9, and 145:\n"
            "    "
            "https://github.com/bitcoin/bips/blob/master/bip-0022.mediawiki\n"
            "    "
            "https://github.com/bitcoin/bips/blob/master/bip-0023.mediawiki\n"
            "    "
            "https://github.com/bitcoin/bips/blob/master/"
            "bip-0009.mediawiki#getblocktemplate_changes\n"
            "    "
            "https://github.com/bitcoin/bips/blob/master/bip-0145.mediawiki\n"

            "\nArguments:\n"
            "1. template_request         (json object, optional) A json object "
            "in the following spec\n"
            "     {\n"
            "       \"mode\":\"template\"    (string, optional) This must be "
            "set to \"template\", \"proposal\" (see BIP 23), or omitted\n"
            "       \"capabilities\":[     (array, optional) A list of "
            "strings\n"
            "           \"support\"          (string) client side supported "
            "feature, 'longpoll', 'coinbasetxn', 'coinbasevalue', 'proposal', "
            "'serverlist', 'workid'\n"
            "           ,...\n"
            "       ]\n"
            "     }\n"
            "\n"

            "\nResult:\n"
            "{\n"
            "  \"version\" : n,                    (numeric) The preferred "
            "block version\n"
            "  \"previousblockhash\" : \"xxxx\",     (string) The hash of "
            "current highest block\n"
            "  \"transactions\" : [                (array) contents of "
            "non-coinbase transactions that should be included in the next "
            "block\n"
            "      {\n"
            "         \"data\" : \"xxxx\",             (string) transaction "
            "data encoded in hexadecimal (byte-for-byte)\n"
            "         \"txid\" : \"xxxx\",             (string) transaction id "
            "encoded in little-endian hexadecimal\n"
            "         \"hash\" : \"xxxx\",             (string) hash encoded "
            "in little-endian hexadecimal (including witness data)\n"
            "         \"depends\" : [                (array) array of numbers "
            "\n"
            "             n                          (numeric) transactions "
            "before this one (by 1-based index in 'transactions' list) that "
            "must be present in the final block if this one is\n"
            "             ,...\n"
            "         ],\n"
            "         \"fee\": n,                    (numeric) difference in "
            "value between transaction inputs and outputs (in Satoshis); for "
            "coinbase transactions, this is a negative Number of the total "
            "collected block fees (ie, not including the block subsidy); if "
            "key is not present, fee is unknown and clients MUST NOT assume "
            "there isn't one\n"
            "         \"required\" : true|false      (boolean) if provided and "
            "true, this transaction must be in the final block\n"
            "      }\n"
            "      ,...\n"
            "  ],\n"
            "  \"coinbaseaux\" : {                 (json object) data that "
            "should be included in the coinbase's scriptSig content\n"
            "      \"flags\" : \"xx\"                  (string) key name is to "
            "be ignored, and value included in scriptSig\n"
            "  },\n"
            "  \"coinbasevalue\" : n,              (numeric) maximum allowable "
            "input to coinbase transaction, including the generation award and "
            "transaction fees (in Satoshis)\n"
            "  \"coinbasetxn\" : { ... },          (json object) information "
            "for coinbase transaction\n"
            "  \"target\" : \"xxxx\",                (string) The hash target\n"
            "  \"mintime\" : xxx,                  (numeric) The minimum "
            "timestamp appropriate for next block time in seconds since epoch "
            "(Jan 1 1970 GMT)\n"
            "  \"mutable\" : [                     (array of string) list of "
            "ways the block template may be changed \n"
            "     \"value\"                          (string) A way the block "
            "template may be changed, e.g. 'time', 'transactions', "
            "'prevblock'\n"
            "     ,...\n"
            "  ],\n"
            "  \"noncerange\" : \"00000000ffffffff\",(string) A range of valid "
            "nonces\n"
            "  \"sizelimit\" : n,                  (numeric) limit of block "
            "size\n"
            "  \"curtime\" : ttt,                  (numeric) current timestamp "
            "in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"bits\" : \"xxxxxxxx\",              (string) compressed "
            "target of next block\n"
            "  \"height\" : n                      (numeric) The height of the "
            "next block\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("getblocktemplate", "") +
            HelpExampleRpc("getblocktemplate", ""));
    }

    if(httpReq == nullptr)
        return;

    std::string strMode = "template";
    UniValue lpval = NullUniValue;
    std::set<std::string> setClientRules;
    if (request.params.size() > 0) {
        const UniValue &oparam = request.params[0].get_obj();
        const UniValue &modeval = find_value(oparam, "mode");
        if (modeval.isStr()) {
            strMode = modeval.get_str();
        } else if (modeval.isNull()) {
            /* Do nothing */
        } else {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");
        }
        lpval = find_value(oparam, "longpollid");

        if (strMode == "proposal") {
            const UniValue &dataval = find_value(oparam, "data");
            if (!dataval.isStr()) {
                throw JSONRPCError(RPC_TYPE_ERROR,
                                   "Missing data String key for proposal");
            }

            CBlock block;
            if (!DecodeHexBlk(block, dataval.get_str())) {
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR,
                                   "Block decode failed");
            }

            uint256 hash = block.GetHash();
            // result is of type UniValue because of BIP22ValidationResult return type
            UniValue result;
            if (auto pindex = mapBlockIndex.Get(hash); pindex) {
                if (pindex->IsValid(BlockValidity::SCRIPTS))
                {
                    result = "duplicate";
                }
                else if (pindex->getStatus().isInvalid())
                {
                    result = "duplicate-invalid";
                }
                else
                {
                    result = "duplicate-inconclusive";
                }

            }
            else
            {
                LOCK(cs_main);

                CBlockIndex *const pindexPrev = chainActive.Tip();
                // TestBlockValidity only supports blocks built on the current Tip
                if (block.hashPrevBlock != pindexPrev->GetBlockHash())
                {
                    result = "inconclusive-not-best-prevblk";
                }
                else
                {
                    CValidationState state;
                    BlockValidationOptions validationOptions =
                        BlockValidationOptions().withCheckPoW(false);
                    TestBlockValidity(config, state, block, pindexPrev,
                                      validationOptions);
                    result = BIP22ValidationResult(config, state);
                }
            }

            // after start of writing chunks no exception must be thrown, otherwise JSON response will be invalid
            if (!processedInBatch)
            {
                httpReq->WriteHeader("Content-Type", "application/json");
                httpReq->StartWritingChunks(HTTP_OK);
            }

            {
                CHttpTextWriter httpWriter(*httpReq);
                CJSONWriter jWriter(httpWriter, false);
                jWriter.writeBeginObject();
                jWriter.pushKVJSONFormatted("result", result.write());
                jWriter.pushKV("error", nullptr);
                jWriter.pushKVJSONFormatted("id", request.id.write());
                jWriter.writeEndObject();
                jWriter.flush();
            }

            if (!processedInBatch)
            {
                httpReq->StopWritingChunks();
            }
            return;  
        }
    }

    if (strMode != "template") {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");
    }

    if (!g_connman) {
        throw JSONRPCError(
            RPC_CLIENT_P2P_DISABLED,
            "Error: Peer-to-peer functionality missing or disabled");
    }

    // "-standalone" is an undocumented option.
    if ((g_connman->GetNodeCount(CConnman::CONNECTIONS_ALL) == 0) && !gArgs.IsArgSet("-standalone"))
    {
        throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Bitcoin is not connected!");
    }

    if (IsInitialBlockDownload()) {
        throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD,
                           "Bitcoin is downloading blocks...");
    }

    static unsigned int nTransactionsUpdatedLast;

    if (!lpval.isNull()) {
        // Wait to respond until either the best block changes, OR a minute has
        // passed and there are more transactions
        uint256 hashWatchedChain;
        boost::system_time checktxtime;
        unsigned int nTransactionsUpdatedLastLP;

        if (lpval.isStr()) {
            // Format: <hashBestChain><nTransactionsUpdatedLast>
            std::string lpstr = lpval.get_str();

            hashWatchedChain.SetHex(lpstr.substr(0, 64));
            nTransactionsUpdatedLastLP = atoi64(lpstr.substr(64));
        } else {
            // NOTE: Spec does not specify behaviour for non-string longpollid,
            // but this makes testing easier
            hashWatchedChain = chainActive.Tip()->GetBlockHash();
            nTransactionsUpdatedLastLP = nTransactionsUpdatedLast;
        }

        checktxtime =
            boost::get_system_time() + boost::posix_time::minutes(1);

        boost::unique_lock<boost::mutex> lock(csBestBlock);
        while (chainActive.Tip()->GetBlockHash() == hashWatchedChain &&
               IsRPCRunning()) {
            if (!cvBlockChange.timed_wait(lock, checktxtime)) {
                // Timeout: Check transactions for update
                if (mempool.GetTransactionsUpdated() !=
                    nTransactionsUpdatedLastLP) {
                    break;
                }
                checktxtime += boost::posix_time::seconds(10);
            }
        }

        if (!IsRPCRunning()) {
            throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Shutting down");
        }
        // TODO: Maybe recheck connections/IBD and (if something wrong) send an
        // expires-immediately template to stop miners?
    }

    static std::mutex localMutex;
    std::lock_guard lock{ localMutex };

    // Update block - these static variables are protected by locked localMutex
    static CBlockIndex *pindexPrev;
    static int64_t nStart;
    static std::unique_ptr<CBlockTemplate> pblocktemplate;

    const CBlockIndex* tip;

    {
        LOCK(cs_main);

        tip = chainActive.Tip();

        if (pindexPrev != tip ||
            (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast &&
             ((GetTime() - nStart > 5) || nTransactionsUpdatedLast < mempool.GetFrozenTxnUpdatedAt()))) {
            // Clear pindexPrev so future calls make a new block, despite any
            // failures from here on
            pindexPrev = nullptr;

            // Update other fields for tracking state of this candidate
            nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();
            nStart = GetTime();

            // Create new block
            if(!mining::g_miningFactory) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "No mining factory available");
            }
            CScript scriptDummy = CScript() << OP_TRUE;
            pblocktemplate = mining::g_miningFactory->GetAssembler()->CreateNewBlock(scriptDummy, pindexPrev);
            if (!pblocktemplate) {
                throw JSONRPCError(RPC_OUT_OF_MEMORY, "Out of memory");
            }
        }
    }

    // pointer for convenience
    CBlockRef blockRef = pblocktemplate->GetBlockRef();
    CBlock *pblock = blockRef.get();

    // Update nTime
    mining::UpdateTime(pblock, config, pindexPrev);
    pblock->nNonce = 0;

    // after start of writing chunks no exception must be thrown, otherwise JSON response will be invalid
    if (!processedInBatch)
    {
        httpReq->WriteHeader("Content-Type", "application/json");
        httpReq->StartWritingChunks(HTTP_OK);
    }

    {
        CHttpTextWriter httpWriter(*httpReq);
        CJSONWriter jWriter(httpWriter, false);

        jWriter.writeBeginObject();
        jWriter.pushKNoComma("result");
        jWriter.writeBeginObject();

        jWriter.writeBeginArray("capabilities");
        jWriter.pushV("proposal");
        jWriter.writeEndArray();

        jWriter.pushKV("version", pblock->nVersion);
        jWriter.pushKV("previousblockhash", pblock->hashPrevBlock.GetHex());
        jWriter.writeBeginArray("transactions");

        std::map<uint256, int64_t> setTxIndex;
        int i = 0;
        for (const auto &it : pblock->vtx) {
            const CTransaction &tx = *it;
            uint256 txId = tx.GetId();
            setTxIndex[txId] = i++;

            if (tx.IsCoinBase()) {
                continue;
            }

            jWriter.writeBeginObject();

            jWriter.pushK("data");
            jWriter.pushQuote();
            jWriter.flush();
            // EncodeHexTx supports streaming (large transaction's hex should be chunked)
            EncodeHexTx(tx, httpWriter, RPCSerializationFlags());
            jWriter.pushQuote();

            jWriter.pushKV("txid", txId.GetHex());
            jWriter.pushKV("hash", tx.GetHash().GetHex());

            jWriter.writeBeginArray("depends");
            for (const CTxIn &in : tx.vin) {
                if (setTxIndex.count(in.prevout.GetTxId())) {
                    jWriter.pushV(setTxIndex[in.prevout.GetTxId()]);
                }
            }
            jWriter.writeEndArray();

            unsigned int index_in_template = i - 1;
            jWriter.pushKV("fee", pblocktemplate->vTxFees[index_in_template].GetSatoshis());

            jWriter.writeEndObject();
        }

        jWriter.writeEndArray();

        jWriter.writeBeginObject("coinbaseaux");
        jWriter.pushKV("flags", HexStr(COINBASE_FLAGS.begin(), COINBASE_FLAGS.end()));
        jWriter.writeEndObject();

        jWriter.pushKV("coinbasevalue", (int64_t)pblock->vtx[0]->vout[0].nValue.GetSatoshis());

        jWriter.pushKV("longpollid", tip->GetBlockHash().GetHex() + i64tostr(nTransactionsUpdatedLast));

        arith_uint256 hashTarget = arith_uint256().SetCompact(pblock->nBits);
        jWriter.pushKV("target", hashTarget.GetHex());

        jWriter.pushKV("mintime", (int64_t)pindexPrev->GetMedianTimePast() + 1);

        jWriter.writeBeginArray("mutable");
        jWriter.pushV("time");
        jWriter.pushV("transactions");
        jWriter.pushV("prevblock");
        jWriter.writeEndArray();

        jWriter.pushKV("noncerange", "00000000ffffffff");

        int64_t defaultmaxBlockSize = config.GetChainParams().GetDefaultBlockSizeParams().maxGeneratedBlockSizeAfter;
        jWriter.pushKV("sizelimit", defaultmaxBlockSize);

        jWriter.pushKV("curtime", pblock->GetBlockTime());
        jWriter.pushKV("bits", strprintf("%08x", pblock->nBits));
        jWriter.pushKV("height", (int64_t)(pindexPrev->GetHeight() + 1));

        jWriter.writeEndObject();

        jWriter.pushKV("error", nullptr);
        jWriter.pushKVJSONFormatted("id", request.id.write());
        jWriter.writeEndObject();
        jWriter.flush();
    }

    if (!processedInBatch)
    {
        httpReq->StopWritingChunks();
    } 
}

class submitblock_StateCatcher : public CValidationInterface {
public:
    uint256 hash;
    bool found;
    CValidationState state;

    submitblock_StateCatcher(const uint256 &hashIn)
        : hash(hashIn), found(false), state() {}

protected:
    void BlockChecked(const CBlock &block,
                      const CValidationState &stateIn) override {
        if (block.GetHash() != hash) {
            return;
        }

        found = true;
        state = stateIn;
    }
};

UniValue processBlock(
    const Config& config,
    const std::shared_ptr<CBlock>& blockptr,
    std::function<bool(const Config&, const std::shared_ptr<CBlock>&)> performBlockOperation)
{
    CBlock &block = *blockptr;
    if (block.vtx.empty() || !block.vtx[0]->IsCoinBase()) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR,
                           "Block does not start with a coinbase");
    }

    if (block.vtx[0]->HasP2SHOutput()) {
        throw JSONRPCError(RPC_TRANSACTION_REJECTED, "bad-txns-vout-p2sh");
    }

    uint256 hash = block.GetHash();
    bool fBlockPresent = false;

    if (auto pindex = mapBlockIndex.Get(hash); pindex) {
        if (pindex->IsValid(BlockValidity::SCRIPTS)) {
            return "duplicate";
        }
        if (pindex->getStatus().isInvalid()) {
            return "duplicate-invalid";
        }
        // Otherwise, we might only have the header - process the block
        // before returning
        fBlockPresent = true;
    }

    submitblock_StateCatcher sc(block.GetHash());
    RegisterValidationInterface(&sc);
    bool fAccepted = performBlockOperation(config, blockptr);
    UnregisterValidationInterface(&sc);
    if (fBlockPresent) {
        if (fAccepted && !sc.found) {
            return "duplicate-inconclusive";
        }
        return "duplicate";
    }

    if (!sc.found) {
        return "inconclusive";
    }

    return BIP22ValidationResult(config, sc.state);
}

static UniValue verifyblockcandidate(const Config &config,
                            const JSONRPCRequest &request) {
    if (request.fHelp || request.params.size() < 1 ||
        request.params.size() > 2) {
        throw std::runtime_error(
            "verifyblockcandidate \"hexdata\" ( \"jsonparametersobject\" )\n"
            "\nTest a block template for validity without a valid PoW.\n"
            "The 'jsonparametersobject' parameter is currently ignored.\n"
            "See https://en.bitcoin.it/wiki/BIP_0022 for full specification.\n"

            "\nArguments\n"
            "1. \"hexdata\"        (string, required) the hex-encoded block "
            "data to submit\n"
            "2. \"parameters\"     (string, optional) object of optional "
            "parameters\n"
            "    {\n"
            "      \"workid\" : \"id\"    (string, optional) if the server "
            "provided a workid, it MUST be included with submissions\n"
            "    }\n"
            "\nResult:\n"
            "\nExamples:\n" +
            HelpExampleCli("verifyblockcandidate", "\"mydata\"") +
            HelpExampleRpc("verifyblockcandidate", "\"mydata\""));
    }

    std::shared_ptr<CBlock> blockptr = std::make_shared<CBlock>();
    CBlock &block = *blockptr;
    if (!DecodeHexBlk(block, request.params[0].get_str())) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block decode failed");
    }

    auto verifyBlock = [](const Config& config , const std::shared_ptr<CBlock>& blockptr)
    {
        return VerifyNewBlock(config, blockptr);
    };
    return processBlock(config, blockptr, verifyBlock);
}

static UniValue submitblock(const Config &config,
                            const JSONRPCRequest &request) {
    if (request.fHelp || request.params.size() < 1 ||
        request.params.size() > 2) {
        throw std::runtime_error(
            "submitblock \"hexdata\" ( \"jsonparametersobject\" )\n"
            "\nAttempts to submit new block to network.\n"
            "The 'jsonparametersobject' parameter is currently ignored.\n"
            "See https://en.bitcoin.it/wiki/BIP_0022 for full specification.\n"

            "\nArguments\n"
            "1. \"hexdata\"        (string, required) the hex-encoded block "
            "data to submit\n"
            "2. \"parameters\"     (string, optional) object of optional "
            "parameters\n"
            "    {\n"
            "      \"workid\" : \"id\"    (string, optional) if the server "
            "provided a workid, it MUST be included with submissions\n"
            "    }\n"
            "\nResult:\n"
            "\nExamples:\n" +
            HelpExampleCli("submitblock", "\"mydata\"") +
            HelpExampleRpc("submitblock", "\"mydata\""));
    }

    std::shared_ptr<CBlock> blockptr = std::make_shared<CBlock>();
    CBlock &block = *blockptr;
    if (!DecodeHexBlk(block, request.params[0].get_str())) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block decode failed");
    }

    auto submitBlock = [](const Config& config , const std::shared_ptr<CBlock>& blockptr)
    {
        CScopedBlockOriginRegistry reg(blockptr->GetHash(), "submitblock");
        return ProcessNewBlock(config, blockptr, true, nullptr, CBlockSource::MakeRPC());
    };
    return processBlock(config, blockptr, submitBlock);
}

// clang-format off
static const CRPCCommand commands[] = {
    //  category   name                     actor (function)       okSafeMode
    //  ---------- ------------------------ ---------------------- ----------
    {"mining",     "getnetworkhashps",      getnetworkhashps,      true, {"nblocks", "height"}},
    {"mining",     "getmininginfo",         getmininginfo,         true, {}},
    {"mining",     "prioritisetransaction", prioritisetransaction, true, {"txid", "priority_delta", "fee_delta"}},
    {"mining",     "getblocktemplate",      getblocktemplate,      true, {"template_request"}},
    {"mining",     "verifyblockcandidate",  verifyblockcandidate,  true, {"hexdata", "parameters"}},
    {"mining",     "submitblock",           submitblock,           true, {"hexdata", "parameters"}},

    {"generating", "generatetoaddress",     generatetoaddress,     true, {"nblocks", "address", "maxtries"}},
};
// clang-format on

void RegisterMiningRPCCommands(CRPCTable &t) {
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
