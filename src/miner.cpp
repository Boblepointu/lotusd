// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <miner.h>

#include <amount.h>
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <config.h>
#include <consensus/activation.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <minerfund.h>
#include <net.h>
#include <policy/policy.h>
#include <policy/settings.h>
#include <pow/pow.h>
#include <primitives/transaction.h>
#include <timedata.h>
#include <util/moneystr.h>
#include <util/system.h>
#include <validation.h>

#include <algorithm>
#include <utility>

int64_t UpdateTime(CBlockHeader *pblock, const CChainParams &chainParams,
                   const CBlockIndex *pindexPrev) {
    int64_t nOldTime = pblock->GetBlockTime();
    int64_t nNewTime =
        std::max(pindexPrev->GetMedianTimePast() + 1, GetAdjustedTime());

    if (nOldTime < nNewTime) {
        pblock->SetBlockTime(nNewTime);
    }

    // Updating time can change work required on testnet:
    if (chainParams.GetConsensus().fPowAllowMinDifficultyBlocks) {
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, chainParams);
    }

    return nNewTime - nOldTime;
}

Amount GetBlockRewardFromFees(Amount nFees) {
    // 50% of fees get burned.
    return nFees / 2;
}

uint64_t CTxMemPoolModifiedEntry::GetVirtualSizeWithAncestors() const {
    return GetVirtualTransactionSize(nSizeWithAncestors,
                                     nSigOpCountWithAncestors);
}

BlockAssembler::Options::Options()
    // : nExcessiveBlockSize(1024 * 16),
    //   nMaxGeneratedBlockSize(1024 * 16),
    : nExcessiveBlockSize(DEFAULT_MAX_BLOCK_SIZE),
      nMaxGeneratedBlockSize(DEFAULT_MAX_GENERATED_BLOCK_SIZE),
      blockMinFeeRate(DEFAULT_BLOCK_MIN_TX_FEE_PER_KB) {}

BlockAssembler::BlockAssembler(const CChainParams &params,
                               const CTxMemPool &mempool,
                               const Options &options)
    : chainParams(params), m_mempool(mempool) {
    blockMinFeeRate = options.blockMinFeeRate;
    enableMinerFund = options.enableMinerFund;
    // Limit size to between 1K and options.nExcessiveBlockSize -1K for sanity:
    nMaxGeneratedBlockSize = std::max<uint64_t>(
        1000, std::min<uint64_t>(options.nExcessiveBlockSize - 1000,
                                 options.nMaxGeneratedBlockSize));
    // Calculate the max consensus sigchecks for this block.
    auto nMaxBlockSigChecks = GetMaxBlockSigChecksCount(nMaxGeneratedBlockSize);
    // Allow the full amount of signature check operations in lieu of a separate
    // config option. (We are mining relayed transactions with validity cached
    // by everyone else, and so the block will propagate quickly, regardless of
    // how many sigchecks it contains.)
    nMaxGeneratedBlockSigChecks = nMaxBlockSigChecks;
}

static BlockAssembler::Options DefaultOptions(const Config &config) {
    // Block resource limits
    // If -blockmaxsize is not given, limit to DEFAULT_MAX_GENERATED_BLOCK_SIZE
    // If only one is given, only restrict the specified resource.
    // If both are given, restrict both.
    BlockAssembler::Options options;

    // Get the excessive block size from configuration
    options.nExcessiveBlockSize = config.GetMaxBlockSize();
    
    // Log the excessive block size for debugging
    LogPrint(BCLog::MINING, "DefaultOptions: Excessive block size: %u bytes\n", 
             options.nExcessiveBlockSize);
    
    // Default the maximum generated block size to either the excessive block size - 1000 bytes
    // or the DEFAULT_MAX_GENERATED_BLOCK_SIZE, whichever is smaller
    options.nMaxGeneratedBlockSize = std::min<uint64_t>(
        options.nExcessiveBlockSize - 1000, DEFAULT_MAX_GENERATED_BLOCK_SIZE);

    // If the blockmaxsize parameter is set, use that value instead (but still
    // capped by the excessive block size)
    if (gArgs.IsArgSet("-blockmaxsize")) {
        // Convert the value to a number and log it
        uint64_t requestedMaxBlockSize = gArgs.GetArg("-blockmaxsize", DEFAULT_MAX_GENERATED_BLOCK_SIZE);
        LogPrintf("DefaultOptions: User requested maximum block size: %u bytes\n", requestedMaxBlockSize);
        
        // Apply a safety margin of 3000 bytes to account for:
        // 1. Coinbase transaction size variations
        // 2. Block metadata overhead
        // 3. Other potential size discrepancies in accounting
        // This ensures the final block size will be below the specified limit
        uint64_t adjustedMaxBlockSize = requestedMaxBlockSize > 3000 ? requestedMaxBlockSize - 3000 : requestedMaxBlockSize;
        
        // Cap by the excessive block size
        uint64_t cappedMaxBlockSize = std::min<uint64_t>(
            adjustedMaxBlockSize, options.nExcessiveBlockSize - 1000);
            
        // Print detailed information about how we arrived at the final value
        LogPrintf("DefaultOptions: Processing -blockmaxsize parameter:\n"
                  "  - Raw parameter value: %u bytes\n"
                  "  - After safety margin: %u bytes\n"
                  "  - After capping by excessive block size: %u bytes\n", 
                  requestedMaxBlockSize,
                  adjustedMaxBlockSize,
                  cappedMaxBlockSize);
        
        options.nMaxGeneratedBlockSize = cappedMaxBlockSize;
    }
    
    LogPrintf("DefaultOptions: Final maximum generated block size: %u bytes\n", 
              options.nMaxGeneratedBlockSize);
    
    Amount n = Amount::zero();
    if (gArgs.IsArgSet("-blockmintxfee") &&
        ParseMoney(gArgs.GetArg("-blockmintxfee", ""), n)) {
        options.blockMinFeeRate = CFeeRate(n);
    }

    options.enableMinerFund = config.EnableMinerFund();

    return options;
}

BlockAssembler::BlockAssembler(const Config &config, const CTxMemPool &mempool)
    : BlockAssembler(config.GetChainParams(), mempool, DefaultOptions(config)) {
}

void BlockAssembler::resetBlock() {
    inBlock.clear();

    // Reserve space for coinbase tx.
    nBlockSize = 1000;
    nBlockSigOps = 100;

    // These counters do not include coinbase tx.
    nBlockTx = 0;
    nFees = Amount::zero();
}

std::optional<int64_t> BlockAssembler::m_last_block_num_txs{std::nullopt};
std::optional<int64_t> BlockAssembler::m_last_block_size{std::nullopt};

std::unique_ptr<CBlockTemplate>
BlockAssembler::CreateNewBlock(const CScript &scriptPubKeyIn) {
    int64_t nTimeStart = GetTimeMicros();

    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());
    if (!pblocktemplate.get()) {
        return nullptr;
    }

    // Pointer for convenience.
    CBlock *const pblock = &pblocktemplate->block;

    // Add dummy coinbase tx as first transaction.  It is updated at the end.
    pblocktemplate->entries.emplace_back(CTransactionRef(), -SATOSHI, -1);

    LOCK2(cs_main, m_mempool.cs);
    CBlockIndex *pindexPrev = ::ChainActive().Tip();
    assert(pindexPrev != nullptr);
    nHeight = pindexPrev->nHeight + 1;

    const Consensus::Params &consensusParams = chainParams.GetConsensus();

    // Version must always be 1
    pblock->nHeaderVersion = 1;
    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (chainParams.MineBlocksOnDemand()) {
        pblock->nHeaderVersion =
            gArgs.GetArg("-blockversion", pblock->nHeaderVersion);
    }

    // Fill in header.
    pblock->hashPrevBlock = pindexPrev->GetBlockHash();
    pblock->SetBlockTime(GetAdjustedTime());
    UpdateTime(pblock, chainParams, pindexPrev);
    pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, chainParams);
    pblock->nNonce = 0;
    pblock->nHeight = nHeight;
    if (nHeight % EPOCH_NUM_BLOCKS == 0) { // new epoch started
        pblock->hashEpochBlock = pblock->hashPrevBlock;
    } else {
        pblock->hashEpochBlock = pindexPrev->hashEpochBlock;
    }
    pblock->hashExtendedMetadata = SerializeHash(pblock->vMetadata);

    nLockTimeCutoff = pindexPrev->GetMedianTimePast();

    int nPackagesSelected = 0;
    int nDescendantsUpdated = 0;
    addPackageTxs(nPackagesSelected, nDescendantsUpdated);

    // We make sure transaction are canonically ordered.
    std::sort(
        std::begin(pblocktemplate->entries) + 1,
        std::end(pblocktemplate->entries),
        [](const CBlockTemplateEntry &a, const CBlockTemplateEntry &b) -> bool {
            return a.tx->GetId() < b.tx->GetId();
        });

    // Copy all the transactions refs into the block
    pblock->vtx.reserve(pblocktemplate->entries.size());
    for (const CBlockTemplateEntry &entry : pblocktemplate->entries) {
        pblock->vtx.push_back(entry.tx);
    }

    int64_t nTime1 = GetTimeMicros();
    const Amount amountFeeReward = GetBlockRewardFromFees(nFees);

    m_last_block_num_txs = nBlockTx;
    m_last_block_size = nBlockSize;

    // Create coinbase transaction.
    CMutableTransaction coinbaseTx;
    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].prevout = COutPoint();
    coinbaseTx.vin[0].scriptSig = CScript() << OP_0 << OP_0;
    coinbaseTx.vout.resize(2);
    coinbaseTx.vout[0].scriptPubKey = CScript() << OP_RETURN << COINBASE_PREFIX
                                                << nHeight;
    coinbaseTx.vout[0].nValue = Amount::zero();
    coinbaseTx.vout[1].scriptPubKey = scriptPubKeyIn;
    coinbaseTx.vout[1].nValue =
        amountFeeReward + GetBlockSubsidy(pblock->nBits, consensusParams);

    const Amount coinbaseValue = coinbaseTx.vout[1].nValue;
    const std::vector<CTxOut> requiredOutputs = GetMinerFundRequiredOutputs(
        consensusParams, enableMinerFund, pindexPrev, coinbaseValue);
    for (const CTxOut &requiredOutput : requiredOutputs) {
        coinbaseTx.vout[1].nValue -= requiredOutput.nValue;
        coinbaseTx.vout.push_back(requiredOutput);
    }

    // Make sure the coinbase is big enough.
    uint64_t coinbaseSize = ::GetSerializeSize(coinbaseTx, PROTOCOL_VERSION);
    if (coinbaseSize < MIN_TX_SIZE) {
        coinbaseTx.vin[0].scriptSig
            << std::vector<uint8_t>(MIN_TX_SIZE - coinbaseSize - 1);
    }

    pblocktemplate->entries[0].tx = MakeTransactionRef(coinbaseTx);
    pblocktemplate->entries[0].fees = -1 * nFees;
    pblock->vtx[0] = pblocktemplate->entries[0].tx;

    uint64_t nSerializeSize = GetSerializeSize(*pblock, PROTOCOL_VERSION);
    uint64_t calculatedBlockSize = 0;
    for (const auto& tx : pblock->vtx) {
        calculatedBlockSize += GetSerializeSize(*tx, PROTOCOL_VERSION);
    }
    int64_t realOverhead = nSerializeSize - calculatedBlockSize;
    if (realOverhead < 0) {
        LogPrintf("WARNING: Block size calculation error detected! nSerializeSize=%u, sum of tx sizes=%lld\n", 
                 nSerializeSize, calculatedBlockSize);
        realOverhead = 0;
    }

    LogPrintf("CreateNewBlock(): total size: %u txs: %u fees: %ld sigops %d\n",
              nSerializeSize, nBlockTx, nFees, nBlockSigOps);

    // Always log detailed size information to help diagnose issues
    uint64_t requestedMaxBlockSize = gArgs.IsArgSet("-blockmaxsize") ? 
                                    gArgs.GetArg("-blockmaxsize", DEFAULT_MAX_GENERATED_BLOCK_SIZE) : 
                                    DEFAULT_MAX_GENERATED_BLOCK_SIZE;
    LogPrintf("CreateNewBlock(): size details - requested max: %u, internal limit: %u, coinbase size: %u, overhead: %lld\n",
              requestedMaxBlockSize, nMaxGeneratedBlockSize, 
              coinbaseSize, realOverhead);
    
    // Add detailed information about all transaction sizes for debugging
    LogPrint(BCLog::MINING, "Block transaction sizes:\n");
    for (size_t i = 0; i < pblock->vtx.size(); i++) {
        LogPrint(BCLog::MINING, "  tx[%zu]: %u bytes\n", i, GetSerializeSize(*pblock->vtx[i], PROTOCOL_VERSION));
    }

    // Fill in size
    pblock->SetSize(nSerializeSize);
    pblocktemplate->entries[0].sigOpCount = 0;

    BlockValidationState state;
    if (!TestBlockValidity(state, chainParams, *pblock, pindexPrev,
                           BlockValidationOptions(nMaxGeneratedBlockSize)
                               .withMinerFund(enableMinerFund)
                               .withCheckPoW(false)
                               .withCheckMerkleRoot(false))) {
        throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s",
                                           __func__, state.ToString()));
    }
    int64_t nTime2 = GetTimeMicros();

    LogPrint(BCLog::BENCH,
             "CreateNewBlock() packages: %.2fms (%d packages, %d updated "
             "descendants), validity: %.2fms (total %.2fms)\n",
             0.001 * (nTime1 - nTimeStart), nPackagesSelected,
             nDescendantsUpdated, 0.001 * (nTime2 - nTime1),
             0.001 * (nTime2 - nTimeStart));

    return std::move(pblocktemplate);
}

void BlockAssembler::onlyUnconfirmed(CTxMemPool::setEntries &testSet) {
    for (CTxMemPool::setEntries::iterator iit = testSet.begin();
         iit != testSet.end();) {
        // Only test txs not already in the block.
        if (inBlock.count(*iit)) {
            testSet.erase(iit++);
        } else {
            iit++;
        }
    }
}

bool BlockAssembler::TestPackage(uint64_t packageSize,
                                 int64_t packageSigOps) const {
    auto blockSizeWithPackage = nBlockSize + packageSize;
    
    // Be more conservative with the size limit - leave more room for the coinbase
    // transaction and other overhead that might be added later
    if (blockSizeWithPackage >= nMaxGeneratedBlockSize - 1000) {
        LogPrint(BCLog::MINING, "TestPackage: package of size %u would exceed block size limit (current: %u, max: %u)\n",
                packageSize, nBlockSize, nMaxGeneratedBlockSize);
        return false;
    }

    if (nBlockSigOps + packageSigOps >= nMaxGeneratedBlockSigChecks) {
        return false;
    }

    return true;
}

/**
 * Perform transaction-level checks before adding to block:
 * - Transaction finality (locktime)
 * - Serialized size (in case -blockmaxsize is in use)
 */
bool BlockAssembler::TestPackageTransactions(
    const CTxMemPool::setEntries &package) {
    uint64_t nPotentialBlockSize = nBlockSize;
    for (CTxMemPool::txiter it : package) {
        TxValidationState state;
        if (!ContextualCheckTransaction(chainParams.GetConsensus(), it->GetTx(),
                                        state, nHeight, nLockTimeCutoff,
                                        nMedianTimePast)) {
            return false;
        }

        uint64_t nTxSize = ::GetSerializeSize(it->GetTx(), PROTOCOL_VERSION);
        if (nPotentialBlockSize + nTxSize >= nMaxGeneratedBlockSize) {
            return false;
        }

        nPotentialBlockSize += nTxSize;
    }

    return true;
}

void BlockAssembler::AddToBlock(CTxMemPool::txiter iter) {
    pblocktemplate->entries.emplace_back(iter->GetSharedTx(), iter->GetFee(),
                                         iter->GetSigOpCount());
    nBlockSize += iter->GetTxSize();
    ++nBlockTx;
    nBlockSigOps += iter->GetSigOpCount();
    nFees += iter->GetFee();
    inBlock.insert(iter);

    bool fPrintPriority =
        gArgs.GetBoolArg("-printpriority", DEFAULT_PRINTPRIORITY);
    if (fPrintPriority) {
        LogPrintf(
            "fee %s txid %s\n",
            CFeeRate(iter->GetModifiedFee(), iter->GetTxSize()).ToString(),
            iter->GetTx().GetId().ToString());
    }
}

int BlockAssembler::UpdatePackagesForAdded(
    const CTxMemPool::setEntries &alreadyAdded,
    indexed_modified_transaction_set &mapModifiedTx) {
    int nDescendantsUpdated = 0;
    for (CTxMemPool::txiter it : alreadyAdded) {
        CTxMemPool::setEntries descendants;
        m_mempool.CalculateDescendants(it, descendants);
        // Insert all descendants (not yet in block) into the modified set.
        for (CTxMemPool::txiter desc : descendants) {
            if (alreadyAdded.count(desc)) {
                continue;
            }

            ++nDescendantsUpdated;
            modtxiter mit = mapModifiedTx.find(desc);
            if (mit == mapModifiedTx.end()) {
                CTxMemPoolModifiedEntry modEntry(desc);
                modEntry.nSizeWithAncestors -= it->GetTxSize();
                modEntry.nModFeesWithAncestors -= it->GetModifiedFee();
                modEntry.nSigOpCountWithAncestors -= it->GetSigOpCount();
                mapModifiedTx.insert(modEntry);
            } else {
                mapModifiedTx.modify(mit, update_for_parent_inclusion(it));
            }
        }
    }

    return nDescendantsUpdated;
}

// Skip entries in mapTx that are already in a block or are present in
// mapModifiedTx (which implies that the mapTx ancestor state is stale due to
// ancestor inclusion in the block). Also skip transactions that we've already
// failed to add. This can happen if we consider a transaction in mapModifiedTx
// and it fails: we can then potentially consider it again while walking mapTx.
// It's currently guaranteed to fail again, but as a belt-and-suspenders check
// we put it in failedTx and avoid re-evaluation, since the re-evaluation would
// be using cached size/sigops/fee values that are not actually correct.
bool BlockAssembler::SkipMapTxEntry(
    CTxMemPool::txiter it, indexed_modified_transaction_set &mapModifiedTx,
    CTxMemPool::setEntries &failedTx) {
    assert(it != m_mempool.mapTx.end());
    return mapModifiedTx.count(it) || inBlock.count(it) || failedTx.count(it);
}

void BlockAssembler::SortForBlock(
    const CTxMemPool::setEntries &package,
    std::vector<CTxMemPool::txiter> &sortedEntries) {
    // Sort package by ancestor count. If a transaction A depends on transaction
    // B, then A's ancestor count must be greater than B's. So this is
    // sufficient to validly order the transactions for block inclusion.
    sortedEntries.clear();
    sortedEntries.insert(sortedEntries.begin(), package.begin(), package.end());
    std::sort(sortedEntries.begin(), sortedEntries.end(),
              CompareTxIterByAncestorCount());
}

/**
 * addPackageTx includes transactions paying a fee by ensuring that
 * the partial ordering of transactions is maintained.  That is to say
 * children come after parents, despite having a potentially larger fee.
 * @param[out] nPackagesSelected    How many packages were selected
 * @param[out] nDescendantsUpdated  Number of descendant transactions updated
 */
void BlockAssembler::addPackageTxs(int &nPackagesSelected,
                                   int &nDescendantsUpdated) {
    // selection algorithm orders the mempool based on feerate of a
    // transaction including all unconfirmed ancestors. Since we don't remove
    // transactions from the mempool as we select them for block inclusion, we
    // need an alternate method of updating the feerate of a transaction with
    // its not-yet-selected ancestors as we go. This is accomplished by
    // walking the in-mempool descendants of selected transactions and storing
    // a temporary modified state in mapModifiedTxs. Each time through the
    // loop, we compare the best transaction in mapModifiedTxs with the next
    // transaction in the mempool to decide what transaction package to work
    // on next.

    // mapModifiedTx will store sorted packages after they are modified because
    // some of their txs are already in the block.
    indexed_modified_transaction_set mapModifiedTx;
    // Keep track of entries that failed inclusion, to avoid duplicate work.
    CTxMemPool::setEntries failedTx;

    // Start by adding all descendants of previously added txs to mapModifiedTx
    // and modifying them for their already included ancestors.
    UpdatePackagesForAdded(inBlock, mapModifiedTx);

    CTxMemPool::indexed_transaction_set::index<ancestor_score>::type::iterator
        mi = m_mempool.mapTx.get<ancestor_score>().begin();
    CTxMemPool::txiter iter;

    // Limit the number of attempts to add transactions to the block when it is
    // close to full; this is just a simple heuristic to finish quickly if the
    // mempool has a lot of entries.
    const int64_t MAX_CONSECUTIVE_FAILURES = 1000;
    int64_t nConsecutiveFailed = 0;

    while (mi != m_mempool.mapTx.get<ancestor_score>().end() ||
           !mapModifiedTx.empty()) {
        // First try to find a new transaction in mapTx to evaluate.
        if (mi != m_mempool.mapTx.get<ancestor_score>().end() &&
            SkipMapTxEntry(m_mempool.mapTx.project<0>(mi), mapModifiedTx,
                           failedTx)) {
            ++mi;
            continue;
        }

        // Now that mi is not stale, determine which transaction to evaluate:
        // the next entry from mapTx, or the best from mapModifiedTx?
        bool fUsingModified = false;

        modtxscoreiter modit = mapModifiedTx.get<ancestor_score>().begin();
        if (mi == m_mempool.mapTx.get<ancestor_score>().end()) {
            // We're out of entries in mapTx; use the entry from mapModifiedTx
            iter = modit->iter;
            fUsingModified = true;
        } else {
            // Try to compare the mapTx entry to the mapModifiedTx entry.
            iter = m_mempool.mapTx.project<0>(mi);
            if (modit != mapModifiedTx.get<ancestor_score>().end() &&
                CompareTxMemPoolEntryByAncestorFee()(
                    *modit, CTxMemPoolModifiedEntry(iter))) {
                // The best entry in mapModifiedTx has higher score than the one
                // from mapTx. Switch which transaction (package) to consider
                iter = modit->iter;
                fUsingModified = true;
            } else {
                // Either no entry in mapModifiedTx, or it's worse than mapTx.
                // Increment mi for the next loop iteration.
                ++mi;
            }
        }

        // We skip mapTx entries that are inBlock, and mapModifiedTx shouldn't
        // contain anything that is inBlock.
        assert(!inBlock.count(iter));

        uint64_t packageSize = iter->GetSizeWithAncestors();
        Amount packageFees = iter->GetModFeesWithAncestors();
        int64_t packageSigOps = iter->GetSigOpCountWithAncestors();
        if (fUsingModified) {
            packageSize = modit->nSizeWithAncestors;
            packageFees = modit->nModFeesWithAncestors;
            packageSigOps = modit->nSigOpCountWithAncestors;
        }

        if (packageFees < blockMinFeeRate.GetFee(packageSize)) {
            // Don't include this package, but don't stop yet because something
            // else we might consider may have a sufficient fee rate (since txes
            // are ordered by virtualsize feerate, not actual feerate).
            if (fUsingModified) {
                // Since we always look at the best entry in mapModifiedTx, we
                // must erase failed entries so that we can consider the next
                // best entry on the next loop iteration
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }
            continue;
        }

        // The following must not use virtual size since TestPackage relies on
        // having an accurate call to
        // GetMaxBlockSigOpsCount(blockSizeWithPackage).
        if (!TestPackage(packageSize, packageSigOps)) {
            if (fUsingModified) {
                // Since we always look at the best entry in mapModifiedTx, we
                // must erase failed entries so that we can consider the next
                // best entry on the next loop iteration
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }

            ++nConsecutiveFailed;

            if (nConsecutiveFailed > MAX_CONSECUTIVE_FAILURES &&
                nBlockSize > nMaxGeneratedBlockSize - 1000) {
                // Give up if we're close to full and haven't succeeded in a
                // while.
                break;
            }

            continue;
        }

        CTxMemPool::setEntries ancestors;
        uint64_t nNoLimit = std::numeric_limits<uint64_t>::max();
        std::string dummy;
        m_mempool.CalculateMemPoolAncestors(*iter, ancestors, nNoLimit,
                                            nNoLimit, nNoLimit, nNoLimit, dummy,
                                            false);

        onlyUnconfirmed(ancestors);
        ancestors.insert(iter);

        // Test if all tx's are Final.
        if (!TestPackageTransactions(ancestors)) {
            if (fUsingModified) {
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }
            continue;
        }

        // This transaction will make it in; reset the failed counter.
        nConsecutiveFailed = 0;

        // Package can be added. Sort the entries in a valid order.
        std::vector<CTxMemPool::txiter> sortedEntries;
        SortForBlock(ancestors, sortedEntries);

        for (auto &entry : sortedEntries) {
            AddToBlock(entry);
            // Erase from the modified set, if present
            mapModifiedTx.erase(entry);
        }

        ++nPackagesSelected;

        // Update transactions that depend on each of these
        nDescendantsUpdated += UpdatePackagesForAdded(ancestors, mapModifiedTx);
    }
    
    // Log the final block size as a percentage of the maximum allowed size
    LogPrintf("addPackageTxs(): Final block size: %u bytes (%.2f%% of max %u bytes)\n", 
              nBlockSize, 
              (nBlockSize * 100.0) / nMaxGeneratedBlockSize, 
              nMaxGeneratedBlockSize);
}

static const std::vector<uint8_t>
getExcessiveBlockSizeSig(uint64_t nExcessiveBlockSize) {
    std::string cbmsg = "/EB" + getSubVersionEB(nExcessiveBlockSize) + "/";
    std::vector<uint8_t> vec(cbmsg.begin(), cbmsg.end());
    return vec;
}

void IncrementExtraNonce(CBlock *pblock, const CBlockIndex *pindexPrev,
                         uint64_t nExcessiveBlockSize,
                         unsigned int &nExtraNonce) {
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock) {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }

    ++nExtraNonce;
    CMutableTransaction txCoinbase(*pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig =
        (CScript() << CScriptNum(nExtraNonce)
                   << getExcessiveBlockSizeSig(nExcessiveBlockSize));

    // Make sure the coinbase is big enough.
    uint64_t coinbaseSize = ::GetSerializeSize(txCoinbase, PROTOCOL_VERSION);
    if (coinbaseSize < MIN_TX_SIZE) {
        txCoinbase.vin[0].scriptSig
            << std::vector<uint8_t>(MIN_TX_SIZE - coinbaseSize - 1);
    }

    assert(txCoinbase.vin[0].scriptSig.size() <= MAX_COINBASE_SCRIPTSIG_SIZE);
    assert(::GetSerializeSize(txCoinbase, PROTOCOL_VERSION) >= MIN_TX_SIZE);

    pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
}
