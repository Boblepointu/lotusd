// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/transaction.h>

#include <config.h>
#include <consensus/validation.h>
#include <net.h>
#include <net_processing.h>
#include <node/context.h>
#include <primitives/txid.h>
#include <txmempool.h>
#include <validation.h>
#include <validationinterface.h>

#include <future>

static TransactionError HandleATMPError(const TxValidationState &state,
                                        std::string &err_string_out) {
    err_string_out = state.ToString();
    if (state.IsInvalid()) {
        if (state.GetResult() == TxValidationResult::TX_MISSING_INPUTS) {
            return TransactionError::MISSING_INPUTS;
        }
        return TransactionError::MEMPOOL_REJECTED;
    } else {
        return TransactionError::MEMPOOL_ERROR;
    }
}

TransactionError BroadcastTransaction(NodeContext &node, const Config &config,
                                      const CTransactionRef tx,
                                      std::string &err_string,
                                      const Amount max_tx_fee, bool relay,
                                      bool wait_callback) {
    // BroadcastTransaction can be called by either sendrawtransaction RPC or
    // wallet RPCs. node.connman is assigned both before chain clients and
    // before RPC server is accepting calls, and reset after chain clients and
    // RPC sever are stopped. node.connman should never be null here.
    assert(node.connman);
    assert(node.mempool);
    std::promise<void> promise;
    TxId txid = tx->GetId();
    bool callback_set = false;

    { // cs_main scope
        LOCK(cs_main);
        // If the transaction is already confirmed in the chain, don't do
        // anything and return early.
        CCoinsViewCache &view = ::ChainstateActive().CoinsTip();
        for (size_t o = 0; o < tx->vout.size(); o++) {
            const Coin &existingCoin = view.AccessCoin(COutPoint(txid, o));
            // IsSpent doesn't mean the coin is spent, it means the output
            // doesn't exist. So if the output does exist, then this transaction
            // exists in the chain.
            if (!existingCoin.IsSpent()) {
                return TransactionError::ALREADY_IN_CHAIN;
            }
        }

        if (!node.mempool->exists(txid)) {
            // Transaction is not already in the mempool.
            TxValidationState state;
            if (max_tx_fee > Amount::zero()) {
                // First, call ATMP with test_accept and check the fee. If ATMP
                // fails here, return error immediately.
                Amount fee = Amount::zero();
                if (!AcceptToMemoryPool(config, *node.mempool, state, tx,
                                        true /* bypass_limits */,
                                        /* test_accept */ true, &fee)) {
                    return HandleATMPError(state, err_string);
                } else if (fee > max_tx_fee) {
                    return TransactionError::MAX_FEE_EXCEEDED;
                }
            }
            // Try to submit the transaction to the mempool.
            if (!AcceptToMemoryPool(config, *node.mempool, state, tx,
                                    true /* bypass_limits */)) {
                return HandleATMPError(state, err_string);
            }

            // Transaction was accepted to the mempool.

            if (wait_callback) {
                // For transactions broadcast from outside the wallet, make sure
                // that the wallet has been notified of the transaction before
                // continuing.
                //
                // This prevents a race where a user might call
                // sendrawtransaction with a transaction to/from their wallet,
                // immediately call some wallet RPC, and get a stale result
                // because callbacks have not yet been processed.
                CallFunctionInValidationInterfaceQueue(
                    [&promise] { promise.set_value(); });
                callback_set = true;
            }
        }
    } // cs_main

    if (callback_set) {
        // Wait until Validation Interface clients have been notified of the
        // transaction entering the mempool.
        promise.get_future().wait();
    }

    if (relay) {
        // the mempool tracks locally submitted transactions to make a
        // best-effort of initial broadcast
        node.mempool->AddUnbroadcastTx(txid);

        RelayTransaction(txid, *node.connman);
    }

    return TransactionError::OK;
}
