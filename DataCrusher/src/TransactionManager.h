// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// Transaction manager: groups changes by XID, tracks commit context
// =============================================================================
#pragma once

#include "Platform.h"
#include "Constants.h"
#include "WalMessageParser.h"
#include "SignalTable.h"
#include <vector>
#include <string>
#include <optional>
#include <unordered_map>

namespace DC
{

// A complete transaction with all its changes
struct Transaction
{
    XID                      TransactionXID = XID_INVALID;
    LSN                      BeginLSN       = LSN_INVALID;
    LSN                      CommitLSN      = LSN_INVALID;
    int64_t                  CommitTimestamp = 0;
    std::vector<ChangeEvent> Changes;
    SessionContext           Context;       // user_id + origin from signal table
    TimePoint                BeginTime;     // When BEGIN was received
    TimePoint                CommitTime;    // When COMMIT was received

    size_t EstimatedMemoryBytes() const;
};

class TransactionManager final
{
public:
    TransactionManager();
    ~TransactionManager() = default;

    TransactionManager(const TransactionManager&) = delete;
    TransactionManager& operator=(const TransactionManager&) = delete;

    // Begin a new transaction
    void BeginTransaction(const BeginMessage& msg);

    // Add a change event to the active transaction
    void AddChange(ChangeEvent&& event);

    // Commit the active transaction: returns the completed transaction
    // Attaches session context from the signal table
    std::optional<Transaction> CommitTransaction(const CommitMessage& msg,
                                                  SignalTable& signalTable,
                                                  int backendPID);

    // Abort the current transaction (e.g., on error or rollback)
    void AbortTransaction();

    // Check if there's an active transaction
    bool HasActiveTransaction() const { return _ActiveXID != XID_INVALID; }

    // Get active transaction XID
    XID ActiveXID() const { return _ActiveXID; }

    // Statistics
    int64_t TotalTransactions()  const { return _TotalTransactions; }
    int64_t TotalChanges()       const { return _TotalChanges; }
    int64_t AbortedTransactions() const { return _AbortedTransactions; }
    size_t  ActiveTransactionBytes() const;

private:
    XID                      _ActiveXID = XID_INVALID;
    Transaction              _ActiveTransaction;

    // Statistics
    int64_t _TotalTransactions   = 0;
    int64_t _TotalChanges        = 0;
    int64_t _AbortedTransactions = 0;
};

} // namespace DC
