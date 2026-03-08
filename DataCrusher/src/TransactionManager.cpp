// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// Transaction manager implementation
// =============================================================================
#include "TransactionManager.h"
#include "Logger.h"

namespace DC
{

size_t Transaction::EstimatedMemoryBytes() const
{
    size_t bytes = sizeof(Transaction);
    for (const auto& change : Changes)
    {
        bytes += sizeof(ChangeEvent);
        bytes += change.SchemaName.capacity();
        bytes += change.TableName.capacity();
        for (const auto& col : change.OldValues)
            bytes += sizeof(ColumnValue) + col.Name.capacity() + col.TextValue.capacity();
        for (const auto& col : change.NewValues)
            bytes += sizeof(ColumnValue) + col.Name.capacity() + col.TextValue.capacity();
    }
    bytes += Context.UserID.capacity() + Context.AppName.capacity();
    return bytes;
}

TransactionManager::TransactionManager() = default;

void TransactionManager::BeginTransaction(const BeginMessage& msg)
{
    if (_ActiveXID != XID_INVALID)
    {
        LOG_WARN("BEGIN received while transaction xid={} is still active. Aborting previous.",
                 _ActiveXID);
        AbortTransaction();
    }

    _ActiveXID = msg.TransactionXID;
    _ActiveTransaction = {};
    _ActiveTransaction.TransactionXID = msg.TransactionXID;
    _ActiveTransaction.BeginLSN       = msg.FinalLSN;
    _ActiveTransaction.CommitTimestamp = msg.CommitTimestamp;
    _ActiveTransaction.BeginTime      = SteadyClock::now();
    _ActiveTransaction.Changes.reserve(32);

    LOG_TRACE("Transaction BEGIN xid={} lsn={}", _ActiveXID, FormatLSN(msg.FinalLSN));
}

void TransactionManager::AddChange(ChangeEvent&& event)
{
    if (_ActiveXID == XID_INVALID)
    {
        LOG_WARN("Change event received outside transaction context (op={}). Dropping.",
                 Operation::ToString(event.Operation));
        return;
    }

    // Safety check: limit memory usage
    auto currentBytes = _ActiveTransaction.EstimatedMemoryBytes();
    if (currentBytes > Performance::TxBufferMaxCapacity)
    {
        LOG_WARN("Transaction xid={} exceeded max buffer ({} bytes). Change dropped.",
                 _ActiveXID, currentBytes);
        return;
    }

    event.TransactionXID = _ActiveXID;
    _ActiveTransaction.Changes.push_back(std::move(event));
    ++_TotalChanges;
}

std::optional<Transaction> TransactionManager::CommitTransaction(
    const CommitMessage& msg, SignalTable& signalTable, int backendPID)
{
    if (_ActiveXID == XID_INVALID)
    {
        LOG_WARN("COMMIT received but no active transaction");
        return std::nullopt;
    }

    _ActiveTransaction.CommitLSN      = msg.CommitLSN;
    _ActiveTransaction.CommitTimestamp = msg.CommitTimestamp;
    _ActiveTransaction.CommitTime     = SteadyClock::now();

    // Fetch session context from signal table
    auto context = signalTable.QueryContext(backendPID);
    if (context.has_value())
    {
        _ActiveTransaction.Context = std::move(context.value());
        LOG_TRACE("Transaction xid={} context: user='{}' app_name='{}'",
                  _ActiveXID, _ActiveTransaction.Context.UserID,
                  _ActiveTransaction.Context.AppName);
    }
    else
    {
        // Try by XID if PID-based lookup failed
        auto ctxByXID = signalTable.QueryContextByXID(_ActiveXID);
        if (ctxByXID.has_value())
            _ActiveTransaction.Context = std::move(ctxByXID.value());
        else
            LOG_DEBUG("No signal context found for xid={}", _ActiveXID);
    }

    auto completedTx = std::move(_ActiveTransaction);
    _ActiveXID = XID_INVALID;
    _ActiveTransaction = {};
    ++_TotalTransactions;

    auto changeCount = completedTx.Changes.size();
    auto duration = std::chrono::duration_cast<Milliseconds>(
        completedTx.CommitTime - completedTx.BeginTime
    );

    LOG_DEBUG("Transaction COMMIT xid={} changes={} lsn={} duration={}ms",
              completedTx.TransactionXID, changeCount,
              FormatLSN(completedTx.CommitLSN), duration.count());

    return completedTx;
}

void TransactionManager::AbortTransaction()
{
    if (_ActiveXID == XID_INVALID)
        return;

    LOG_DEBUG("Transaction ABORTED xid={} (had {} changes)",
              _ActiveXID, _ActiveTransaction.Changes.size());

    _ActiveXID = XID_INVALID;
    _ActiveTransaction = {};
    ++_AbortedTransactions;
}

size_t TransactionManager::ActiveTransactionBytes() const
{
    if (_ActiveXID == XID_INVALID) return 0;
    return _ActiveTransaction.EstimatedMemoryBytes();
}

} // namespace DC
