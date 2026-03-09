# DataCrusher — Copilot Coding & Architecture Guidelines
# Tootega Pesquisa e Inovação — All rights reserved © 1999-2026
# tootega.com.br

---

## Company Information

| Field | Value |
|-------|-------|
| **Razão Social** | Tootega Pesquisa e Inovação Ltda. |
| **CNPJ** | 57.099.637/0001-02 |
| **Endereço** | Avenida Hermógenes Coelho, Nº 3081, Parque Bela Vista, São Luís de Montes Belos — GO |
| **CEP** | 76.050-180 |
| **Website** | tootega.com.br |
| **DPO Email** | privacidade@tootega.com.br |

---

## 0. System Overview

DataCrusher is a **high-performance, multi-tenant Change Data Capture (CDC) engine** written in C++20. It subscribes to PostgreSQL's Write-Ahead Log (WAL) via **Logical Decoding** (`pgoutput` plugin), captures every DML event (`INSERT`, `UPDATE`, `DELETE`, `TRUNCATE`), and writes an immutable audit journal to a target PostgreSQL database using `COPY FROM STDIN` — with no ORM, no middleware, and no external message broker. The only runtime dependency is **libpq** (PostgreSQL client library).

| Aspect | Detail |
|--------|--------|
| **Language** | C++20 (MSVC 19.29+ / GCC 12+ / Clang 14+) |
| **Build system** | CMake 3.20+ |
| **Build automation** | PowerShell 7+ (`Build-DataCrusher.ps1`) |
| **Cross-compilation** | Docker (`ubuntu:24.04` for Linux, `mstorsjo/llvm-mingw` for win-arm64) |
| **Version** | 1.0.0 (defined in `CMakeLists.txt` and `DC::AppInfo::Version`) |
| **License** | Proprietary — Tootega Pesquisa e Inovação Ltda. |

---

## 1. Repository Layout

```
CDC/                                      ← workspace root
├── DataCrusher/                          ← sole C++ project
│   ├── CMakeLists.txt                    ← build definition
│   ├── CMakePresets.json                 ← build presets
│   ├── DataCrusher.vcxproj               ← VS solution for IDE use (not for CI)
│   ├── README.md                         ← full technical documentation
│   ├── src/                              ← ALL source and headers
│   │   ├── Platform.h                    ← cross-platform abstraction
│   │   ├── Constants.h                   ← all compile-time constants & namespaces
│   │   ├── Logger.h/.cpp                 ← singleton thread-safe logger
│   │   ├── TypeHandlers.h/.cpp           ← PostgreSQL binary type → text converter
│   │   ├── ReplicationStream.h/.cpp      ← WAL replication connection
│   │   ├── WalMessageParser.h/.cpp       ← pgoutput binary protocol parser
│   │   ├── TransactionManager.h/.cpp     ← transaction assembler (Begin→Commit)
│   │   ├── CopyWriter.h/.cpp             ← COPY FROM STDIN journal writer
│   │   ├── SignalTable.h/.cpp            ← session context capture
│   │   ├── ReconnectionManager.h/.cpp    ← exponential backoff reconnection
│   │   ├── LatencyMonitor.h/.cpp         ← P50/P95/P99 latency histogram
│   │   ├── TenantWorkerConfig.h          ← per-tenant config structs (header-only)
│   │   ├── TenantWorker.h/.cpp           ← per-tenant CDC pipeline thread
│   │   ├── WorkerManager.h/.cpp          ← multi-tenant orchestrator (main thread)
│   │   ├── DataCrusherEngine.h/.cpp      ← top-level engine entry point
│   │   └── Main.cpp                      ← CLI + env var resolution + startup
│   └── build/                            ← generated build dirs (git-ignored)
│       ├── win-x64/                      ← MSVC x64 build tree + vcpkg
│       ├── win-arm64/                    ← Docker build dir (toolchain + bash script)
│       ├── linux-x64/                    ← Docker linux/amd64 build tree
│       └── linux-arm64/                  ← Docker linux/arm64 build tree
│
├── Deploy/                               ← OUTPUT — all platform binaries land here
│   ├── DataCrusher.win-x64.exe           ← Windows AMD64 static binary
│   ├── DataCrusher.win-arm64.exe         ← Windows ARM64 static binary (llvm-mingw)
│   ├── DataCrusher.linux-x64             ← Linux AMD64 static ELF
│   └── DataCrusher.linux-arm64           ← Linux ARM64 static ELF
│
├── Build-DataCrusher.ps1                  ← multi-platform build orchestrator
├── CDC.slnx                              ← Visual Studio solution (IDE only, not for CI)
├── README.md                             ← project overview
├── LICENSE                               ← proprietary license
└── .github/
    └── copilot-instructions.md           ← this file
```

### Key Rules
- **One type per `.cpp`/`.h` pair.** File name exactly matches the class name.
- **All source is in `DataCrusher/src/`.** No subdirectories within `src/`.
- **All output binaries go to `Deploy/`** with the `DataCrusher.{platform-tag}[.exe]` naming convention.
- **`build/` is generated.** Never commit anything under `DataCrusher/build/`.

---

## 2. Technology Stack

| Component | Technology | Version |
|-----------|-----------|---------|
| Language | C++20 | mandatory (`CMAKE_CXX_STANDARD 20`) |
| Build | CMake | 3.20+ |
| Windows compiler | MSVC (VS 2022 / VS 2026) | 19.29+ |
| Linux compiler | GCC / Clang | 12+ / 14+ |
| win-arm64 compiler | `aarch64-w64-mingw32-clang++` via `mstorsjo/llvm-mingw` | Docker-based |
| PostgreSQL client | libpq | 14+ |
| Static libpq (Windows) | vcpkg (`libpq[core]:x64-windows-static`) | auto-bootstrapped |
| Static libpq (Linux) | built from PostgreSQL source in Docker | 16.8 |
| Static libpq (win-arm64) | built from PostgreSQL source in Docker | 16.8 |
| Cross-compilation | Docker | 24+ |
| Build automation | PowerShell | 7+ |

---

## 3. Build System

### 3.1 `Build-DataCrusher.ps1` — Recommended Entry Point

The canonical build script. Always use this instead of invoking CMake directly.

```powershell
# Build ALL available targets (detects Docker for Linux targets)
.\Build-DataCrusher.ps1

# Specific targets
.\Build-DataCrusher.ps1 -Targets win-x64
.\Build-DataCrusher.ps1 -Targets win-x64,linux-x64

# Clean build
.\Build-DataCrusher.ps1 -Targets win-x64 -Clean

# Override PostgreSQL for dynamic/dev builds
.\Build-DataCrusher.ps1 -Targets win-x64 -PostgreSqlRoot "C:\Program Files\PostgreSQL\17"
```

**Parameters:**

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `-Targets` | `string[]` | all available | `win-x64`, `win-arm64`, `linux-x64`, `linux-arm64` |
| `-Clean` | `switch` | off | Remove `DataCrusher/build/{target}` before building |
| `-PostgreSqlRoot` | `string` | auto-detected | Windows dynamic-link only; ignored for static builds |

**Script internals — key functions:**

| Function | Purpose |
|----------|---------|
| `Find-VSCMake` | Locates cmake.exe (PATH → VS bundled cmake) |
| `Get-CMakeGenerator` | Reads `vswhere` `installationVersion` major → maps to generator string |
| `Get-VSArm64Toolset` | Returns `'MSVC'`/`'ClangCL'`/`$null` — ARM64 Windows toolset availability |
| `Find-PostgreSQL` | Auto-detects PG install from `C:\Program Files\PostgreSQL\{18..15}` |
| `Ensure-VcpkgLibpq` | Clones + bootstraps vcpkg, installs `libpq[core]:{triplet}-windows-static` |
| `Build-Windows` | Drives MSVC static builds for x64/arm64 |
| `Build-WinArm64ViaDocker` | Cross-compiles win-arm64 inside `mstorsjo/llvm-mingw` container |
| `Build-Linux` | Docker `ubuntu:24.04` builds for linux-x64/linux-arm64 |

**win-arm64 fallback chain:**
1. Try native MSVC ARM64 build tools (`VC\Tools\MSVC\*\bin\Hostx64\arm64\cl.exe`)
2. Try ClangCL (`VC\Tools\Llvm\x64\bin\clang-cl.exe`)
3. Fall back to **Docker + `mstorsjo/llvm-mingw`** (always succeeds if Docker is present)

**Linux build strategy:**
- `linux/amd64` → `--platform linux/amd64` on `ubuntu:24.04` (always explicit, never assume host arch)
- `linux/arm64` → `--platform linux/arm64` on `ubuntu:24.04` (QEMU emulation)
- Both build PostgreSQL 16.8 from source inside the container (no SSL, no GSSAPI, no LDAP) for a fully static binary

### 3.2 CMake Options

All options default to the most restrictive (static, self-contained) settings for production use.

| Option | Default | Description |
|--------|---------|-------------|
| `DC_STATIC_LINK` | `ON` | Link libpq (and all stdlibs on Linux/MinGW) statically |
| `DC_STATIC_RUNTIME` | `ON` | Use static CRT on Windows (`/MT`) |
| `DC_ENABLE_ASAN` | `OFF` | AddressSanitizer (debug builds) |
| `DC_ENABLE_TSAN` | `OFF` | ThreadSanitizer (debug builds) |
| `DC_BUILD_TESTS` | `OFF` | Unit tests |
| `DC_DEPLOY_DIR` | `../Deploy` | Override output directory |
| `DC_PLATFORM_TAG` | auto | Binary name suffix (`win-x64`, `linux-arm64`, etc.) |
| `POSTGRESQL_ROOT` | auto | Windows dynamic-link: path to installed PostgreSQL |
| `POSTGRESQL_STATIC_ROOT` | `/opt/pgsql` | Static libpq root (include + lib) |

### 3.3 CMake Presets

Defined in `DataCrusher/CMakePresets.json`. Use these for IDE or manual builds:

| Preset | Platform | Mode | Notes |
|--------|---------|------|-------|
| `windows-x64-debug` | Windows | Debug | No static flags |
| `windows-x64-release` | Windows | Release | `DC_STATIC_LINK=ON`, `DC_STATIC_RUNTIME=ON` |
| `linux-x64-debug` | Linux | Debug | ASan enabled |
| `linux-x64-release` | Linux | Release | `DC_STATIC_LINK=ON` |

### 3.4 CRITICAL: Platform Flag for Linux Docker Builds

The build machine may be an ARM64 host (Windows on ARM). **Always pass `--platform linux/amd64` explicitly** when building the `linux-x64` target so Docker does not silently produce an aarch64 ELF. The script handles this; if you invoke Docker manually, always specify `--platform`.

### 3.5 CRITICAL: Windows.h Capitalization

**Use `<windows.h>` (lowercase)**, not `<Windows.h>`. On Linux-based cross-compilation hosts the filesystem is case-sensitive and `<Windows.h>` will fail to resolve. All headers in `src/` must use lowercase. This applies to all Windows SDK includes.

### 3.6 CRITICAL: Bash Script Line Endings

Any bash script written from PowerShell to be executed inside a Docker Linux container **must use LF line endings**. In `Build-DataCrusher.ps1`, PowerShell here-strings produce CRLF. Always call `.Replace("`r`n", "`n")` (after assigning to a variable, not inline on the here-string) before writing via `[System.IO.File]::WriteAllText(..., $utf8NoBOM)`. **Never use `Set-Content -Encoding utf8NoBOM`** — that encoding name is PowerShell 6+ only and will fail in Windows PowerShell 5.1.

---

## 4. Architecture

### 4.1 Process Model

```
DataCrusher process
├── Main thread: WorkerManager (blocking Run() loop)
│   ├── Polls CDCxConfig every 30 s
│   ├── Starts/stops TenantWorker threads on config changes
│   ├── Health-checks all workers every 10 s (lock-free)
│   └── Saves LSN checkpoints to ADM on shutdown
│
└── TenantWorker threads (one std::jthread per active CDCxConfig row)
    ├── ReplicationStream  → WAL replication connection to source DB
    ├── WalMessageParser   → decodes pgoutput binary stream
    ├── TransactionManager → assembles Begin/Change/Commit into Transaction
    ├── SignalTable        → query connection to source DB (session context)
    ├── CopyWriter         → COPY connection to journal DB
    └── LatencyMonitor     → P50/P95/P99 histogram (per-tenant, not shared)
```

### 4.2 Per-Tenant Connections

Each `TenantWorker` holds **three independent PG connections** — never pool or share them:

| Connection | Purpose |
|------------|---------|
| Replication (`replication=database`) | WAL stream consumption |
| Signal (normal query) | Read `CDCxSession` from source DB |
| Writer (normal query + COPY) | Write to journal DB |

### 4.3 Data Flow

```
PostgreSQL WAL (pgoutput binary)
  → ReplicationStream::ReadMessage()
      → WalMessageParser::Parse()
          → [B] Begin         → TransactionManager::BeginTransaction()
          → [R] Relation      → parser relation cache
          → [I/U/D/T] Change  → TransactionManager::AddChange(ChangeEvent)
          → [C] Commit        → SignalTable::QueryContextByXID()
                              → TransactionManager::CommitTransaction()
                              → CopyWriter::WriteTransaction()
                                    → COPY "CDCx"."CDCxTable" FROM STDIN
                              → CopyWriter::SaveCheckpointLSN()
                              → ReplicationStream::SendStandbyStatus()
```

### 4.4 Delta Compression (UPDATE-only changed columns)

The `Data` JSONB column in journal tables stores a **minimal delta**:

| Operation | `Data` content |
|-----------|---------------|
| `INSERT` | All new column values |
| `DELETE` | All old column values (requires `REPLICA IDENTITY FULL`) |
| `UPDATE` | **Only changed columns** (new value only) |
| `TRUNCATE` | `{}` |

---

## 5. Component Reference

| File | Class/Namespace | Role |
|------|----------------|------|
| `Platform.h` | global macros + `DC::` typedefs | Platform abstraction, byte-swap, timers, socket types |
| `Constants.h` | `DC::AppInfo`, `DC::Replication`, `DC::Connection`, `DC::EnvVar`, `DC::Tables`, etc. | All compile-time constants and inline utilities |
| `Logger.h/.cpp` | `DC::Logger` (singleton) | Thread-safe, rotating file + stderr logger |
| `TypeHandlers.h/.cpp` | `DC::TypeHandlers` (singleton) | OID → binary WAL bytes → COPY TEXT converter |
| `ReplicationStream.h/.cpp` | `DC::ReplicationStream` | Logical replication connection, slot management, WAL read |
| `WalMessageParser.h/.cpp` | `DC::WalMessageParser` | pgoutput binary protocol decoder, relation cache |
| `TransactionManager.h/.cpp` | `DC::TransactionManager` | XID-grouped transaction assembler |
| `CopyWriter.h/.cpp` | `DC::CopyWriter` | Journal bootstrap, COPY ingestion, checkpoint save |
| `SignalTable.h/.cpp` | `DC::SignalTable` | Publication/replica-identity bootstrap + session context reads |
| `ReconnectionManager.h/.cpp` | `DC::ReconnectionManager` | Exponential backoff with ±25% jitter |
| `LatencyMonitor.h/.cpp` | `DC::LatencyMonitor` | Capture/write/E2E latency histogram per tenant |
| `TenantWorkerConfig.h` | `DC::TenantWorkerConfig`, `DC::WorkerState`, etc. | Config structs, state enum, health snapshot (header-only) |
| `TenantWorker.h/.cpp` | `DC::TenantWorker` | Self-contained CDC pipeline on `std::jthread` |
| `WorkerManager.h/.cpp` | `DC::WorkerManager` | Multi-tenant orchestrator, main thread |
| `DataCrusherEngine.h/.cpp` | `DC::DataCrusherEngine` | Env-var resolution, logger init, signal handlers |
| `Main.cpp` | `main()` | CLI parsing (3-layer: defaults → env → CLI), engine launch |

---

## 6. Database Schema

### ADM Database — Tenant Registry

```sql
-- DataCrusher reads this to discover tenants
CREATE TABLE "CDCxConfig" (
    "ID"              UUID         PRIMARY KEY,
    "Name"            VARCHAR(200) NOT NULL,
    "DBHost"          VARCHAR(200) NOT NULL,   -- source DB host
    "DBHostPort"      INT          NOT NULL DEFAULT 5432,
    "DBName"          VARCHAR(200) NOT NULL,
    "DBUser"          VARCHAR(200) NOT NULL,
    "DBPassword"      VARCHAR(500) NOT NULL,
    "DBCDCHost"       VARCHAR(200) NOT NULL,   -- journal DB host
    "DBCDCHostPort"   INT          NOT NULL DEFAULT 5432,
    "DBCDCName"       VARCHAR(200) NOT NULL,
    "DBCDCUser"       VARCHAR(200) NOT NULL,
    "DBCDCPassword"   VARCHAR(500) NOT NULL,
    "TablePrefix"     VARCHAR(50)  NOT NULL DEFAULT 'CDCx',
    "UntrackedTables" VARCHAR(2000)         DEFAULT '',  -- semicolon-delimited
    "State"           INT          NOT NULL DEFAULT 1   -- 1 = active
);

-- Persists last confirmed LSN per tenant (created automatically)
CREATE TABLE "CDCxCheckpoint" (
    "CDCxConfigID" UUID     PRIMARY KEY,
    "SlotName"     TEXT     NOT NULL,
    "ConfirmedLSN" TEXT     NOT NULL,
    "UpdatedAt"    TIMESTAMPTZ NOT NULL
);
```

### Source Database (per tenant)

- Replication slot: `datacrusher_cdc_slot` (pgoutput, created automatically)
- Publication: `datacrusher_publication FOR ALL TABLES` (created automatically)
- Signal table: `public."{TablePrefix}Session"` — e.g. `public."CDCxSession"` (created automatically)
- All user tables must have `REPLICA IDENTITY FULL` (set automatically at bootstrap)

### Journal Database (per tenant)

- Schema: `"{TablePrefix}"` (e.g. `"CDCx"`)
- Journal table per source table: `"CDCx"."CDCx{SourceTable}"` (9 columns: `JournalID`, `XID`, `LSN`, `Operation`, `Data`, `UserID`, `Origin`, `CommittedAt`, `CapturedAt`)
- Typed view per source table: `"CDCx"."{SourceTable}"` — expands JSONB `Data` into typed columns
- Checkpoint table: `"CDCx"."Checkpoint"` (created automatically)

---

## 7. Configuration Reference

### Priority Order

```
CLI arguments  >  Environment variables  >  Hardcoded defaults
```

### All Environment Variables

| Environment Variable | CLI Flag | Default | Description |
|---------------------|----------|---------|-------------|
| `DC_ADM_HOST` | `--adm-host` | `127.0.0.1` | ADM database host |
| `DC_ADM_PORT` | `--adm-port` | `5432` | ADM database port |
| `DC_ADM_DBNAME` | `--adm-db` | `TokenGuard` | ADM database name |
| `DC_ADM_USER` | `--adm-user` | *(required)* | ADM user |
| `DC_ADM_PASSWORD` | `--adm-password` | *(required)* | ADM password |
| `DC_MAX_WORKERS` | `--max-workers` | `64` | Maximum concurrent tenant workers |
| `DC_TENANT_POLL_INTERVAL` | `--tenant-poll` | `30` | Tenant list refresh interval (seconds) |
| `DC_HEALTH_CHECK_INTERVAL` | `--health-check` | `10` | Worker health check interval (seconds) |
| `DC_RECONNECT_BASE_MS` | `--reconnect-base` | `500` | Initial reconnect backoff (ms) |
| `DC_RECONNECT_MAX_MS` | `--reconnect-max` | `60000` | Max reconnect backoff (ms) |
| `DC_RECONNECT_MAX_RETRIES` | `--reconnect-retries` | `-1` | Max retries (-1 = unlimited) |
| `DC_BATCH_SIZE` | *(none)* | `1000` | COPY rows per batch |
| `DC_FLUSH_INTERVAL_MS` | *(none)* | `500` | Forced flush interval (ms) |
| `DC_LOG_LEVEL` | `--log-level` | `info` | `trace`\|`debug`\|`info`\|`warn`\|`error`\|`fatal` |

---

## 8. Naming & Code Conventions

### 8.1 Language

**ALL code MUST be written in English** — class names, methods, fields, variables, comments, SQL column names, constant keys. No exceptions.

### 8.2 C++ Naming

| Element | Convention | Example |
|---------|-----------|---------|
| Classes, structs, enums, methods, properties | `PascalCase` | `TenantWorker`, `ReadMessage()` |
| Namespaces | `PascalCase` | `DC::Replication`, `DC::Tables` |
| Private member fields | `_` prefix + PascalCase | `_Connections`, `_ReplicationStream` |
| Parameters | `p` prefix + PascalCase | `pConfig`, `pTenantID` |
| Local variables | short `camelCase` | `msg`, `lsn`, `xid` |
| Acronyms | ALWAYS uppercase | `LSN`, `XID`, `OID`, `WAL`, `CDC` |
| Constants (namespace-scoped) | `PascalCase` | `DC::Replication::SlotName` |
| Macros | `DC_SCREAMING_SNAKE` | `DC_PLATFORM_WINDOWS`, `DC_FORCEINLINE` |

### 8.3 SQL Table and Column Names

- All SQL names use **double-quoted `PascalCase`** (e.g. `"CDCxConfig"`, `"JournalID"`, `"CommittedAt"`)
- Never use unquoted lowercase in SQL — PostgreSQL would fold to lowercase
- Table naming: entity name = table name (no extra suffixes)

### 8.4 Namespace Organization (`Constants.h`)

All compile-time constants live in `Constants.h` under sub-namespaces of `DC::`:

```
DC::AppInfo        — name, version, copyright
DC::Replication    — slot name, plugin, publication, protocol version
DC::Connection     — timeouts, defaults, SSL mode
DC::Security       — SecureErase() utility
DC::EnvVar         — all DC_* env var name strings
DC::Tables         — SQL name formatting functions
DC::JournalColumns — journal column names
DC::SignalColumns  — signal table column names
DC::Operation      — WAL operation chars (I/U/D/T/B/C/R)
DC::Performance    — batch size, flush intervals, buffer sizes
DC::Reconnection   — backoff parameters
DC::Logging        — log file limits
DC::PgTypeOID      — PostgreSQL built-in type OIDs
DC::ExitCode       — process exit codes
```

**NEVER hardcode SQL table names, column names, or env var strings as string literals outside `Constants.h`.**

---

## 9. Code Style Rules

### 9.1 Universal Rules
- **No comments on obvious code.** Comments only where the *why* is non-obvious.
- **Guard clauses first** — early returns to minimize nesting.
- **One type per file**; filename matches class name exactly.
- **No `using namespace std`** — always qualify (`std::string`, `std::format`, etc.).
- **`#pragma once`** in all headers — no include guards.

### 9.2 Performance (Zero-Allocation Mindset)

DataCrusher is a hot-path engine. Apply these rules in all critical paths:

- **No new/delete on WAL parse path** — use pre-allocated buffers and reuse.
- **`std::format` instead of `std::ostringstream`** — avoid repeated allocations.
- **Function pointer dispatch** for OID → type handler, not virtual calls.
- **`DC_BSwap*` macros** for big-endian reads — direct pointer arithmetic, not `ntohs`/`ntohl`.
- **`DC_FORCEINLINE`** on byte-swap and tight-loop helpers.
- **`DC_LIKELY`/`DC_UNLIKELY`** around rarely-taken branches (errors, reconnections).
- **Reserve vectors and strings** when size is known before the loop.

### 9.3 Thread Safety

- The **logger** is the only shared singleton — it is internally mutex-guarded.
- Each `TenantWorker` owns all its state. **No cross-tenant shared mutable state.**
- Worker health is exposed to the manager via **lock-free atomic reads** (`std::atomic`) on snapshot fields.
- Never pass raw pointers to worker state from the manager thread.

### 9.4 Error Handling

- **Never throw exceptions in the replication hot path.** Use return codes or `bool`.
- Functions that can fail return `bool` or a result enum; callers check and log.
- Connection failures are handled by `ReconnectionManager` — not try/catch.
- `FATAL` log followed by `return false` (never `std::exit()` from worker threads).

### 9.5 Resource Management

- **RAII for all libpq handles.** Wrap `PGconn*` and `PGresult*` in destructors.
- **`PQclear()` on every `PGresult*`** immediately after use — no exceptions.
- **`PQfinish()` on every `PGconn*`** in the worker's cleanup phase.
- `std::jthread` manages worker lifetime — do not `detach()`.

---

## 10. PostgreSQL Integration Notes

### 10.1 Logical Replication Requirements

The source database needs in `postgresql.conf`:
```
wal_level = logical
max_replication_slots ≥ number_of_tenants
max_wal_senders       ≥ number_of_tenants
```

### 10.2 WAL Protocol

- Output plugin: **`pgoutput`** (native binary, fastest, zero external dependencies)
- Protocol version: **4** (enables streaming mode for large transactions)
- Slot name: `datacrusher_cdc_slot` — **one slot per tenant** (not shared)
- `START_REPLICATION` resumes from the last confirmed LSN (checkpoint)
- Standby status updates sent every 10 seconds to prevent WAL accumulation

### 10.3 Replica Identity

`REPLICA IDENTITY FULL` is required on all tracked tables for UPDATE/DELETE to include old row values in the WAL stream. DataCrusher sets this automatically at bootstrap via `SignalTable::EnsureReplicaIdentityFull()`.

### 10.4 COPY FROM STDIN

All journal writes use `PQputCopyData` + `PQputCopyEnd` — never `INSERT` statements. The COPY text format uses:
- Tab character (`\t`) as the column delimiter
- `\N` as the NULL sentinel
- Backslash escaping for `\t`, `\n`, `\r`, `\\` within text values

### 10.5 Session Context Signal Table

Applications write to `public."CDCxSession"` before each transaction to attach user identity. DataCrusher reads this at COMMIT time via `QueryContextByXID(xid)` on the signal query connection (separate from the replication connection).

---

## 11. Supported Platforms & Binary Matrix

| Platform | Arch | Compiler | Link | Docker Required |
|----------|------|---------|------|----------------|
| Windows 10+/ Server 2022+ | x64 | MSVC (VS 2022+) | static (`/MT`) | No |
| Windows 10+/ Server 2022+ | ARM64 | `aarch64-w64-mingw32-clang++` | static (`-static`) | Yes (llvm-mingw) |
| Linux glibc 2.31+ | x64 | GCC 12+ | static (`-static`) | Yes (ubuntu:24.04, explicit `--platform linux/amd64`) |
| Linux glibc 2.31+ | ARM64 | GCC 12+ | static (`-static`) | Yes (ubuntu:24.04, `--platform linux/arm64`) |

**All production binaries are fully static** — zero runtime DLL/SO dependencies, deploy by copying a single executable.

---

## 12. Common Mistakes to Avoid

| Mistake | Correct Approach |
|---------|----------------|
| Use `<Windows.h>` (capital W) in any header | Use `<windows.h>` (lowercase) — case-sensitive on Linux |
| Omit `--platform linux/amd64` in Docker | Always pass explicit `--platform` — host may be ARM64 |
| Write bash scripts via PowerShell `Set-Content` | Assign here-string to variable, `.Replace("`r`n","`n")`, then `[IO.File]::WriteAllText(..., $utf8NoBOM)` |
| Use `Set-Content -Encoding utf8NoBOM` | Use `[System.Text.UTF8Encoding]::new($false)` — `utf8NoBOM` is PS 6+ only |
| Inline `docker run ... bash -c '...'` with `$(nproc)` | Write bash script to file on `$BuildDir`, then `bash /build/script.sh` |
| Share connections between tenants | Each `TenantWorker` owns 3 exclusive connections |
| Use `SELECT *` in any query | Always explicit projection |
| Hardcode SQL table/column names | Use `DC::Tables::*` and `DC::JournalColumns::*` from `Constants.h` |
| Call `std::exit()` from a worker thread | Log FATAL, return false — let WorkerManager handle cleanup |
| Forget `PQclear(result)` | Every `PGresult*` must be cleared immediately after use |
| Build all targets using the `.slnx` / `.vcxproj` | Use `Build-DataCrusher.ps1` or CMake presets directly |
| Put output binaries anywhere except `Deploy/` | CMake is configured to always output to `../Deploy` relative to `DataCrusher/` |
| Commit generated files under `DataCrusher/build/` | `build/` is generated — always git-ignored |
