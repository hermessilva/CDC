# DataCrusher - High-Performance PostgreSQL CDC Engine

**Tootega Pesquisa e Inovação © 1999-2026**

A zero-overhead Change Data Capture (CDC) engine that consumes PostgreSQL's WAL stream via Logical Decoding and writes audit journal entries to a Target database using `COPY FROM STDIN` for maximum throughput.

## Architecture

```
┌─────────────┐   WAL Stream   ┌──────────────────┐   COPY STDIN    ┌──────────┐
│  PostgreSQL  │───────────────▸│   DataCrusher    │───────────────▸│ Target DB│
│  (Tenant)    │  pgoutput      │                  │  Batched        │ cdc.journal│
│              │  Logical       │  ┌────────────┐  │  Tab-delimited  │          │
│  cdc_signal  │  Replication   │  │ WAL Parser │  │                 │ cdc.     │
│  (context)   │◂─ ─ ─ ─ ─ ─ ─ │  │ TX Manager │  │                 │checkpoint│
└─────────────┘   Signal Query  │  │ COPY Writer│  │                 └──────────┘
                                │  │ Type Hdlrs │  │
                                │  └────────────┘  │
                                └──────────────────┘
```

### Component Breakdown

| Component | File | Responsibility |
|-----------|------|----------------|
| **Constants** | `Constants.h` | All configuration, table names, SQL templates, type OIDs |
| **Platform** | `Platform.h` | Cross-platform abstraction, types, LSN utilities |
| **Logger** | `Logger.h/.cpp` | Structured logging with rotation, latency tracking |
| **TypeHandlers** | `TypeHandlers.h/.cpp` | Binary WAL → COPY TEXT conversion per PG type |
| **WalMessageParser** | `WalMessageParser.h/.cpp` | pgoutput binary protocol parser |
| **ReplicationStream** | `ReplicationStream.h/.cpp` | libpq replication connection, slot management |
| **TransactionManager** | `TransactionManager.h/.cpp` | XID grouping, session context attachment |
| **CopyWriter** | `CopyWriter.h/.cpp` | `COPY FROM STDIN` batched ingestion to Target |
| **SignalTable** | `SignalTable.h/.cpp` | Session context capture (user_id, origin) |
| **ReconnectionManager** | `ReconnectionManager.h/.cpp` | Exponential backoff with jitter |
| **LatencyMonitor** | `LatencyMonitor.h/.cpp` | Percentile-based latency histogram |
| **DataCrusherEngine** | `DataCrusherEngine.h/.cpp` | Main orchestrator, lifecycle management |
| **Main** | `Main.cpp` | CLI argument parsing, entry point |

## Building

### Prerequisites

- **C++20** compatible compiler
- **CMake 3.24+**
- **PostgreSQL** dev libraries (libpq)

### Windows (Visual Studio 2022)

```powershell
# Configure (auto-detects PostgreSQL)
cmake -B build -G "Visual Studio 17 2022" -A x64

# Or specify PostgreSQL location
cmake -B build -G "Visual Studio 17 2022" -A x64 -DPOSTGRESQL_ROOT="C:/Program Files/PostgreSQL/16"

# Build
cmake --build build --config Release

# Using presets
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
```

### Linux

```bash
# Install libpq development headers
sudo apt install libpq-dev  # Debian/Ubuntu
sudo dnf install libpq-devel  # Fedora/RHEL

# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -j$(nproc)

# Using presets
cmake --preset linux-x64-release
cmake --build --preset linux-x64-release
```

## Configuration

All options can be set via CLI arguments, environment variables, or constants in `Constants.h`.

### CLI Arguments

```bash
DataCrusher \
  --source-host 10.0.1.100 \
  --source-port 5432 \
  --source-db tenant_db \
  --source-user datacrusher_repl \
  --target-host 10.0.1.200 \
  --target-db target_db \
  --target-user datacrusher_writer \
  --slot datacrusher_cdc_slot \
  --publication datacrusher_publication \
  --batch-size 2000 \
  --flush-interval 250 \
  --log-level debug \
  --log-file /var/log/datacrusher.log
```

### Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `DC_SOURCE_HOST` | Source PostgreSQL host | `127.0.0.1` |
| `DC_SOURCE_PORT` | Source PostgreSQL port | `5432` |
| `DC_SOURCE_DBNAME` | Source database name | `tenant_db` |
| `DC_SOURCE_USER` | Replication user | `datacrusher_repl` |
| `DC_SOURCE_PASSWORD` | Replication password | (empty) |
| `DC_TARGET_HOST` | Target database host | `127.0.0.1` |
| `DC_TARGET_PORT` | Target database port | `5432` |
| `DC_TARGET_DBNAME` | Target database name | `target_db` |
| `DC_TARGET_USER` | Writer user | `datacrusher_writer` |
| `DC_TARGET_PASSWORD` | Writer password | (empty) |
| `DC_SLOT_NAME` | Replication slot name | `datacrusher_cdc_slot` |
| `DC_PUBLICATION_NAME` | Publication name | `datacrusher_publication` |
| `DC_LOG_LEVEL` | Log level | `info` |
| `DC_BATCH_SIZE` | COPY batch size | `1000` |
| `DC_FLUSH_INTERVAL_MS` | Flush interval (ms) | `500` |
| `DC_SSL_MODE` | SSL mode | `prefer` |
| `DC_RECONNECT_BASE_MS` | Initial reconnect delay | `500` |
| `DC_RECONNECT_MAX_MS` | Max reconnect delay | `60000` |
| `DC_RECONNECT_MAX_RETRIES` | Max retries (-1=∞) | `-1` |

## PostgreSQL Setup

### Source Database (Tenant)

```sql
-- 1. Enable logical replication in postgresql.conf
-- wal_level = logical
-- max_replication_slots = 4
-- max_wal_senders = 4

-- 2. Create replication user
CREATE ROLE datacrusher_repl WITH LOGIN REPLICATION PASSWORD 'secure_password';

-- 3. Create signal table for session context capture
CREATE TABLE IF NOT EXISTS public.cdc_signal (
    session_id  TEXT PRIMARY KEY,
    user_id     TEXT,
    origin      TEXT,
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- 4. Create publication
CREATE PUBLICATION datacrusher_publication FOR ALL TABLES;

-- 5. Grant permissions
GRANT USAGE ON SCHEMA public TO datacrusher_repl;
GRANT SELECT ON public.cdc_signal TO datacrusher_repl;
```

### Target Database

The engine auto-bootstraps the Target schema at startup (`--no-bootstrap` to skip):

```sql
-- Created automatically by DataCrusher:
-- cdc.journal     - Audit events
-- cdc.checkpoint  - LSN tracking for idempotent restarts
```

## Performance Characteristics

- **Zero-copy WAL parsing**: Direct big-endian reads from the binary stream
- **COPY FROM STDIN**: Bypasses SQL parsing overhead for bulk writes
- **Transaction buffering**: Accumulates changes per XID before flush
- **Type-specific handlers**: Binary WAL → text conversion without intermediate formats
- **Contiguous memory buffers**: Minimizes allocations in the hot path
- **Latency monitoring**: Histogram-based percentile tracking (P50/P95/P99)

## Resilience

- **Automatic reconnection** with exponential backoff + jitter
- **LSN-based idempotency**: Checkpoint confirmed only after successful Target write
- **Dual LSN persistence**: Database checkpoint + local file fallback
- **Graceful shutdown**: SIGINT/SIGTERM handling, flushes pending data
- **Transaction abort on disconnect**: Re-receives from last confirmed LSN
