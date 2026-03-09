
---

> *"To think is to see the possible — to control is to constrain the possible to what is correct — to act is to make the correct real. Engineering is the art of all three, executed in sequence, without skipping steps."*

---

> 🤖 **Built entirely through AI-driven development with GitHub Copilot**  
> No human wrote this code directly — only prompts.

---

# DataCrusher — High-Performance PostgreSQL CDC Engine

**Tootega Pesquisa e Inovação © 1999-2026**

DataCrusher is a high-performance, multi-tenant **Change Data Capture (CDC)** engine built in C++20. It subscribes to PostgreSQL's Write-Ahead Log (WAL) via **Logical Decoding** (`pgoutput` plugin), captures every `INSERT`, `UPDATE`, `DELETE`, and `TRUNCATE` event, and writes an immutable audit journal to a target PostgreSQL database using `COPY FROM STDIN` for maximum throughput — with no ORM, no middleware, and no external message broker.

---

## Table of Contents

1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Component Reference](#component-reference)
4. [Data Flow](#data-flow)
5. [Database Schema](#database-schema)
6. [Configuration Reference](#configuration-reference)
7. [Building](#building)
8. [PostgreSQL Prerequisites](#postgresql-prerequisites)
9. [Running DataCrusher](#running-datacrusher)
10. [Multi-Tenant Model](#multi-tenant-model)
11. [Journal Table Structure](#journal-table-structure)
12. [Session Context Capture](#session-context-capture)
13. [Reconnection & Resilience](#reconnection--resilience)
14. [Performance Characteristics](#performance-characteristics)
15. [Latency Monitoring](#latency-monitoring)
16. [Logging](#logging)
17. [Type Handling](#type-handling)
18. [Supported Platforms](#supported-platforms)

---

## Overview

DataCrusher connects to a centralized **ADM database** and reads a `CDCxConfig` table that contains one row per monitored tenant/database. For each active configuration it spawns an independent `TenantWorker` thread that:

1. Opens a **logical replication connection** to the source database
2. Consumes the **WAL stream** in real time using the native `pgoutput` binary protocol
3. Captures **session context** (user ID, application name) from a signal table on the source database
4. Groups WAL events into **complete transactions** by XID
5. Writes every transaction to the target journal database via **`COPY FROM STDIN`** (batched, tab-delimited)
6. Checkpoints the confirmed **LSN** so restarts resume exactly where they left off

Everything runs in a single process. No Kafka, no Debezium, no Redis. The only external dependency is **libpq** (PostgreSQL client library).

---

## Architecture

### High-Level Multi-Tenant Flow

```
+--------------------------------------------------------------------------+
|                          DataCrusher Process                             |
|                                                                          |
|  +------------------+   polls (30s)   +------------------------------+   |
|  |  WorkerManager   |<----------------|     ADM Database             |   |
|  |  (main thread)   |                 |     CDCxConfig  (tenants)    |   |
|  +--------+---------+                 |     CDCxCheckpoint (LSNs)    |   |
|           |                           +------------------------------+   |
|           | spawns std::jthread per active CDCxConfig row                |
|           v                                                              |
|  +------------------------------------------------------------+          |
|  |  TenantWorker [A]              TenantWorker [B] ...        |          |
|  |                                                            |          |
|  |  ReplicationStream --WAL--> WalMessageParser               |          |
|  |                                   |                        |          |
|  |  SignalTable (query conn)         | Begin/Commit/Change    |          |
|  |       |                          v                         |          |
|  |       +--------------------> TransactionManager            |          |
|  |                                   | complete Transaction   |          |
|  |                                   v                        |          |
|  |                            CopyWriter --COPY--> Journal DB |          |
|  |                                   |                        |          |
|  |                            LatencyMonitor (P50/P95/P99)    |          |
|  +------------------------------------------------------------+          |
+--------------------------------------------------------------------------+
```

### Per-Tenant Connection Model

Each `TenantWorker` maintains **three independent PostgreSQL connections**:

| Connection | Purpose | Protocol |
|------------|---------|----------|
| **Replication** | WAL stream consumption | `replication=database` |
| **Signal** | Session context queries on source DB | Normal query |
| **Writer** | COPY ingestion to journal DB | Normal query + COPY |

These connections are never shared between tenants. The logger singleton is the only shared resource, and it is internally mutex-guarded.

---

## Component Reference

### `Platform.h` — Cross-Platform Abstraction Layer

Defines platform macros, sleep primitives, high-resolution timers, byte-swap utilities, and socket types for both Windows and Linux. Key macros:

| Macro | Effect |
|-------|--------|
| `DC_PLATFORM_WINDOWS` / `DC_PLATFORM_LINUX` | Platform flag |
| `DC_LIKELY(x)` / `DC_UNLIKELY(x)` | Branch prediction hints |
| `DC_FORCEINLINE` | Force-inline (`__forceinline` / `always_inline`) |
| `DC_SleepMS(ms)` | Portable sleep |
| `DC_BSwap64/32/16` | Big-endian byte swap for WAL parsing |
| `DC_HighResTimerNow()` | Nanosecond-resolution timer |

Also defines core typedefs: `LSN` (uint64), `XID` (uint32), `OID` (uint32), `Byte` (uint8), `Microseconds`, `TimePoint`.

---

### `Constants.h` — Configuration Constants & Utilities

Central header containing all compile-time constants, default values, and utility functions. Organized into namespaces:

| Namespace | Contents |
|-----------|---------|
| `DC::AppInfo` | Name, version, copyright strings |
| `DC::Replication` | Slot name, output plugin (`pgoutput`), publication name, protocol version |
| `DC::Connection` | Timeouts, default host/port, SSL mode, application name |
| `DC::Security` | `SecureErase()` — zero-fills password strings from RAM |
| `DC::EnvVar` | All environment variable name constants |
| `DC::Tables` | Journal table name formatting functions |
| `DC::JournalColumns` | Journal column names and COPY header |
| `DC::SignalColumns` | Signal table column names |
| `DC::Operation` | WAL operation type chars (I/U/D/T/B/C/R) |
| `DC::Performance` | Batch size, flush intervals, buffer sizes |
| `DC::Reconnection` | Backoff parameters, jitter factor |
| `DC::Logging` | Log file defaults, rotation limits |
| `DC::PgTypeOID` | PostgreSQL built-in type OIDs |
| `DC::ExitCode` | Process exit codes (`Success`, `FatalError`, `InvalidArguments`) |

---

### `Logger.h/.cpp` — Structured Logger

A thread-safe singleton logger with:

- **6 levels**: `Trace`, `Debug`, `Info`, `Warn`, `Error`, `Fatal`
- **Dual output**: stderr always; optional log file with rotation (100 MB / 10 files)
- **Format**: `[YYYY-MM-DD HH:MM:SS] [LEVEL] message`
- **Template API**: `LOG_INFO("Tenant {} connected", id)` using `std::format`
- **Atomic level check** before string formatting (zero-cost filtered messages)

Convenience macros: `LOG_TRACE`, `LOG_DEBUG`, `LOG_INFO`, `LOG_WARN`, `LOG_ERROR`, `LOG_FATAL`.

---

### `TypeHandlers.h/.cpp` — PostgreSQL Binary Type Converter

A singleton registry mapping PostgreSQL OIDs to handler functions. Converts binary WAL column values to tab-safe text for `COPY FROM STDIN`. Built-in handlers:

| PostgreSQL Types | Handler Strategy |
|-----------------|------------------|
| `bool` | `t` / `f` |
| `int2`, `int4`, `int8` | Big-endian byte-swap to decimal string |
| `float4`, `float8` | Big-endian float/double to decimal string |
| `numeric` | Decoded from PostgreSQL binary numeric format |
| `varchar`, `text`, `bpchar`, `char` | Raw copy with COPY escape sequences |
| `bytea` | Hex-encoded (`\x...`) |
| `uuid` | Raw bytes to canonical `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx` |
| `timestamp`, `timestamptz` | PostgreSQL epoch offset to ISO 8601 UTC |
| `date` | Days from 2000-01-01 to ISO 8601 |
| `time` | Microseconds to `HH:MM:SS.ffffff` |
| `json`, `jsonb` | Raw text passthrough |
| Unknown OIDs | Generic text passthrough |

COPY TEXT escaping handles `\t`, `\n`, `\r`, `\\`, and `\N` (NULL sentinel).

---

### `ReplicationStream.h/.cpp` — WAL Stream Connection

Manages the logical replication connection to the source PostgreSQL database.

**Responsibilities:**
- Opens a `replication=database` connection via `libpq`
- Pre-flight check: verifies `wal_level = logical`
- Creates the logical replication slot (`datacrusher_cdc_slot`) if it does not exist
- Calls `IDENTIFY_SYSTEM` to retrieve the server system ID and current WAL position
- Issues `START_REPLICATION SLOT ... LOGICAL ...` to begin streaming
- Exposes `ReadMessage()` returning `StreamReadResult`:
  - `Data` — raw pgoutput WAL bytes
  - `KeepAlive` — server heartbeat (must be acknowledged)
  - `Timeout` — no data in poll window
  - `EndOfStream` — slot dropped or stream closed
  - `Error` — connection failure
- Sends **standby status updates** (`SendStandbyStatus`) to confirm processed LSNs

Key constants:
- Slot name: `datacrusher_cdc_slot`
- Publication: `datacrusher_publication`
- Protocol version: 4 (streaming mode enabled)
- Output plugin: `pgoutput` (fallback: `wal2json`)

---

### `WalMessageParser.h/.cpp` — pgoutput Binary Protocol Parser

Decodes the raw binary pgoutput WAL stream. The first byte of each message is the type tag:

| Tag | Message | Action |
|-----|---------|--------|
| `B` | Begin | Records transaction XID and commit timestamp |
| `C` | Commit | Records commit LSN |
| `R` | Relation | Caches table OID to schema + column metadata |
| `I` | Insert | Parses `TupleData` for new row values |
| `U` | Update | Parses old key + new full row |
| `D` | Delete | Parses old key values |
| `T` | Truncate | Records truncation event |
| `Y` | Type | Composite type metadata (cached, not journalled) |
| `O` | Origin | Replication origin tag |
| `M` | Message | Generic logical message |
| `S` | Stream | Large transaction streaming header |

**Relation cache**: The parser maintains an OID-keyed map of `RelationInfo` (schema, table name, replica identity, column definitions) used to annotate every `ChangeEvent` with the full table name and column metadata.

**Delta compression** (`ChangeEvent::DeltaJSON()`):
- `INSERT` — all new column values as JSON
- `DELETE` — all old column values as JSON
- `UPDATE` — **only changed columns** (new value only), minimizing journal storage

Big-endian binary reading uses direct pointer arithmetic with `DC_BSwap*` utilities, zero allocations per message.

---

### `TransactionManager.h/.cpp` — Transaction Assembler

Groups `ChangeEvent` objects by their `XID` and assembles a complete `Transaction` at `COMMIT` time.

**`Transaction` structure:**

| Field | Description |
|-------|-------------|
| `TransactionXID` | uint32 transaction identifier |
| `BeginLSN` | LSN of BEGIN message |
| `CommitLSN` | LSN of COMMIT message |
| `CommitTimestamp` | PostgreSQL commit timestamp (microseconds since 2000-01-01) |
| `Changes` | Ordered list of ChangeEvent (INSERT/UPDATE/DELETE/TRUNCATE) |
| `Context` | SessionContext (UserID, AppName) from signal table |
| `BeginTime` | When DataCrusher received BEGIN |
| `CommitTime` | When DataCrusher received COMMIT |

At commit, `TransactionManager` calls `SignalTable::QueryContextByXID()` to attach the user session context. Transactions with no changes (DDL-only or empty) are discarded.

---

### `CopyWriter.h/.cpp` — Bulk Journal Ingestion

Connects to the target/journal database and writes completed transactions via `COPY FROM STDIN` (tab-delimited text), bypassing PostgreSQL's SQL parser for maximum write throughput.

**Startup bootstrap (`Bootstrap()`):**
1. Creates the journal schema (e.g., `CDCx`) if absent
2. Creates the `Checkpoint` table for LSN persistence

**Per-table setup (`EnsureJournalTable()`):**
Creates a journal table `"CDCx"."CDCx{SourceTable}"` with the standard 9-column schema. Journals each table separately so the audit log can be queried per entity. Results are cached to avoid redundant DDL checks on repeat calls.

**View generation (`EnsureTableView()`):**
Creates a convenience view `"CDCx"."SourceTable"` that expands the JSONB `Data` column into typed columns mirroring the source schema, with `JNLx`-prefixed audit columns. Views are recreated on every startup to reflect schema changes.

**Write path (`WriteTransaction()`):**
1. Groups changes by table name within the transaction
2. Opens a PostgreSQL `BEGIN`
3. For each table group:
   - Calls `EnsureJournalTable()` (lazy creation for tables first seen at runtime)
   - Issues `COPY "schema"."table" ("XID","LSN","Operation","Data","UserID","Origin","CommittedAt","CapturedAt") FROM STDIN`
   - Formats each row as tab-separated text and sends via `PQputCopyData` + `PQputCopyEnd`
4. Issues `COMMIT`
5. Saves checkpoint LSN

**Statistics** exposed via `WriterStats`: total rows, bytes, batches, errors, last flush duration.

---

### `SignalTable.h/.cpp` — Session Context Capture

Maintains a **non-replication query connection** to the source database for two purposes:

**1. Bootstrap / setup operations:**
- `Bootstrap()` — creates the `public."{prefix}Session"` signal table on the source
- `EnsurePublication()` — creates `datacrusher_publication FOR ALL TABLES` if missing
- `EnsureReplicaIdentityFull()` — sets `REPLICA IDENTITY FULL` on every user table (required for full old-row capture in UPDATE/DELETE)
- `GetPublicationTables()` — lists all tables in the publication for journal pre-creation
- `GetTableColumns()` — reads `information_schema.columns` to build typed views on the target

**2. Runtime context queries:**
- `QueryContext(backendPID)` — queries the signal table by the replication backend PID
- `QueryContextByXID(xid)` — queries by transaction XID

The **signal table** (`public."CDCxSession"`) is written by the application layer before each transaction, recording `UserID` and `AppName`. DataCrusher reads this at COMMIT time to attach identity context to every journal entry.

---

### `ReconnectionManager.h/.cpp` — Exponential Backoff Reconnection

Implements the reconnection policy for both source and target connections.

**Algorithm:**
```
delay = BaseDelayMS x BackoffMultiplier^attempt x (1 +/- JitterFactor)
delay = clamp(delay, BaseDelayMS, MaxDelayMS)
```

**Default policy:**

| Parameter | Default |
|-----------|---------|
| `BaseDelayMS` | 500 ms |
| `MaxDelayMS` | 60,000 ms |
| `BackoffMultiplier` | 2.0x |
| `JitterFactor` | ±25% random jitter |
| `MaxRetries` | -1 (infinite) |
| `StabilityPeriodSec` | 60 s |

After a successful reconnection that remains stable for `StabilityPeriodSec`, the retry counter resets to zero. Jitter is generated using a `std::mt19937` Mersenne Twister.

---

### `LatencyMonitor.h/.cpp` — Pipeline Latency Histogram

Each `TenantWorker` owns a `LatencyMonitor` instance (not shared). Records two latency dimensions per transaction:

| Metric | Measured From → To |
|--------|-------------------|
| **Capture latency** | PostgreSQL commit timestamp → DataCrusher receipt |
| **Write latency** | CopyWriter COPY start → COPY end |
| **End-to-end latency** | PostgreSQL commit → journal write complete |

Statistics exposed in a `LatencySnapshot`:
- `Min`, `Max`, `Avg` per dimension
- **P50, P95, P99** capture latency via histogram (8 buckets: 0–100 µs, 100–500 µs, 0.5–1 ms, 1–5 ms, 5–10 ms, 10–50 ms, 50–100 ms, 100+ ms)

Reported to the log every `LatencyReportIntervalSec` seconds (default: 60 s).

---

### `TenantWorkerConfig.h` — Per-Tenant Configuration

Aggregates all configuration for one tenant worker:

| Field | Type | Description |
|-------|------|-------------|
| `TenantIdentifier` | UUID string | Primary key from `CDCxConfig.ID` |
| `TenantName` | string | Human-readable label for logs |
| `TablePrefix` | string | Schema name + signal table prefix (e.g. `CDCx`) |
| `UntrackedTables` | `unordered_set<string>` | Tables excluded from journalling |
| `Source` | `ReplicationConfig` | Source DB host, port, credentials, slot, publication |
| `Journal` | `WriterConfig` | Target DB host, port, credentials, schema, batch size |
| `Reconnection` | `ReconnectionPolicy` | Per-tenant backoff settings |
| `BootstrapJournal` | bool | Auto-create journal schema on first start |
| `BootstrapSignal` | bool | Auto-create signal table on source DB |

Also defines:
- `WorkerState` enum: `Idle`, `Connecting`, `Streaming`, `Reconnecting`, `Paused`, `Stopping`, `Stopped`, `Failed`
- `WorkerHealthSnapshot` — lock-free snapshot for manager health checks
- `ManagerConfig` — ADM connection + global defaults for all workers

---

### `TenantWorker.h/.cpp` — Per-Tenant CDC Pipeline Thread

The self-contained CDC pipeline for one tenant, running on a `std::jthread`.

**Worker lifecycle phases:**

```
IDLE -> CONNECTING -> STREAMING -> (RECONNECTING ->) STOPPING -> STOPPED
                                                               -> FAILED
```

**Phase 1 — Connect** (`ConnectAll()`):
- Opens replication connection (source)
- Opens signal/query connection (source)
- Opens writer connection (journal)

**Phase 2 — Bootstrap:**
- Creates journal schema and checkpoint table on target
- Creates signal table on source (if `BootstrapSignal = true`)
- Calls `EnsureReplicaIdentityFull()` on all source tables
- Queries publication tables and pre-creates journal tables
- Fetches column metadata and creates/refreshes typed views on target

**Phase 3 — Start Replication** (`StartReplication()`):
- Reads checkpoint LSN from journal DB (resume position)
- Falls back to LSN file or `0/0` (full stream from the beginning of the slot)
- Issues `START_REPLICATION` from the checkpoint LSN

**Phase 4 — Main Loop** (`MainLoop()`):
- Calls `ReplicationStream::ReadMessage()` in a tight loop
- Dispatches each WAL message type to `WalMessageParser`
- On `Begin`: starts a new transaction in `TransactionManager`
- On `Relation`: updates the parser's relation cache
- On `Insert`/`Update`/`Delete`/`Truncate`: adds `ChangeEvent` to the active transaction
- On `Commit`: finalizes transaction, attaches session context, submits to `CopyWriter`, updates confirmed LSN, sends standby status acknowledgment
- On `KeepAlive`: sends standby status if reply is requested
- Periodically reports latency and statistics

**Phase 5 — Cleanup:**
- Disconnects all connections
- Saves final statistics

**Lock-free health interface** (callable from manager thread):
`State()`, `ConfirmedLSN()`, `TotalTransactions()`, `TotalChanges()`, `TotalErrors()`, `ReconnectionCount()`, `TakeHealthSnapshot()`

---

### `WorkerManager.h/.cpp` — Multi-Tenant Orchestrator

Runs on the **main thread** (blocking `Run()` loop). Responsibilities:

1. **ADM connection**: Connects to the centralized ADM PostgreSQL database
2. **Tenant discovery** (`RefreshTenantList()`): Queries `CDCxConfig WHERE "State" = 1` every `TenantPollIntervalSec` (default: 30 s)
3. **Reconciliation**: Starts workers for new configurations, stops workers for removed/deactivated rows
4. **Health monitoring** (`CheckWorkerHealth()`): Every `HealthCheckIntervalSec` (default: 10 s) reads lock-free health snapshots from all workers and logs their status
5. **Worker reaping** (`ReapFinishedWorkers()`): Removes completed (Stopped/Failed) workers and logs final statistics
6. **Graceful shutdown** (`RequestShutdown()`): Signals all workers to stop, waits for them to finish, saves final LSN checkpoints to ADM
7. **LSN persistence on shutdown** (`ConfirmAllLSNToADM()`): Upserts last confirmed LSN per tenant into `CDCxCheckpoint` in the ADM database

**Signal handling**: `SIGINT`, `SIGTERM` (and `SIGHUP` on Linux) are caught and route to `RequestShutdown()`.

**Worker isolation**: Each `TenantWorker` is stored as a `unique_ptr` in an `unordered_map<TenantID, unique_ptr<TenantWorker>>`. Workers share nothing except the logger.

---

### `DataCrusherEngine.h/.cpp` — Top-Level Orchestrator

Thin wrapper over `WorkerManager`. Responsibilities:
- **Environment variable resolution** (`ResolveEnvOverrides()`): Maps `DC_*` env vars to `EngineConfig` fields (lowest priority, overridden by CLI)
- **Logger initialization**
- **Type handler initialization** (`TypeHandlers::Instance().InitializeBuiltins()`)
- **Signal handler installation** for the process
- **Delegation** to `WorkerManager::Initialize()` and `WorkerManager::Run()`

---

### `Main.cpp` — Entry Point

Three-layer configuration assembly (priority: CLI > env > defaults):
1. Hardcoded defaults in `EngineConfig`
2. `ResolveEnvOverrides()` — env vars override defaults
3. `ParseArgs()` — CLI flags override everything

Validates that ADM credentials are present, then creates and runs `DataCrusherEngine`.

---

## Data Flow

```
PostgreSQL WAL Stream (pgoutput binary)
        |
        v
ReplicationStream::ReadMessage()
        |
        v  (raw bytes)
WalMessageParser::Parse()
        |
        +--[B] Begin --------> TransactionManager::BeginTransaction()
        +--[R] Relation ------> parser relation cache update
        +--[I/U/D/T] Change --> TransactionManager::AddChange(ChangeEvent)
        |                               |
        |                       ChangeEvent::DeltaJSON()
        |                       (INSERT: all new; DELETE: all old; UPDATE: diff only)
        |
        +--[C] Commit ----------> SignalTable::QueryContextByXID()
                                          |
                                          v  SessionContext (UserID, AppName)
                                  TransactionManager::CommitTransaction()
                                          |
                                          v  complete Transaction
                                  CopyWriter::WriteTransaction()
                                          |
                                          v
                                  COPY "CDCx"."CDCxTable" FROM STDIN
                                          |
                                          v
                                  CopyWriter::SaveCheckpointLSN()
                                  ReplicationStream::SendStandbyStatus()
```

---

## Database Schema

### ADM Database

The centralized administration database where DataCrusher reads its tenant list.

#### `CDCxConfig` — Tenant Registry

| Column | Type | Description |
|--------|------|-------------|
| `ID` | UUID | Tenant identifier (unique per CDC configuration) |
| `Name` | varchar | Human-readable tenant name |
| `DBHost` | varchar | Source database host |
| `DBHostPort` | int | Source database port |
| `DBName` | varchar | Source database name |
| `DBUser` | varchar | Source database replication user |
| `DBPassword` | varchar | Source database password |
| `DBCDCHost` | varchar | Journal (target) database host |
| `DBCDCHostPort` | int | Journal database port |
| `DBCDCName` | varchar | Journal database name |
| `DBCDCUser` | varchar | Journal database writer user |
| `DBCDCPassword` | varchar | Journal database password |
| `TablePrefix` | varchar | Schema prefix (e.g. `CDCx`) — used for journal schema and signal table name |
| `UntrackedTables` | varchar | Semicolon-delimited list of table names to exclude from journalling |
| `State` | int | `1` = Active (polled by DataCrusher), other values = inactive |

#### `CDCxCheckpoint` — LSN Persistence (created automatically)

| Column | Type | Description |
|--------|------|-------------|
| `CDCxConfigID` | UUID (PK) | References `CDCxConfig.ID` |
| `SlotName` | text | Replication slot name |
| `ConfirmedLSN` | text | Last safely written LSN (e.g. `0/1A3F00`) |
| `UpdatedAt` | timestamptz | When the checkpoint was last saved |

---

### Source Database (per tenant)

#### Logical Replication Slot

```
Name:   datacrusher_cdc_slot   (configurable)
Plugin: pgoutput
```

Created automatically on first connection if it does not exist.

#### Publication

```
Name: datacrusher_publication FOR ALL TABLES
```

Created automatically if it does not exist (requires `CREATE PUBLICATION` privilege).

#### `public."{prefix}Session"` — Signal Table (e.g. `public."CDCxSession"`)

Created automatically by DataCrusher on bootstrap. Written by the application before each transaction to provide identity context:

| Column | Type | Description |
|--------|------|-------------|
| `SessionID` | text (PK) | Application session identifier |
| `BackendPID` | int | PostgreSQL backend PID |
| `UserID` | text | Application user identifier |
| `AppName` | text | Application name |
| `Moment` | timestamptz | When the session context was written |

---

### Journal (Target) Database (per tenant)

#### `"{schema}"."{schema}{SourceTable}"` — Journal Tables

One journal table per tracked source table. Example: source table `DOCxContato` with prefix `CDCx` creates `"CDCx"."CDCxDOCxContato"`.

| Column | Type | Description |
|--------|------|-------------|
| `JournalID` | BIGSERIAL (PK) | Auto-increment journal entry ID |
| `XID` | bigint | PostgreSQL transaction ID |
| `LSN` | text | Log Sequence Number of the change event |
| `Operation` | char(1) | `I` (INSERT), `U` (UPDATE), `D` (DELETE), `T` (TRUNCATE) |
| `Data` | jsonb | Delta payload (see Delta Compression below) |
| `UserID` | text | User identity from signal table |
| `Origin` | text | Application name from signal table |
| `CommittedAt` | timestamptz | PostgreSQL commit timestamp |
| `CapturedAt` | timestamptz | DataCrusher ingestion timestamp |

#### `"{schema}"."{SourceTable}"` — Typed Views

For every source table, DataCrusher creates a PostgreSQL view that expands the JSONB `Data` column into typed columns matching the source schema. Journal audit columns are prefixed with `JNLx`. Views are recreated on every startup to track schema changes.

#### `"{schema}"."Checkpoint"` — Local LSN Persistence

| Column | Type | Description |
|--------|------|-------------|
| `SlotName` | text (PK) | Replication slot name |
| `ConfirmedLSN` | text | Last safely written LSN |
| `UpdatedAt` | timestamptz | Checkpoint timestamp |

---

## Configuration Reference

### Priority Order (highest wins)

```
CLI arguments  >  Environment variables  >  Hardcoded defaults
```

### ADM Database Options

| CLI Flag | Environment Variable | Default | Description |
|----------|---------------------|---------|-------------|
| `--adm-host` | `DC_ADM_HOST` | `127.0.0.1` | ADM PostgreSQL host |
| `--adm-port` | `DC_ADM_PORT` | `5432` | ADM PostgreSQL port |
| `--adm-db` | `DC_ADM_DBNAME` | `TokenGuard` | ADM database name |
| `--adm-user` | `DC_ADM_USER` | *(required)* | ADM user |
| `--adm-password` | `DC_ADM_PASSWORD` | *(required)* | ADM password |

### Worker Options

| CLI Flag | Environment Variable | Default | Description |
|----------|---------------------|---------|-------------|
| `--max-workers` | `DC_MAX_WORKERS` | `64` | Maximum concurrent tenant workers |
| `--tenant-poll` | `DC_TENANT_POLL_INTERVAL` | `30` | Tenant list poll interval (seconds) |
| `--health-check` | `DC_HEALTH_CHECK_INTERVAL` | `10` | Worker health check interval (seconds) |

### Reconnection Options

| CLI Flag | Environment Variable | Default | Description |
|----------|---------------------|---------|-------------|
| `--reconnect-base` | `DC_RECONNECT_BASE_MS` | `500` | Initial backoff delay (ms) |
| `--reconnect-max` | `DC_RECONNECT_MAX_MS` | `60000` | Maximum backoff delay (ms) |
| `--reconnect-retries` | `DC_RECONNECT_MAX_RETRIES` | `-1` | Max retries (-1 = unlimited) |

### Logging Options

| CLI Flag | Environment Variable | Default | Description |
|----------|---------------------|---------|-------------|
| `--log-level` | `DC_LOG_LEVEL` | `info` | `trace`\|`debug`\|`info`\|`warn`\|`error`\|`fatal` |
| `--log-file` | *(CLI only)* | *(stderr)* | Log file path (stderr always active) |

### General Flags

| Flag | Description |
|------|-------------|
| `--version` / `-v` | Print version and exit |
| `--help` / `-h` | Print usage and exit |

---

## Building

### Prerequisites

> **DataCrusher produces a fully self-contained static binary by default.** The target machine requires no runtime libraries, no PostgreSQL installation, and no additional dependencies.

The following are **build-time** requirements only:

#### Compiler & Toolchain

| Component | Minimum | Reason |
|-----------|---------|--------|
| **MSVC** | 19.29+ (Visual Studio 2022 v17.0+) | Windows builds; C++ workload with CMake integration must be installed |
| **GCC** | 12.0+ | Linux builds; full C++20 support including `std::jthread`, `std::format`, and concepts |
| **Clang** | 14.0+ | Alternative Linux compiler; `libc++` or `libstdc++` both supported |
| **CMake** | 3.20 | Build system generator; CMakePresets.json schema v6 for IDE preset integration |
| **Ninja** | 1.11+ | Optional but recommended; used as the preferred single-config generator on Linux |

#### Windows Build Automation

| Component | Minimum | Reason |
|-----------|---------|--------|
| **PowerShell** | 7.2+ | `Build-DataCrusher.ps1` — orchestrates the full multi-platform build pipeline |
| **Git** | 2.30+ | Required by the build script to clone and bootstrap vcpkg for static libpq |
| **Docker Desktop** | 24+ | Linux cross-compilation via GCC containers; ARM64 via QEMU emulation. Must be configured for **Linux containers**. |

#### Static libpq — Default Mode (Zero Runtime Dependencies)

| Platform | How it is obtained | SSL | GSSAPI | LDAP |
|----------|--------------------|-----|--------|------|
| **Windows** | vcpkg auto-cloned and bootstrapped by `Build-DataCrusher.ps1` | ✗ | ✗ | ✗ |
| **Linux** | PostgreSQL source compiled inside Docker (`--without-openssl --without-gssapi --without-ldap`) | ✗ | ✗ | ✗ |

`Build-DataCrusher.ps1` handles the entire static libpq setup automatically — no manual steps required.

#### Dynamic libpq — Optional (Development Only)

| Platform | Install Command |
|----------|-----------------|
| Ubuntu / Debian | `sudo apt install libpq-dev` |
| Fedora / RHEL / Rocky | `sudo dnf install libpq-devel` |
| Windows | Install PostgreSQL 14+ from postgresql.org; pass `-DPOSTGRESQL_ROOT="C:/Program Files/PostgreSQL/17"` to CMake |

> **Dynamic builds are not recommended for production.** They require `libpq.dll` / `libpq.so` and its SSL + crypto dependencies on every target machine.

#### PostgreSQL Server — Source Database Requirements

| Setting | Required Value | Location |
|---------|---------------|----------|
| `wal_level` | `logical` | `postgresql.conf` |
| `max_replication_slots` | ≥ 1 per monitored database | `postgresql.conf` |
| `max_wal_senders` | ≥ 1 per monitored database | `postgresql.conf` |
| Replication role | `REPLICATION` attribute + `SELECT` on all monitored tables | Database role |
| Publication | Named publication on each source database | Auto-created by DataCrusher if the role has `CREATE` privilege |

---

### Windows — Using the Build Script (Recommended)

`Build-DataCrusher.ps1` automates PostgreSQL detection, vcpkg bootstrap (for static builds), and multi-platform compilation:

```powershell
# Build all targets (win-x64, linux-x64, linux-arm64)
.\Build-DataCrusher.ps1

# Build only Windows x64
.\Build-DataCrusher.ps1 -Targets win-x64

# Build Windows + Linux, clean first
.\Build-DataCrusher.ps1 -Targets win-x64,linux-x64 -Clean

# Specify PostgreSQL path
.\Build-DataCrusher.ps1 -Targets win-x64 -PostgreSqlRoot "C:\Program Files\PostgreSQL\17"
```

Output binaries are placed in `./Deploy/` with platform tags:
- `DataCrusher.win-x64.exe`
- `DataCrusher.win-arm64.exe`
- `DataCrusher.linux-x64`
- `DataCrusher.linux-arm64`

---

### Windows — Manual CMake

```powershell
# Dynamic linking against installed PostgreSQL (development)
cmake -B build/windows-x64-release `
      -G "Visual Studio 17 2022" -A x64 `
      -DPOSTGRESQL_ROOT="C:/Program Files/PostgreSQL/17"

cmake --build build/windows-x64-release --config Release

# Using CMake presets
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
```

**Static linking** (self-contained, no DLL dependencies needed on target machine):
```powershell
cmake -B build/windows-x64-release `
      -G "Visual Studio 17 2022" -A x64 `
      -DDC_STATIC_LINK=ON `
      -DPOSTGRESQL_STATIC_ROOT="path/to/static-libpq"

cmake --build build/windows-x64-release --config Release
```

---

### Linux — CMake

```bash
# Install libpq development headers
sudo apt install libpq-dev        # Debian / Ubuntu
sudo dnf install libpq-devel      # Fedora / RHEL / Rocky

# Configure and build
cmake -B build/linux-x64-release -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux-x64-release -j$(nproc)

# Using CMake presets
cmake --preset linux-x64-release
cmake --build --preset linux-x64-release
```

**Static linked binary** (for deployment without libpq installed):
```bash
cmake -B build/linux-x64-release \
      -DCMAKE_BUILD_TYPE=Release \
      -DDC_STATIC_LINK=ON \
      -DPOSTGRESQL_STATIC_ROOT=/opt/pgsql

cmake --build build/linux-x64-release -j$(nproc)
```

---

### CMake Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `DC_ENABLE_ASAN` | `OFF` | AddressSanitizer — enable for debug builds to detect memory errors |
| `DC_ENABLE_TSAN` | `OFF` | ThreadSanitizer — enable for debug builds to detect data races |
| `DC_BUILD_TESTS` | `OFF` | Build unit tests |
| `DC_STATIC_RUNTIME` | **`ON`** | Use static CRT on Windows (`/MT`) — binary runs without MSVC redistributables |
| `DC_STATIC_LINK` | **`ON`** | Statically link all dependencies — produces a fully self-contained binary with no runtime library requirements |
| `DC_DEPLOY_DIR` | `../Deploy` | Output directory for compiled binaries; overridden automatically by `Build-DataCrusher.ps1` |

---

### Available CMake Presets

| Preset | Platform | Config |
|--------|---------|--------|
| `windows-x64-debug` | Windows | Debug |
| `windows-x64-release` | Windows | Release |
| `linux-x64-debug` | Linux | Debug + ASan |
| `linux-x64-release` | Linux | Release |
| `linux-arm64-release` | Linux ARM64 | Release |

---

## PostgreSQL Prerequisites

### Source Database

#### 1. Enable Logical Replication

In `postgresql.conf`:
```ini
wal_level = logical
max_replication_slots = 4    # minimum 1 per tenant
max_wal_senders = 4          # minimum 1 per tenant
```

Restart PostgreSQL after changing `wal_level`.

#### 2. Create Replication User

```sql
CREATE ROLE datacrusher_repl WITH LOGIN REPLICATION PASSWORD 'strong_password';

-- Grant access to the signal table schema
GRANT USAGE ON SCHEMA public TO datacrusher_repl;

-- Grant SELECT on the signal table (auto-created by DataCrusher)
GRANT SELECT ON public."CDCxSession" TO datacrusher_repl;
```

#### 3. Allow Replication Connections

In `pg_hba.conf`:
```
host  replication  datacrusher_repl  10.0.0.5/32  scram-sha-256
```

#### 4. Pre-Create Publication (optional — DataCrusher auto-creates)

```sql
CREATE PUBLICATION datacrusher_publication FOR ALL TABLES;
```

#### 5. Set Replica Identity (optional — DataCrusher auto-configures)

Required for capturing old row values in UPDATE and DELETE:
```sql
-- DataCrusher runs this automatically at startup for all user tables:
ALTER TABLE your_table REPLICA IDENTITY FULL;
```

Without `REPLICA IDENTITY FULL`, `DELETE` and `UPDATE` events in the WAL will not include previous column values.

---

### Journal (Target) Database

#### Create Writer User

```sql
CREATE ROLE datacrusher_writer WITH LOGIN PASSWORD 'strong_password';

-- Grant schema creation (DataCrusher creates the journal schema automatically)
GRANT CREATE ON DATABASE journal_db TO datacrusher_writer;
```

---

### ADM Database

#### 1. Create the `CDCxConfig` Table

```sql
CREATE TABLE "CDCxConfig" (
    "ID"              UUID         PRIMARY KEY DEFAULT gen_random_uuid(),
    "Name"            VARCHAR(200) NOT NULL,
    "DBHost"          VARCHAR(200) NOT NULL,
    "DBHostPort"      INT          NOT NULL DEFAULT 5432,
    "DBName"          VARCHAR(200) NOT NULL,
    "DBUser"          VARCHAR(200) NOT NULL,
    "DBPassword"      VARCHAR(500) NOT NULL,
    "DBCDCHost"       VARCHAR(200) NOT NULL,
    "DBCDCHostPort"   INT          NOT NULL DEFAULT 5432,
    "DBCDCName"       VARCHAR(200) NOT NULL,
    "DBCDCUser"       VARCHAR(200) NOT NULL,
    "DBCDCPassword"   VARCHAR(500) NOT NULL,
    "TablePrefix"     VARCHAR(50)  NOT NULL DEFAULT 'CDCx',
    "UntrackedTables" VARCHAR(2000)         DEFAULT '',
    "State"           INT          NOT NULL DEFAULT 1
);
```

#### 2. Register a Tenant

```sql
INSERT INTO "CDCxConfig" (
    "Name",
    "DBHost", "DBHostPort", "DBName", "DBUser", "DBPassword",
    "DBCDCHost", "DBCDCHostPort", "DBCDCName", "DBCDCUser", "DBCDCPassword",
    "TablePrefix", "UntrackedTables", "State"
) VALUES (
    'Tenant Alpha',
    '10.0.1.100', 5432, 'alpha_db', 'datacrusher_repl', 'repl_password',
    '10.0.2.100', 5432, 'alpha_journal', 'datacrusher_writer', 'writer_password',
    'CDCx',
    'SYSxSession;SYSxMetrics',   -- tables to exclude from journalling
    1                             -- 1 = active
);
```

#### 3. Grant ADM Access

```sql
CREATE ROLE datacrusher_adm WITH LOGIN PASSWORD 'adm_password';
GRANT SELECT ON "CDCxConfig" TO datacrusher_adm;
GRANT SELECT, INSERT, UPDATE ON "CDCxCheckpoint" TO datacrusher_adm;
-- CDCxCheckpoint is created automatically by DataCrusher on first run
```

---

## Running DataCrusher

### Basic Invocation

```bash
# Using environment variables (recommended for passwords)
export DC_ADM_HOST=10.0.0.10
export DC_ADM_USER=datacrusher_adm
export DC_ADM_PASSWORD=adm_password

./DataCrusher

# Full CLI (override all defaults)
./DataCrusher \
  --adm-host 10.0.0.10 \
  --adm-user datacrusher_adm \
  --adm-password adm_password \
  --adm-db TokenGuard \
  --max-workers 32 \
  --tenant-poll 60 \
  --log-level info \
  --log-file /var/log/datacrusher.log
```

### Docker / Container

```dockerfile
FROM debian:bookworm-slim
COPY DataCrusher.linux-x64 /usr/local/bin/DataCrusher
RUN chmod +x /usr/local/bin/DataCrusher

ENV DC_ADM_HOST=postgres-adm
ENV DC_ADM_USER=datacrusher_adm
ENV DC_ADM_PASSWORD=secret
ENV DC_ADM_DBNAME=TokenGuard
ENV DC_LOG_LEVEL=info
ENV DC_MAX_WORKERS=64

CMD ["/usr/local/bin/DataCrusher"]
```

### Graceful Shutdown

Send `SIGINT` (Ctrl+C) or `SIGTERM`. DataCrusher will:
1. Signal all tenant workers to stop
2. Wait for each worker to finish its current transaction
3. Flush all pending COPY data
4. Save final LSN checkpoints to the ADM database
5. Exit with code `0`

---

## Multi-Tenant Model

DataCrusher monitors **multiple databases simultaneously** from a single process:

```
ADM Database
+-- CDCxConfig (State=1)
    +-- row [Tenant A] -> TenantWorker A (thread)
    |     Source: pg-host-a / app_db_a -> Journal: pg-journal-a / journal_a
    +-- row [Tenant B] -> TenantWorker B (thread)
    |     Source: pg-host-b / app_db_b -> Journal: pg-journal-b / journal_b
    +-- row [Tenant C] -> TenantWorker C (thread)
          Source: pg-host-c / app_db_c -> Journal: pg-journal-c / journal_c
```

**Dynamic tenant management**: The manager polls `CDCxConfig` every 30 seconds (configurable). Adding a new row with `State=1` starts a new worker automatically. Setting `State` to any value other than `1` (or deleting the row) gracefully stops that worker.

**Worker isolation**: Each worker has its own connections, WAL parser state, transaction buffer, reconnection manager, and latency monitor. A failure or reconnection in one tenant does not affect others.

**`UntrackedTables`**: A semicolon-delimited list of source table names to exclude from journalling (e.g., `SYSxSession;SYSxMetrics;SYSxAuditLog`). These tables remain in the publication — they are simply filtered out after WAL parsing.

---

## Journal Table Structure

Every change event is stored in a journal table:

```sql
CREATE TABLE "CDCx"."CDCxDOCxContato" (
    "JournalID"   BIGSERIAL    PRIMARY KEY,
    "XID"         BIGINT       NOT NULL,
    "LSN"         TEXT         NOT NULL,
    "Operation"   CHAR(1)      NOT NULL,  -- I, U, D, T
    "Data"        JSONB        NOT NULL,
    "UserID"      TEXT,
    "Origin"      TEXT,
    "CommittedAt" TIMESTAMPTZ  NOT NULL,
    "CapturedAt"  TIMESTAMPTZ  NOT NULL DEFAULT NOW()
);
```

### Delta Compression

The `Data` JSONB column stores a **minimal delta**:

| Operation | `Data` contents |
|-----------|----------------|
| `INSERT` | All new column values |
| `DELETE` | All old column values (requires `REPLICA IDENTITY FULL`) |
| `UPDATE` | Only the **changed columns** (new value only) |
| `TRUNCATE` | Empty object `{}` |

For UPDATE-heavy workloads where only 1–3 fields change per row, this significantly reduces journal storage compared to storing full row images.

---

## Session Context Capture

To associate journal entries with the application user who made the change (not just the database role), the application writes to the signal table before each transaction:

```sql
-- Write before BEGIN or within the transaction
INSERT INTO public."CDCxSession"
    ("SessionID", "BackendPID", "UserID", "AppName", "Moment")
VALUES (
    gen_random_uuid()::text,
    pg_backend_pid(),
    'user-uuid-here',         -- application user ID
    'MyApp/2.0',              -- application name
    NOW()
)
ON CONFLICT ("SessionID") DO UPDATE
    SET "UserID"  = EXCLUDED."UserID",
        "AppName" = EXCLUDED."AppName",
        "Moment"  = EXCLUDED."Moment";
```

DataCrusher queries this table at every COMMIT and attaches `UserID` and `AppName` to all journal rows of that transaction. If no signal row is found, `UserID` and `Origin` are stored as empty strings.

---

## Reconnection & Resilience

### Automatic Reconnection

When either the source (replication) or target (writer) connection is lost, `ReconnectionManager` retries with exponential backoff:

```
Attempt 1: wait 500 ms (+/- 25% jitter)
Attempt 2: wait 1,000 ms
Attempt 3: wait 2,000 ms
Attempt 4: wait 4,000 ms
...
Attempt N: wait min(500 x 2^N, 60,000) ms +/- 25% jitter
```

The reconnection counter resets after 60 seconds of stable operation, so transient failures do not permanently increase reconnection delays.

### LSN Checkpointing

DataCrusher writes the confirmed LSN to two places:
1. **Journal database** (`"CDCx"."Checkpoint"`) — updated after every successful `WriteTransaction()`
2. **ADM database** (`"CDCxCheckpoint"`) — updated on graceful shutdown

On restart, DataCrusher reads the checkpoint LSN and resumes streaming from exactly that position. This ensures **no data loss** and **no duplicate entries** through restarts and reconnections.

### Standby Status Updates

DataCrusher sends PostgreSQL `standby_status_update` messages every 10 seconds confirming the last processed LSN. This:
- Prevents WAL accumulation on the source server (controls disk usage)
- Keeps the replication connection alive (avoids `wal_sender_timeout`)
- Allows the source to reclaim WAL segments before the confirmed LSN

---

## Performance Characteristics

| Aspect | Implementation |
|--------|---------------|
| **WAL parsing** | Direct big-endian reads via pointer arithmetic (`DC_BSwap*`), zero allocations per message |
| **COPY ingestion** | `PQputCopyData` bypasses SQL parsing; default batch 1,000 rows (up to 50,000) |
| **Transaction buffer** | Pre-allocated 64 KB initial, grows to 16 MB max before forced flush |
| **COPY row buffer** | Pre-allocated 512 bytes × batch size at writer construction |
| **Type conversion** | Function pointer dispatch (no virtual calls) from OID lookup table |
| **Thread model** | One `std::jthread` per tenant; zero shared mutable state except the logger |
| **Batch flush trigger** | Row count threshold OR time interval (default: 500 ms), whichever comes first |
| **Poll interval** | 1 ms sleep when no WAL data is available (prevents busy-wait) |

### Default Tuning Parameters

| Parameter | Default | Environment Variable |
|-----------|---------|---------------------|
| Batch size | 1,000 rows | `DC_BATCH_SIZE` |
| Flush interval | 500 ms | `DC_FLUSH_INTERVAL_MS` |
| Transaction buffer | 64 KB initial / 16 MB max | (constant) |
| Standby status interval | 10 s | (constant) |
| WAL poll interval | 1 ms | (constant) |

---

## Latency Monitoring

DataCrusher tracks end-to-end latency per tenant and logs a report every 60 seconds:

```
[INFO ] [TenantA] Latency report -- samples: 15,420
        Capture P50:  12 us  P95:  45 us  P99: 130 us
        Write   avg: 980 us  min: 200 us  max: 3,200 us
        E2E     avg: 1,100 us  min: 250 us  max: 3,500 us
```

| Metric | Definition |
|--------|-----------|
| **Capture latency** | PostgreSQL commit timestamp (from WAL) to DataCrusher receipt |
| **Write latency** | COPY stream start to COPY end (journal DB write time) |
| **End-to-end latency** | PostgreSQL commit to journal write complete |

---

## Logging

DataCrusher writes structured log lines to stderr (always) and optionally to a rotating log file:

```
[2026-03-08 14:22:01] [INFO ] [TenantA] CDC pipeline starting...
[2026-03-08 14:22:01] [INFO ] [TenantA] Source: 10.0.1.100:5432/alpha_db
[2026-03-08 14:22:01] [INFO ] [TenantA] Journal: 10.0.2.100:5432/alpha_journal
[2026-03-08 14:22:01] [INFO ] [TenantA] Slot: datacrusher_cdc_slot (plugin: pgoutput)
[2026-03-08 14:22:02] [INFO ] [TenantA] Entering main CDC loop
[2026-03-08 14:22:15] [DEBUG] [TenantA] TX xid=583921 lsn=0/2A1F00 changes=3 user=usr-001
```

**Log rotation**: Maximum 100 MB per file, keeps 10 rotated files.

**Log levels** (from most to least verbose):
`trace` > `debug` > `info` > `warn` > `error` > `fatal` > `off`

---

## Type Handling

DataCrusher includes native binary deserializers for all common PostgreSQL types:

| Category | Types |
|----------|-------|
| Integer | `bool`, `int2`, `int4`, `int8` |
| Float | `float4`, `float8`, `numeric` |
| Text | `varchar`, `text`, `char`, `bpchar` |
| Binary | `bytea` (hex-encoded) |
| UUID | `uuid` (canonical 8-4-4-4-12 format) |
| Date/Time | `timestamp`, `timestamptz`, `date`, `time`, `timetz`, `interval` |
| Document | `json`, `jsonb` |
| Network | `inet`, `cidr`, `macaddr` |
| Bit | `bit`, `varbit` |
| OID | Generic OID passthrough |
| Unknown | Generic text passthrough (safe fallback for custom types) |

COPY TEXT special characters are properly escaped: backslash, tab, newline, carriage return, and the NULL sentinel `\N`.

---

## Supported Platforms

| Platform | Architecture | Compiler | Status |
|----------|-------------|---------|--------|
| Windows 10 / 11 / Server 2022+ | x64 | MSVC (VS 2022+) | Supported |
| Windows 10 / 11 / Server 2022+ | ARM64 | MSVC (VS 2022+) | Supported |
| Linux (glibc 2.31+) | x64 | GCC 12+ / Clang 14+ | Supported |
| Linux (glibc 2.31+) | ARM64 | GCC 12+ | Supported |

Linux cross-compilation from Windows is performed via Docker using GCC toolchains. The `Build-DataCrusher.ps1` script handles this automatically (requires Docker Desktop with Linux containers).

---

## License

DataCrusher is released under the [MIT License](LICENSE).

```
MIT License

Copyright (c) 2026 Tootega Pesquisa e Inovação Ltda.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

> 🤖 **Built entirely through AI-driven development with GitHub Copilot**  
> No human wrote this code directly — only prompts.

---

> *"To think is to see the possible — to control is to constrain the possible to what is correct — to act is to make the correct real. Engineering is the art of all three, executed in sequence, without skipping steps."*

