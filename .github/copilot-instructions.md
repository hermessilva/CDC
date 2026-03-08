# TokenGuard — Copilot Coding & Architecture Guidelines
# Tootega Pesquisa e Inovação — All rights reserved © 1999-2026
# tootega.com.br

---

## Company Information

| Field | Value |
|-------|-------|
| **Razão Social** | Tootega Pesquisa e Inovação Ltda. |
| **Nome Fantasia** | Tootega Pesquisa e Inovação |
| **CNPJ** | 57.099.637/0001-02 |
| **Inscrição Municipal** | 1000648 |
| **Endereço** | Avenida Hermógenes Coelho, QD 00012, LT 00003, Nº 3081, Parque Bela Vista 2ª Etapa |
| **CEP** | 76.050-180 |
| **Município** | São Luís de Montes Belos — GO |
| **Telefone** | 62 991230002 |
| **Website** | tootega.com.br |
| **DPO Email** | privacidade@tootega.com.br |

---

## 0. System Overview

| Aspect | Detail |
|--------|--------|
| **Type** | Multi-tenant SaaS administration platform with enterprise security tools |
| **Deployments** | **ADM** (admin console) + **SaaS** (subscriber app) + **DOC** (document management) |
| **Framework** | .NET 10 (backend), React 19 + Vite 6 + TypeScript (frontend), C++20 (native tools) |
| **Target** | `net10.0` — defined in `Directory.Build.props` |
| **Database** | PostgreSQL 17 (prod), SQL Server (alt), SQLite (tests) |
| **Deployment** | Docker Swarm via GitHub Actions CD, images on GHCR |
| **SSL** | Cloudflare Flexible (SSL terminates at Cloudflare edge) |
| **Languages** | 4 primary backend locales (`pt-BR`, `en-001`, `es-419`, `zh-TW`) |

### 0.1 Production URLs

| Service | URL | Published Port |
|---------|-----|----------------|
| ADM Frontend | `https://admtokenguard.tootega.com.br` | 3003 |
| SaaS Frontend | `https://apptokenguard.tootega.com.br` | 3113 |
| DOC Frontend | `https://docguard.com.br` | 3223 |
| ADM Backend | internal | 8080 |
| SaaS Backend | internal | 8081 |
| DOC Backend | internal | 8082 |

### 0.2 Dev Server Ports

| Service | Port | Backend Proxy |
|---------|------|---------------|
| Backend ADM | 5005 | — |
| Backend SaaS | 5115 | — |
| Backend DOC | 5225 | — |
| Frontend ADM | 3003 | `localhost:5005` |
| Frontend SaaS | 3113 | `localhost:5115` |
| Frontend DOC | 3223 | `localhost:5225` |

### 0.3 Key Technologies Stack

| Layer | Technology | Version |
|-------|------------|---------|
| Backend Runtime | .NET | 10.0 |
| Backend Web | ASP.NET Core | 10.0 |
| ORM | Entity Framework Core | 10.0.3 |
| API Docs | Scalar + OpenAPI | 2.12.38 |
| Auth | JWT Bearer | 10.0.3 |
| Frontend Runtime | React | 19.0 |
| Frontend Bundler | Vite | 6.0 |
| Frontend Router | TanStack Router | 1.93 |
| Frontend State | TanStack Query | 5.62 |
| Styling | Tailwind CSS | 3.4 |
| i18n | i18next | 25.8 |
| Native Tools | C++20 | VS 2022 |
| Container | Docker | 24+ |
| Orchestration | Docker Swarm | — |

---

## 1. Repository Layout & Module Rules

### 1.1 Complete Structure

```
TokenGuard/
├── Back/                               # All backend projects (.NET 10)
│   ├── ADM/                            # ADM-specific backend
│   │   ├── Launcher/                   # ADM API entry point (Web SDK)
│   │   │   ├── Dockerfile              # Build context: repo root (.)
│   │   │   ├── docker-entrypoint.sh
│   │   │   ├── Program.cs
│   │   │   └── Version.props           # Auto-incrementing build number
│   │   ├── TokenGuard.Core/            # Framework library (shared binary, XModel/XLang)
│   │   ├── TokenGuard.Common/          # ADM entities, DTOs, interfaces
│   │   ├── TokenGuard.Services/        # ADM business logic
│   │   ├── TokenGuard.Infra/           # ADM data access (SYSxDBContext)
│   │   └── Tests/
│   │       ├── TokenGuard.Test.Unit/
│   │       ├── TokenGuard.Test.Integration/
│   │       └── TokenGuard.Test.Shared/
│   │
│   ├── SaaS/                           # SaaS-specific backend
│   │   ├── TokenGuard.SaaS.Launcher/   # SaaS API entry point (Web SDK)
│   │   ├── TokenGuard.SaaS.Common/     # SaaS entities (DOCx*), DTOs
│   │   ├── TokenGuard.SaaS.Infra/      # SaaS data access (DOCxDBContext)
│   │   ├── TokenGuard.SaaS.Services/   # SaaS business logic
│   │   └── Tests/
│   │       └── TokenGuard.SaaS.Test.Unit/
│   │
│   ├── DOC/                            # DOC-specific backend (Document Guard)
│   │   ├── TokenGuard.DOC.Launcher/    # DOC API entry point (Web SDK)
│   │   ├── TokenGuard.DOC.Common/      # DOC entities, DTOs
│   │   ├── TokenGuard.DOC.Infra/       # DOC data access
│   │   └── TokenGuard.DOC.Services/    # DOC business logic
│   │
│   ├── Shared/                         # Shared projects (.shproj) — NO .csproj!
│   │   ├── TokenGuard.Shared.Common/   # Shared entities, DTOs, interfaces, localization
│   │   ├── TokenGuard.Shared.Infra/    # Shared repositories, EF configs, seeders
│   │   ├── TokenGuard.Shared.Service/  # Shared services (users, roles)
│   │   ├── TokenGuard.Shared.Launcher/ # Shared controllers, middleware, extensions
│   │   └── Auth/                       # Authentication shared projects (for DOC)
│   │       ├── TokenGuard.Shared.Auth.Common/   # Auth entities, DTOs
│   │       ├── TokenGuard.Shared.Auth.Infra/    # Auth data access
│   │       ├── TokenGuard.Shared.Auth.Service/  # Auth business logic
│   │       └── TokenGuard.Shared.Auth.Launcher/ # Auth controllers
│   │
│   └── DevOps/                         # Pulumi-based tenant orchestrator (.NET 10)
│       ├── TokenGuard.DevOps.Launcher/
│       ├── TokenGuard.DevOps.Common/
│       └── TokenGuard.DevOps.Services/
│
├── Front/                              # Frontend monorepo (React 19 + TypeScript)
│   ├── Core/                           # Shared component library (source-only, no build)
│   │   └── src/
│   │       ├── components/             # UI components, layouts, templates, editors
│   │       ├── contexts/               # React contexts (Auth, Sidebar, Tabs, Tour, Error)
│   │       ├── hooks/                  # Custom hooks
│   │       ├── i18n/                   # i18next config + locales/
│   │       ├── lib/                    # API client, query client, utilities
│   │       ├── styles/                 # Global CSS, design tokens
│   │       ├── types/                  # TypeScript type definitions
│   │       └── test/                   # Test utilities
│   │
│   ├── ADM/                            # ADM SPA (React + TanStack Router)
│   │   ├── Dockerfile                  # Build context: ./Front
│   │   ├── nginx.conf
│   │   ├── Tests/                      # Playwright E2E tests
│   │   └── src/                        # Pages, routes, version
│   │
│   ├── SaaS/                           # SaaS SPA (React + TanStack Router)
│   │   ├── Dockerfile                  # Build context: ./Front
│   │   ├── nginx.conf
│   │   └── src/
│   │
│   └── DOC/                            # DOC SPA (React + TanStack Router)
│       ├── Dockerfile                  # Build context: ./Front
│       ├── nginx.conf
│       ├── Tests/                      # Playwright E2E tests
│       └── src/
│
├── Tools/                              # C++ Native Tools (Windows-only, Visual Studio)
│   ├── TokenGuardCommon/               # Core C++ utility library (static .lib)
│   ├── TokenGuardMonitor/              # Certificate monitoring Windows service
│   ├── TokenGuardKSP/                  # CNG Key Storage Provider (DLL)
│   ├── TokenGuardKSPConfig/            # KSP configuration utility
│   ├── TokenGuardCapture/              # Screen recording (DDUP + H.264)
│   ├── TokenGuardLog/                  # Forensic event log analyzer
│   ├── TokenGuardFakeAgent/            # Capture control panel (test utility)
│   ├── TokenGuardInstaller/            # WiX MSI installer
│   ├── DataCrusher/                    # PostgreSQL CDC engine (WAL → audit journal)
│   ├── TFX.Runtime/                    # Runtime support library
│   ├── Inc/                            # Shared C++ headers
│   ├── Include/                        # External headers
│   ├── Lib/                            # Pre-built libraries
│   └── docs/                           # Tools documentation
│
├── deploy/                             # Deployment configuration
│   ├── stacks/                         # Docker Swarm stack YAML files
│   │   ├── backend-adm.yaml
│   │   ├── backend-saas.yaml
│   │   ├── backend-doc.yaml
│   │   ├── frontend-adm.yaml
│   │   ├── frontend-saas.yaml
│   │   ├── frontend-doc.yaml
│   │   ├── database.yaml
│   │   ├── reverse-proxy.yaml
│   │   └── RabbitMQ.yaml
│   ├── scripts/                        # Deployment scripts
│   └── create-release-tag.ps1          # Release tag generator
│
├── .github/
│   ├── workflows/
│   │   ├── ci.yml                      # Continuous Integration
│   │   ├── cd.yml                      # Continuous Deployment (main)
│   │   ├── cd-adm.yml                  # ADM-only deployment
│   │   ├── cd-saas.yml                 # SaaS-only deployment
│   │   ├── cd-doc.yml                  # DOC-only deployment
│   │   ├── cd-proxy.yml                # Reverse proxy deployment
│   │   └── cd-rabbitmq.yml             # RabbitMQ deployment
│   └── copilot-instructions.md         # This file
│
├── Run/                                # Local run configuration
│   ├── ADM/                            # ADM dev runtime files
│   ├── SaaS/                           # SaaS dev runtime files
│   └── DOC/                            # DOC dev runtime files
│
├── Script/                             # Database scripts
│   └── Create DB.sql
│
├── Directory.Build.props               # Centralized build output → Dist/
└── TokenGuard.slnx                     # Solution file (XML format)
```

### 1.2 CRITICAL: Three Backend Deployments

The system has THREE independent backend deployments:

| Deployment | Purpose | Shared Projects Used | DB Context |
|------------|---------|---------------------|------------|
| **ADM** | Admin console for system management | `Shared.*` | SYSxDBContext |
| **SaaS** | Subscriber application | `Shared.*` | DOCxDBContext |
| **DOC** | Document management (Pildra) | `Shared.Auth.*` only | AuthDBContext |

**Key Difference:** DOC uses a separate Auth-only shared layer (`Back/Shared/Auth/`), not the full `Shared.*` projects.

### 1.3 CRITICAL: Shared Projects Are `.shproj` (NOT `.csproj`)

All projects under `Back/Shared/` use MSBuild Shared Projects (`.shproj` + `.projitems`). They are **NOT** compiled into DLLs — their source is compiled directly into the consuming project.

**Impact on Dockerfiles:** When copying project files for `dotnet restore`, copy `.shproj` and `.projitems` — NEVER reference `.csproj` for Shared projects.

```dockerfile
# CORRECT — Shared projects
COPY ["Back/Shared/TokenGuard.Shared.Common/TokenGuard.Shared.Common.shproj", "Back/Shared/TokenGuard.Shared.Common/"]
COPY ["Back/Shared/TokenGuard.Shared.Common/TokenGuard.Shared.Common.projitems", "Back/Shared/TokenGuard.Shared.Common/"]

# WRONG — Shared projects have no .csproj
COPY ["Back/Shared/TokenGuard.Shared.Common/TokenGuard.Shared.Common.csproj", "..."]
```

**The only `.csproj` shared between ADM, SaaS, and DOC is `Back/ADM/TokenGuard.Core/`.**

### 1.4 CRITICAL: Module Separation Rules

| Code Scope | Backend Location | Frontend Location |
|------------|------------------|-------------------|
| Framework (XModel, XLang) | `Back/ADM/TokenGuard.Core/` | `Front/Core/` |
| Shared (SYSx entities, auth) | `Back/Shared/TokenGuard.Shared.*` | `Front/Core/` |
| Auth-only Shared (for DOC) | `Back/Shared/Auth/TokenGuard.Shared.Auth.*` | `Front/Core/` |
| ADM-specific | `Back/ADM/` projects | `Front/ADM/` |
| SaaS-specific (DOCx*) | `Back/SaaS/TokenGuard.SaaS.*` | `Front/SaaS/` |
| DOC-specific | `Back/DOC/TokenGuard.DOC.*` | `Front/DOC/` |

**NEVER:**
- Put deployment-specific code in Shared or Core
- Put module-specific translations in `Front/Core/src/i18n/locales/`
- Reference `.csproj` for Shared projects

### 1.5 CRITICAL: Backend and Frontend Independence

Backend and Frontend are **completely independent systems** with separate build pipelines.

- Backend tests CANNOT read, validate, or reference any Frontend files
- Frontend tests CANNOT read, validate, or reference any Backend files
- Backend defines `CoreKeys` as constants (dot notation: `button.new`, `common.status`)
- Frontend has translations for all CoreKeys it uses
- Each system must be self-contained and testable in isolation

### 1.6 Dependency Graph

**ADM:**
```
Launcher → Services → Common → Core
         → Infra    → Common → Core
         (imports Shared.Launcher.projitems)
```

**SaaS:**
```
SaaS.Launcher → SaaS.Services → SaaS.Common → Core
              → SaaS.Infra    → SaaS.Common → Core
              (imports Shared.Launcher.projitems)
```

**DOC:**
```
DOC.Launcher → DOC.Services → DOC.Common → Core
             → DOC.Infra    → DOC.Common → Core
             (imports Shared.Auth.Launcher.projitems)
```

**Shared project injection (source-level, not binary):**
```
Shared.Common.projitems       → TokenGuard.Common + SaaS.Common
Shared.Infra.projitems        → TokenGuard.Infra + SaaS.Infra
Shared.Service.projitems      → TokenGuard.Services + SaaS.Services
Shared.Launcher.projitems     → Launcher + SaaS.Launcher
Shared.Auth.*.projitems       → DOC.* projects
```

### 1.7 Entity Module Prefixes

| Prefix | Module | Deployment |
|--------|--------|------------|
| `SYSx` | System (users, roles, menus, claims) | Shared (ADM + SaaS) |
| `DOCx` | Document/Pessoa management | SaaS only |
| `AUTH` | Authentication entities | DOC only (via Shared.Auth) |

---

## 2. Tools — Native C++ Projects (Windows Only)

### 2.1 Overview

The `Tools/` folder contains Windows-native C++ applications built with Visual Studio 2022. These are **NOT part of CI/CD** — they are built separately on Windows and deployed manually to client machines.

### 2.2 Project Catalog

| Project | Type | Purpose | Technologies |
|---------|------|---------|--------------|
| **TokenGuardCommon** | Static lib (.lib) | Core utility library for all C++ projects | C++17/20, RAII, Result types |
| **TokenGuardMonitor** | Windows Service | Certificate monitoring and protection | CAPI2, ETW, WinHTTP, CertStore API |
| **TokenGuardKSP** | CNG Provider (DLL) | Key Storage Provider with 2FA | CNG, DPAPI, NCrypt, PKCS |
| **TokenGuardKSPConfig** | Console app | KSP registration and configuration | Registry, CNG |
| **TokenGuardCapture** | Executable | Multi-monitor screen recording | DDUP, H.264, Media Foundation, DirectX 11 |
| **TokenGuardLog** | Console app | Forensic event log analyzer | Windows Event Log API, HTML/JSON reports |
| **TokenGuardFakeAgent** | GUI app | Capture control panel (test utility) | Win32 GUI, Named Events |
| **TokenGuardInstaller** | WiX MSI | Enterprise installer package | WiX 4, GPO-ready, Silent install |
| **DataCrusher** | Executable | PostgreSQL CDC engine | libpq, Logical Replication, COPY protocol |
| **TFX.Runtime** | DLL | Runtime support library | C++ runtime |

### 2.3 TokenGuardCommon — Core Library

**Purpose:** High-performance static library providing foundational utilities for all TokenGuard C++ projects.

**Key Modules:**
- `XPlatform.h` — Platform abstraction, Windows SDK targeting
- `XTypes.h` — RAII wrappers (`XUniqueHandle`, `XUniqueCertContext`)
- `XResult.h` — Monadic error handling (`XResult<T>`)
- `XMemory.h` — Zero-allocation memory management
- `XString.h` — String utilities
- `XLogger.h` — Structured logging with rotation
- `XRegistry.h` — Registry operations
- `XCrypto.h` — SHA/HMAC cryptography
- `XFile.h` — File I/O
- `XProcess.h` — Process management
- `XEventLog.h` — Windows Event Log writer
- `XEventLogForensic.h` — Deep event scan API
- `XCapturePipeClient/Server.h` — IPC for screen capture
- `XDemoGuard.h` — Trial protection

**Design Principles:**
- Zero-allocation hot paths
- RAII everywhere (automatic resource cleanup)
- `noexcept` on hot paths
- Result types instead of exceptions
- Thread-safe logging

### 2.4 TokenGuardMonitor — Certificate Monitoring Service

**Purpose:** Real-time certificate store monitoring, protection, and synchronization with backend.

**Features:**
- **Certificate Store Monitoring:** Detects additions/removals in User and Machine stores
- **CAPI2 Event Capture:** Tracks cryptographic operations via ETW
- **Blacklist Enforcement:** Auto-blocks unauthorized certificates
- **Backend Sync:** Reports certificate inventory via WinHTTP
- **Forensic Logging:** Complete audit trail with Event Log integration

**Technologies:** CAPI2, ETW, Certificate Store API, WinHTTP

### 2.5 TokenGuardKSP — Key Storage Provider

**Purpose:** Custom CNG Key Storage Provider with 2FA and audit logging.

**Features:**
- **Export Control:** Configurable key export blocking
- **2FA Integration:** Requires authentication before critical operations
- **Audit Trail:** Logs every cryptographic operation
- **HSM-Ready:** Designed for future hardware security module integration
- **Transparent Integration:** Works with all CNG-aware applications

**Technologies:** CNG, DPAPI, NCrypt, RSA, PKCS#1/PKCS#8/PKCS#12

### 2.6 TokenGuardCapture — Screen Recording

**Purpose:** High-performance multi-monitor screen capture for compliance and audit.

**Features:**
- **GPU-Accelerated:** DirectX 11 + DDUP (Desktop Duplication API)
- **Multi-Monitor:** Simultaneous capture of all displays
- **H.264 Native:** Hardware-accelerated encoding via Media Foundation
- **Zero Dependencies:** 100% Windows native (no FFmpeg, codecs, etc.)
- **< 2% CPU:** Minimal performance impact

**Technologies:** DDUP, H.264/AVC, Media Foundation, DirectX 11

### 2.7 DataCrusher — PostgreSQL CDC Engine

**Purpose:** Zero-overhead Change Data Capture from PostgreSQL to audit journal.

**Architecture:**
```
PostgreSQL (Tenant) → WAL Stream (pgoutput) → DataCrusher → COPY STDIN → Target DB (cdc.journal)
```

**Features:**
- Logical replication consumption
- Session context capture via `cdc_signal` table
- Batched writes with `COPY FROM STDIN`
- Exponential backoff reconnection
- Latency monitoring with percentile histograms

**Technologies:** libpq, Logical Decoding, COPY protocol, C++20

### 2.8 Building Native Tools

**Prerequisites:**
- Visual Studio 2022 with C++ workload
- Windows SDK 10.0.19041.0+
- CMake 3.24+ (for DataCrusher)
- PostgreSQL dev libraries (for DataCrusher)

**Build from Visual Studio:**
```
Open TokenGuard.slnx → Select Tools/{Project} → Build
```

**Build DataCrusher (CMake):**
```powershell
cd Tools/DataCrusher
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
```

**IMPORTANT:** C++ projects are excluded from `dotnet build` and CI/CD. They must be built on Windows with Visual Studio.

---

## 3. Language and Naming Standards

### 3.1 CRITICAL: All Code in English
- **ALL code MUST be written in English**, including:
  - Class names, method names, property names, field names
  - Variable names, parameter names
  - Comments and XML documentation
  - Database table names, column names
  - JSON keys in translation files, error message keys
- **Translation values** are the ONLY exception (they contain localized text)

### 3.2 Naming Conventions

| Element | Convention | Example |
|---------|-----------|----------|
| Classes, Structs, Enums, Records, Methods, Properties | `PascalCase` | `GetByID`, `SaveChanges` |
| Interfaces | `I` prefix | `IRepository`, `IDOCxPessoaService` |
| Private Fields | `_` prefix + PascalCase | `_Cache`, `_UserRepository` |
| Parameters | `p` prefix + PascalCase | `pUserID`, `pTenantName` |
| Local Variables | Short, `camelCase` | `user`, `count`, `dto` |
| Acronyms | ALWAYS uppercase | `GetByID`, `LoadFromDB`, `pUserID` |
| DTOs | Uppercase suffix | `AuthenticationDTO`, `UserDTO` |
| Core Framework Classes | `X` prefix | `XUserService`, `XDataView` |
| Core Interfaces | `XI` prefix | `XIRepository` |
| DB Table Names | Entity name = Table name | `SYSxUser` |
| DB Column Names | Property name = Column name | exact match |
| EF Configurations | Entity + `Configuration` | `SYSxUserConfiguration` |
| C++ Classes | `X` prefix | `XCaptureAgent`, `XVideoEncoder` |
| C++ RAII Types | `XUnique` prefix | `XUniqueHandle`, `XUniqueCertContext` |

### 3.3 Abbreviations (Always Uppercase)
CEP, CPF, CNPJ, ID, URL, HTTP, JSON, XML, SQL, DB, UI, UX, DTO, API, CDC, WAL, KSP, CNG, ETW, CAPI, DDUP, MSI, GPO

---

## 4. Code Style (Zero-Noise)

### 4.1 General Rules
- **Self-documenting:** No comments; XML-Doc only for public APIs
- **One type per file;** filename matches type name
- **No braces** `{}` for single-line blocks (`if`, `foreach`, `while`)
- **Guard Clauses:** Early returns mandatory to avoid nesting
- **No Lambdas** in hot paths: avoid `Func<>`, `Action<>`

### 4.2 C++ Specific
- **RAII:** All resources managed via smart pointers/unique handles
- **Result Types:** Use `XResult<T>` instead of exceptions
- **`noexcept`:** Mandatory on hot paths
- **`TokenGuard_DISABLE_COPY_MOVE`:** Macro for non-copyable types
- **Headers:** Use `#pragma once` (no include guards)

---

## 5. Performance (Zero-Allocation Mindset)

### 5.1 .NET Performance
- **Anti-LINQ:** No LINQ in hot paths; use `for`/`foreach`
- **Async:** `async/await` for I/O; `ConfigureAwait(false)` in libraries
- **ValueTask:** Use `ValueTask<T>` for methods that often complete synchronously
- **Value Types:** Use `readonly struct` and `sealed class` by default
- **Buffers:** `Span<T>`, `ReadOnlySpan<T>`, `StringBuilder` for text

### 5.2 C++ Performance
- **Zero-copy:** Minimize memory allocations
- **Pre-allocated buffers:** Reuse buffers in hot paths
- **Stack allocation:** Prefer stack over heap
- **Move semantics:** Use `std::move` for transfers

---

## 6. Data and Security

- **SOLID:** Focused interfaces, exhaustive Dependency Injection
- **Immutability:** Prefer `record` for DTOs, `readonly` fields
- **Queries:** Explicit projections (`Select`); no `SELECT *` or N+1
- **SQL Injection:** Zero tolerance; always parameterized
- **Secrets:** No hardcoding; use `IConfiguration`, Docker Secrets, or vaults
- **Exceptions:** Flow control via `Try*` pattern
- **Resources:** `using` declarations for all `IDisposable`
- **NO EXTERNAL RUNTIME RESOURCES:** NEVER load images, fonts, scripts from CDNs at runtime. ALL resources MUST be static files bundled at build time.

---

## 7. Internationalization (i18n)

### 7.1 Supported Languages

**Backend (4):** `pt-BR`, `en-001`, `es-419`, `zh-TW`

**Frontend (4):** `pt-BR`, `en-001`, `es-419`, `zh-TW`

### 7.2 Backend Localization (XLang System)
- Use `XLang.Get(XLangKey.KeyName)` for all user-facing messages
- Keys defined in `XLangKey` enum (type-safe)
- Translations in `Localization/Translations/Messages.{culture}.json`
- Per-application translations in `Localization/Apps/{AppCode}/Translations/`
- NEVER hardcode user-facing strings in services/controllers
- Startup validates all translations (fail-fast in Release mode)

### 7.3 Frontend Localization (i18next)
- Use `t('key.name')` hook for all UI text
- ALL locale files live in `Front/Core/src/i18n/locales/` (4 files)
- All 4 bundles are eagerly loaded (no lazy loading)
- Missing keys logged to console via `saveMissing` handler

### 7.4 CRITICAL: All Locale Files Must Be Updated Together
- **MANDATORY:** When adding/modifying ANY translation key, ALL 4 locale files MUST be updated simultaneously
- Location: `Front/Core/src/i18n/locales/`
- Files: `pt-BR.json`, `en-001.json`, `es-419.json`, `zh-TW.json`
- **NEVER add a key to only some files**

### 7.5 Application Texts Come from Backend
- **ALL application-specific texts come from backend already translated** (tours, tabs, field labels, entity names, error/validation messages)
- **Frontend locale files are ONLY for:** generic UI strings (buttons, common labels, framework messages, static page content)
- When a translation appears untranslated, the problem is in the **backend** not the frontend

### 7.6 Accept-Language Header (Mandatory)
- Frontend MUST send `Accept-Language` header on ALL API requests
- Backend MUST validate and return HTTP 400 if missing/invalid
- Supported: `pt-BR`, `en-001`, `es-419`, `zh-TW`
- Frontend MUST invalidate caches and reload data on language change

---

## 8. API Contract Requirements

### 8.1 Required Headers
- `Accept-Language` — on all localized endpoints
- `Authorization: Bearer <token>` — on authenticated endpoints
- Missing headers → HTTP 400 Bad Request with localized error

### 8.2 Validation Pattern
```csharp
var lang = Request.Headers.AcceptLanguage.FirstOrDefault();
if (string.IsNullOrWhiteSpace(lang) || !XLangCultures.IsSupported(lang))
    return BadRequest(new { Message = "Accept-Language required (pt-BR, en-001, es-419, zh-TW)" });
```

### 8.3 Zero-Tolerance
- NEVER assume default values for critical request data
- ALWAYS return explicit, localized errors for missing/invalid inputs

---

## 9. Frontend Guidelines (React / TypeScript)

### 9.1 Architecture

| Concern | Technology |
|---------|------------|
| Routing | TanStack Router (file-based) |
| Server State | TanStack React Query (5min stale, 30min GC) |
| Client State | React Context (Auth, Sidebar, Tabs, ProductTour, Error) |
| Styling | Tailwind CSS 3.4 + CSS custom properties (design tokens) |
| Themes | `[data-theme]` attribute on `<html>`, HSL color tokens |
| Tours | driver.js via `ProductTourContext` |
| Builds | Vite 6 with `build:no-increment` for Docker |
| Testing | Vitest (unit) + Playwright (E2E) |

**No Redux, Zustand, or external state libraries.** Context + TanStack Query only.

### 9.2 Front/Core — Shared Library

Core is a **source-only** TypeScript library (no build step). Consumed via:
- `package.json`: `"@tokenguard/core": "file:../Core"`
- Path alias: `@core/` → `../Core/src/*` (in tsconfig + vite.config)
- Tailwind: Core components scanned via `../Core/src/**/*.{js,ts,jsx,tsx}`

**Core provides:**
- `components/` — UI components, layout, templates, XModel editors
- `contexts/` — Auth, Sidebar, Tabs, ProductTour, Error
- `hooks/` — Custom React hooks
- `i18n/` — i18next configuration and locale files
- `lib/` — API client, query client, utilities
- `styles/` — Global CSS, design tokens
- `types/` — TypeScript type definitions

### 9.3 XModel-Driven UI (Template System)

The backend sends `XModel` schemas that describe entire screens. The frontend renders them dynamically:

`XApplicationView` → `XButtonBar` + `XFilter` + `XDataView` + `XPagination` + `XFormModal` + `XTabbedFormModal` + `XExportDialog` + `XApplicationTour`

**15 editor types:** Text, ComboBox, Lookup, Document (CPF/CNPJ), Date, DateTime, Time, Number, Currency, Percent, Boolean, Color, URL — dispatched by `XEditor`.

### 9.4 ADM/SaaS/DOC — Thin Shells

Each frontend app is a minimal shell providing:
- Entry point (`main.tsx`) with provider tree
- TanStack Router routes (`/`, `/login`, `/home`)
- Root layout (`__root.tsx`: Header + Sidebar + Tabs + Tour)
- Version tracking (`version.ts` — auto-incremented by `build` script)
- Deployment config (Dockerfile, nginx.conf, `.env.production`)

**Provider tree:**
```
<StrictMode> → QueryClientProvider → ErrorProvider → AuthProvider → ProductTourProvider → SidebarProvider → TabsProvider → RouterProvider
```

### 9.5 Build Scripts

| Script | Purpose |
|--------|---------|
| `build` | Increments version + `tsc -b` + `vite build` |
| `build:no-increment` | `tsc -b` + `vite build` — **used in Docker** |
| `copy-core-assets` | Copies Core public assets (runs as `predev`/`prebuild`) |
| `dev` | Vite dev server with HMR |
| `test` | `vitest run --passWithNoTests` |

### 9.6 Mobile-First Design
- Design mobile-first, enhance for desktop
- Responsive Tailwind: `sm:` (640px), `lg:` (1024px), `xl:` (1280px)
- **Minimum touch target: 44×44px** — use `min-h-[44px]` / `min-w-[44px]`
- Modals: full-screen on mobile, centered on desktop
- Forms: single-column on mobile, grid on desktop
- Buttons: full-width on mobile, auto-width on desktop
- **Mobile has NO double-click!** Use action buttons for row actions

### 9.7 CSS Design Tokens
```css
--row-height: 48px (mobile) / 40px (desktop)
--header-height: 52px (mobile) / 45px (desktop)
--touch-target-min: 44px
--ui-font-size: 15px (mobile) / 14px (desktop)
```

---

## 10. Docker & Deployment

### 10.1 CRITICAL: Build Contexts

| Image | Dockerfile Path | Build Context | Why |
|-------|-----------------|---------------|-----|
| Backend ADM | `Back/ADM/Launcher/Dockerfile` | `.` (repo root) | Needs `Back/`, `Directory.Build.props` |
| Backend SaaS | `Back/SaaS/TokenGuard.SaaS.Launcher/Dockerfile` | `.` (repo root) | Needs `Back/SaaS/`, `Back/ADM/Core/`, `Back/Shared/` |
| Backend DOC | `Back/DOC/TokenGuard.DOC.Launcher/Dockerfile` | `.` (repo root) | Needs `Back/DOC/`, `Back/ADM/Core/`, `Back/Shared/Auth/` |
| Frontend ADM | `Front/ADM/Dockerfile` | `./Front` | Needs `Core/` + `ADM/` |
| Frontend SaaS | `Front/SaaS/Dockerfile` | `./Front` | Needs `Core/` + `SaaS/` |
| Frontend DOC | `Front/DOC/Dockerfile` | `./Front` | Needs `Core/` + `DOC/` |

**CRITICAL:** Frontend Dockerfiles use `./Front` as context (NOT `./Front/ADM`), because they need access to `Core/` as a sibling directory.

### 10.2 Frontend Dockerfile Pattern
```dockerfile
# Stage 1: Build (node:20-alpine)
COPY Core/package.json Core/package-lock.json* ./Core/
COPY Core/src ./Core/src
COPY Core/tsconfig.json ./Core/
WORKDIR /app/Core
RUN npm ci --silent

WORKDIR /app/ADM            # or /app/SaaS or /app/DOC
COPY ADM/package.json ADM/package-lock.json* ./
RUN npm ci --silent
COPY ADM/ .
RUN npm run build:no-increment

# Stage 2: Production (nginx:alpine)
COPY ADM/nginx.conf /etc/nginx/nginx.conf       # Path relative to context!
COPY --from=build /app/ADM/dist /usr/share/nginx/html
```

### 10.3 Backend Dockerfile — Shared Project Files
```dockerfile
# Copy regular .csproj projects
COPY ["Back/DOC/TokenGuard.DOC.Launcher/TokenGuard.DOC.Launcher.csproj", "Back/DOC/TokenGuard.DOC.Launcher/"]
COPY ["Back/ADM/TokenGuard.Core/TokenGuard.Core.csproj", "Back/ADM/TokenGuard.Core/"]

# Copy shared projects — .shproj + .projitems (NOT .csproj!)
COPY ["Back/Shared/Auth/TokenGuard.Shared.Auth.Common/TokenGuard.Shared.Auth.Common.shproj", "Back/Shared/Auth/TokenGuard.Shared.Auth.Common/"]
COPY ["Back/Shared/Auth/TokenGuard.Shared.Auth.Common/TokenGuard.Shared.Auth.Common.projitems", "Back/Shared/Auth/TokenGuard.Shared.Auth.Common/"]
# ... repeat for other Shared.Auth projects

COPY ["Directory.Build.props", "./"]
RUN dotnet restore "Back/DOC/TokenGuard.DOC.Launcher/TokenGuard.DOC.Launcher.csproj"
```

### 10.4 Docker Swarm Topology

| Stack | Service | Container Port | Published Port | Resources |
|-------|---------|---------------|----------------|-----------|
| `tokenguard-backend-adm` | API | 5005 | **8080** | 1 CPU / 512M |
| `tokenguard-backend-saas` | API | 5115 | **8081** | 1 CPU / 512M |
| `tokenguard-backend-doc` | API | 5225 | **8082** | 1 CPU / 512M |
| `tokenguard-frontend-adm` | nginx | 80 | **3003** | 0.5 CPU / 256M |
| `tokenguard-frontend-saas` | nginx | 80 | **3113** | 0.5 CPU / 256M |
| `tokenguard-frontend-doc` | nginx | 80 | **3223** | 0.5 CPU / 256M |

- Network: `tokenguard_public` (overlay)
- Update strategy: `order: start-first` (zero-downtime)
- Secrets: `jwt_secret`, `adm_db_cnn_string`, `saas_db_password`, `doc_db_pwd`
- Connection strings built at runtime by `docker-entrypoint.sh` from secrets

### 10.5 Database Strategy
- `DB_PROVIDER` env var selects provider: `postgresql` (default) or `sqlserver`
- `docker-entrypoint.sh` reads secrets from `/run/secrets/*` and builds connection string
- Auto-migration: `AUTO_MIGRATE=true` → runs `dotnet <dll> --Initialize` (dev only)
- Production migrations: manual or via DevOps orchestrator

---

## 11. CI/CD Pipeline

### 11.1 CI (`.github/workflows/ci.yml`)
- **Triggers:** Push to `main`/`master`, PRs targeting `main`/`master`
- **Runner:** `self-hosted` (all jobs)
- **Backend Jobs:** Builds ADM, SaaS, DOC separately using specific `.csproj` files
- **Tests:** xUnit (backend) + Vitest (frontend), coverage via Coverlet
- **E2E:** Playwright (chromium) against running backend
- **Security:** Scans for vulnerable NuGet packages
- **Badges:** Published to GitHub Gist on `main` push

### 11.2 CD (`.github/workflows/cd.yml`)
- **Triggers:** Tag push `v*` or manual `workflow_dispatch`
- **Builds 6 Docker images** → pushes to `ghcr.io/hermessilva/tokenguard-*`
- **Tagging:** `{sha7}` + `latest` + semver
- **Deploy:** SSH into staging, copies stack YAMLs, deploys Swarm stacks
- **Release:** Creates GitHub Release with auto-generated notes
- **Cleanup:** Prunes old workflow runs and GHCR images

### 11.3 Specialized CD Workflows

| Workflow | Purpose |
|----------|---------|
| `cd-adm.yml` | Deploy only ADM (backend + frontend) |
| `cd-saas.yml` | Deploy only SaaS (backend + frontend) |
| `cd-doc.yml` | Deploy only DOC (backend + frontend) |
| `cd-proxy.yml` | Deploy reverse proxy configuration |
| `cd-rabbitmq.yml` | Deploy RabbitMQ message broker |

### 11.4 CRITICAL: CI/CD Build Commands

**Backend — Use specific .csproj files, NEVER the solution:**
```bash
# CORRECT
dotnet restore Back/ADM/Launcher/Launcher.csproj
dotnet build Back/ADM/Launcher/Launcher.csproj -c Release --no-restore

dotnet restore Back/DOC/TokenGuard.DOC.Launcher/TokenGuard.DOC.Launcher.csproj
dotnet build Back/DOC/TokenGuard.DOC.Launcher/TokenGuard.DOC.Launcher.csproj -c Release --no-restore

# WRONG — includes C++ projects that fail on Linux
dotnet restore TokenGuard.slnx
```

**Frontend — No `npm ci` before Docker build:**
Docker handles its own dependency installation. The CD pipeline only needs to run `docker build`.

### 11.5 Version Management
- Backend: `Version.props` in each Launcher folder (MSBuild auto-increment)
- Frontend: `src/version.ts` (Node.js script increment)
- Docker images tagged with git SHA, not app version
- Release tags created via `deploy/create-release-tag.ps1`

---

## 12. Testing

### 12.1 Backend Tests

| Project | Type | Framework | Mocking |
|---------|------|-----------|---------|
| `TokenGuard.Test.Unit` | Unit | xUnit 2.9.3 | NSubstitute 5.3 |
| `TokenGuard.Test.Integration` | Integration | xUnit + `WebApplicationFactory` | SQLite in-memory |
| `TokenGuard.Test.Shared` | Utilities | xUnit | Moq 4.20, Bogus 5.6 |
| `TokenGuard.SaaS.Test.Unit` | Unit | xUnit 2.9.3 | NSubstitute 5.3 |

- Assertions: FluentAssertions 8.8
- Coverage: Coverlet (Cobertura format)
- Locale integrity: `LocaleKeyIntegrityTests` and `LocaleFilesIntegrityTests` validate translation completeness

### 12.2 Frontend Tests

| Tool | Scope | Location |
|------|-------|----------|
| Vitest | Unit tests | `Front/{App}/src/**/*.test.{ts,tsx}` |
| Playwright | E2E | `Front/{App}/Tests/` |

### 12.3 CRITICAL: Test Script Configuration
Frontend `package.json` scripts MUST include `--passWithNoTests`:
```json
"test": "vitest run --passWithNoTests"
```
This prevents CI failure when no test files exist yet.

---

## 13. Application Model Pattern (XModel)

Each backend "application" (screen/feature) follows this structure:

```
Apps/{Module}/
├── {Entity}ApplicationModel.cs     # Screen definition (views, forms, buttons, tours)
├── {Entity}Fields.cs               # Field definitions (columns, editors, validation)
├── {Entity}Keys.cs                 # Translation keys for this application
└── Translations/
    ├── {AppCode}.pt-BR.json
    ├── {AppCode}.en-001.json
    ├── {AppCode}.es-419.json
    └── {AppCode}.zh-TW.json
```

- **Registration:** Via `ApplicationRegistry` → seeded to DB by `ApplicationSeeder`
- **ADM apps:** `SYSx001`–`SYSx004` (in `Back/Shared/TokenGuard.Shared.Common/Apps/SYS/`)
- **SaaS apps:** `DOCx001` (in `Back/SaaS/TokenGuard.SaaS.Common/Apps/DOC/`)

---

## 14. DevOps Orchestrator (Pulumi)

A separate .NET 10 project at `Back/DevOps/` for multi-tenant infrastructure provisioning.

**Flow:** `POST /api/tenant/provision` → Clone GitOps repo → Create Pulumi stack → Generate nginx config → Push branch → Create GitHub PR → Human approval → `pulumi up`

**NOT part of the main CI/CD pipeline.** Built and deployed independently.

---

## 15. Local Development

### 15.1 Start Scripts

**Backend:**
```powershell
# Start specific backend
.\Back\start-adm.ps1    # Starts ADM backend on port 5005
.\Back\start-saas.ps1   # Starts SaaS backend on port 5115
.\Back\start-doc.ps1    # Starts DOC backend on port 5225
.\Back\start-all.ps1    # Starts all backends

# Or run directly
dotnet run --project Back/ADM/Launcher/Launcher.csproj
```

**Frontend:**
```powershell
# Start specific frontend
.\Front\start-adm.ps1   # Starts ADM frontend on port 3003
.\Front\start-saas.ps1  # Starts SaaS frontend on port 3113
.\Front\start-doc.ps1   # Starts DOC frontend on port 3223
.\Front\start-all.ps1   # Starts all frontends

# Or run directly
cd Front/ADM && npm run dev
```

### 15.2 Database Setup

1. Create PostgreSQL databases for each deployment
2. Configure connection strings in `appsettings.Development.json` or user secrets
3. Run migrations: `dotnet ef database update`

### 15.3 IDE Configuration

**Visual Studio 2022:**
- Open `TokenGuard.slnx`
- C++ projects visible but excluded from build (use Solution Configuration)

**VS Code:**
- Use workspace settings for multi-root editing
- Recommended extensions: C#, ESLint, Tailwind CSS IntelliSense

---

## 16. Common Mistakes to Avoid

| Mistake | Correct Approach |
|---------|-----------------|
| Reference `.csproj` for Shared projects | Use `.shproj` + `.projitems` |
| Use `./Front/ADM` as Docker context for frontends | Use `./Front` (needs Core sibling) |
| Run `dotnet restore` on `.slnx` in CI | Use specific `.csproj` paths (excludes C++ tools) |
| Add `npm ci` before `docker build` for frontends | Docker handles its own `npm ci` inside the build stage |
| Hardcode connection strings in images | Use `docker-entrypoint.sh` + Docker Swarm secrets |
| Copy `nginx.conf` without context-relative path | Frontend nginx.conf path is `{App}/nginx.conf` from `./Front` context |
| Add application-specific keys to frontend locale files | Application texts come from backend via XModel |
| Include C++ Tools in CI builds | Exclude — they're Windows-only, built separately |
| Use `vitest run` without `--passWithNoTests` | Always use `--passWithNoTests` in CI |
| Create tests that cross-reference backend ↔ frontend files | Each system is independent and testable in isolation |
| Use Shared.* projects for DOC | DOC uses Shared.Auth.* only |
| Forget to update all 3 backends when changing Core | Core changes affect ADM, SaaS, and DOC |
| Build C++ on Linux CI runner | C++ projects require Windows + Visual Studio |
