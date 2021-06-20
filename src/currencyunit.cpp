// Copyright (c) 2021 The Bitcoin Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <currencyunit.h>

#include <util/system.h>

void SetupCurrencyUnitOptions(ArgsManager &argsman) {
    // whether to use eCash default unit and address prefix
    argsman.AddArg("-ecash",
                   strprintf("Use the eCash prefixes and units (default: %s)",
                             DEFAULT_ECASH ? "true" : "false"),
                   ArgsManager::ALLOW_BOOL, OptionsCategory::OPTIONS);
    argsman.AddArg("-useecashprefix",
               strprintf("Use the eCash prefixes (default: %s)",
                         DEFAULT_ECASH ? "true" : "false"),
               ArgsManager::ALLOW_BOOL, OptionsCategory::OPTIONS);
}
