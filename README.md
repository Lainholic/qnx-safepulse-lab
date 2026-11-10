# BlackBerry QNX SafePulse Lab
### Real-Time Microkernel Safety Linter & Self-Healing Telemetry Suite

> **BlackBerry QNX Global Hackathon 2026 Entry**  
> **Target OS**: BlackBerry QNX Neutrino RTOS 8.0 (Microkernel x86_64) on QEMU 11.1.0  
> **Host Stack**: Python 3.10+, Native Win32 / POSIX Sockets, Three.js 3D Digital Twin (HTML5/Canvas)  
> **Empirical Benchmarks (QNX 8.0 SDP)**:
> - **WCET (Worst-Case Latency)**: **0.97 µs** (p50) / **1.05 µs** (Mean) / **1.28 µs** (p99), consumes **3.11% of 10ms FTTI** at peak
> - **Heap Growth**: **0 bytes (Deterministic Zero-Leak)** across 100,000+ burst frames
> - **Multi-Fault 50-Crash Marathon**: **100.0% Survival** across 4 signal domains (SIGSEGV, SIGFPE, SIGABRT, SIGILL), **0.00% Data Loss**, **5.0 ms Mean Recovery** (**46.7 ms** in single-crash E2E gate)
> - **Anti-Rollover Slew-Rate Limiting**: **250 deg/s Ramped Steering**, **100%/s Throttle**
> - **Microkernel Scheduling**: Configures **SCHED_SPORADIC (Pri 50/15, 3ms/10ms)** with **SCHED_FIFO (Pri 50)** fallback + `DCMD_SAFEPULSE_SYNC_EDGE`
> - **Verification Status**: **8/8 Automated E2E Gates PASS** ([`_reports/final_evaluation_report_2026-11-11.md`](_reports/final_evaluation_report_2026-11-11.md))
> - **Official Video Demonstration (YouTube)**: [https://youtu.be/DGCus_hPJjc](https://youtu.be/DGCus_hPJjc) (1m 34s, 1080p60)

---

## 1. Executive Summary

In automotive safety-critical systems (ISO 26262 ASIL-D targeted, drive-by-wire), an unhandled sensor discrepancy, out-of-bounds steering jerk, or process crash can lead to severe failure.

**QNX SafePulse Lab** leverages the architectural capabilities of the **BlackBerry QNX Neutrino RTOS 8.0 Microkernel**:
1. **Deterministic Microsecond Inline Linter**: Inspects every binary telemetry frame in **< 2 microseconds** directly inside the `/dev/safepulse` POSIX resource manager before admitting it to the ringbuffer.
2. **ISO 26262 ASIL-D Targeted Safety Rules & Real-Time AUTOSAR E2E Profile 1**: Bounds checking, angular jerk limit, periodic deadline monitoring, software CRC-32 (`0xEDB88320`) frame integrity, multi-sensor plausibility analysis, and **live inline AUTOSAR E2E Profile 1 verification (`autosar_e2e_protect_verify`)** enforcing CRC-32 (`0xF4ACFB13`), 16-bit Data ID anti-masquerade, and 4-bit modulo-16 rolling counter checks (catching 1-bit flips `0x1004`, counter jumps `0x1007`, and repeated counters `0x1006`) directly on the `/dev/safepulse` packet path.
3. **High-Availability Self-Healing (`posix_spawn` Watcher)**: When a severe memory or arithmetic fault (SIGSEGV, SIGFPE, SIGABRT, SIGILL) is induced in the resource manager, the custom `posix_spawn`-based **SafePulse Watcher** supervisor respawns the driver (measured **46.7 ms** in the single-crash E2E gate and **5.0 ms mean** across the 50-crash marathon) with **zero data loss**, preserving historical flight packets and latched FSM state in persistent kernel POSIX Shared Memory (`/safepulse_ringbuf`).
4. **Host 3D Digital Twin & Chaos Injection**: A web cockpit streaming live telemetry at 50Hz, enabling single-click fault injection and real-time visualization. Both the dashboard's **ASIL-D Safe State FSM badge** (`NORMAL` / `DEGRADED` / `LIMP_HOME` / `EMERGENCY_STOP`) and **Real-Time Scheduler badge** (`SCHED_SPORADIC (Pri 50)` or `SCHED_FIFO (Pri 50)`) are populated directly from authoritative QNX kernel values stamped into the telemetry packet (`reserved[8..11]`) and verified via `DCMD_SAFEPULSE_GET_STATS` (displaying `알 수 없음 (UNKNOWN)` when disconnected).
5. **Power-On Built-In Self-Test (BIST / POST)**: 7-stage pre-operational invariant suite verifying linter rules, software CRC-32 calculation, and FSM latching before `/dev/safepulse` is exposed.
6. **Blackbox Flight Recorder Scrubber**: Rolling historical buffer allowing engineers to pause and scrub backward frame-by-frame to inspect pre-fault dynamics.

---

## 2. System Architecture

```mermaid
flowchart TB
    subgraph Host["Host Operating System (Windows / Linux)"]
        UI["3D Vehicle Digital Twin & Dashboard (Three.js / Port 8765)"]
        PyBridge["Telemetry Bridge & HTTP Server (host/bridge/qnx_bridge.py)"]
        Harness["Autonomous E2E Verification Harness (host/test_comprehensive_e2e.py)"]
        
        UI <-->|HTTP / JSON Stream| PyBridge
        Harness -->|TCP 9876 Raw Sockets| PyBridge
    end

    subgraph QEMU["QEMU Virtualization Layer"]
        PortFwd["VirtIO Port Forwarding (127.0.0.1:9876 <-> Guest:9876)"]
    end

    subgraph QNX["BlackBerry QNX Neutrino RTOS 8.0"]
        direction TB
        subgraph SafePulse["SafePulse Suite (Native C / Zero-Dependency)"]
            DevNode["/dev/safepulse (Microkernel Resource Manager)"]
            RingBuf["2MB Zero-Copy Ringbuffer (POSIX SHM: /safepulse_ringbuf)"]
            Linter["Deterministic Safety Linter Engine (linter_engine.c)"]
            Watcher["SafePulse Watcher Daemon (posix_spawn Supervisor)"]
            Bridge["SafePulse Guest Bridge (VirtIO TCP 9876 Streamer)"]
        end

        Watcher -->|Supervises & Respawns via posix_spawn| DevNode
        DevNode -->|Inline Verification| Linter
        DevNode -->|Atomic Push| RingBuf
        Bridge -->|Non-blocking Read| RingBuf
        Bridge <==>|Bi-directional VirtIO Socket| PortFwd
    end

    PyBridge <==>|64-Byte Wire Protocol| PortFwd
```

---

## 3. Empirical Verification Matrix (8/8 PASS)

All assertions are verified autonomously via [`host/test_comprehensive_e2e.py`](host/test_comprehensive_e2e.py):

| Test Case | Safety Requirement | Measured Metric | Status |
|:---|:---|:---:|:---:|
| **QNX TCP Bridge Socket Connection** | VirtIO Network Transport | Connect on `127.0.0.1:9876` | **PASS** |
| **Baseline 50Hz Telemetry Integrity** | Periodic Transmission | 10/10 frames, Score=100.0, 0 Violations | **PASS** |
| **Rule 1: Physical Out-of-Bounds Detection** | Steering Limit (`±540.0°`) | Detected `0x1001` at `850.0°` (clamped to `180.0°`) | **PASS** |
| **Rule 3: Deterministic Deadline Miss** | Period Watchdog (`< 80ms`) | Detected `0x1003` at `120ms` freeze | **PASS** |
| **Rule 4: CRC-32 Frame Integrity** | Bus Bitflip Detection | Detected `0x1004` corrupted software CRC-32 | **PASS** |
| **Rule 5: Multi-Metric Plausibility** | Redundant Sensor Logic | Detected `0x1005` (95% throttle @ 0 km/h) | **PASS** |
| **Self-Healing Watcher: Respawn & Recovery** | High-Availability Recovery | **46.7 ms** E2E driver resurrection | **PASS** |
| **Post-Recovery Stream Continuity** | Zero Data Loss Guarantee | 5/5 frames resumed seamlessly in SHM | **PASS** |

---

## 4. 64-Byte Deterministic Wire Protocol

Fixed-size struct packed to 64 bytes (`#pragma pack(push, 1)`):

```c
typedef struct {
    uint32_t magic;           /* 0x00: 0x53504C53 ("SPLS") */
    uint32_t seq_num;         /* 0x04: Monotonic sequence counter */
    uint64_t timestamp_ns;    /* 0x08: High-precision monotonic nanoseconds */
    uint32_t task_id;         /* 0x10: Emitting QNX Task / PID */
    uint16_t sensor_type;     /* 0x14: 1=Steering, 2=Brake, 3=Throttle, 4=IMU */
    uint16_t status_flags;    /* 0x16: Health and recovery status bitmask */
    double   val_primary;     /* 0x18: Primary metric (e.g. angle deg, %) */
    double   val_secondary;   /* 0x20: Secondary metric (e.g. rate, speed) */
    float    safety_score;    /* 0x28: Evaluated Safety Index (0.0 ~ 100.0) */
    uint32_t violation_code;  /* 0x2C: Detected violation enum */
    uint8_t  reserved[12];    /* 0x30: Future expansion padding (zeroed) */
    uint32_t crc32;           /* 0x3C: Software CRC-32 of bytes 0x00..0x3B */
} safepulse_packet_t;
```

---

## 5. How to Build & Run

### Prerequisites & Environment
- **BlackBerry QNX SDP 8.0** (default path `C:\qnx800` or `$QNX_SDP`, toolchain `qcc -Vgcc_ntox86_64`)
- **QEMU 11.1.0** (`qemu-system-x86_64`, default path `C:\Program Files\qemu` or `$QEMU_DIR`)
- **Python 3.10+** (standard library only; no external pip packages required for runtime)

### Step 1: Build All 12 QNX Native Binaries
Activate the QNX SDP 8.0 environment and run `guest/build.bat` (Windows) or `make -C guest` (Linux/macOS):
```cmd
call C:\qnx800\qnxsdp-env.bat
guest\build.bat
```
*(Or via Make: `cd guest && make all`)*  
This compiles `safepulse_resmgr`, `safepulse_watcher`, `safepulse_bridge`, the 3 virtual ECUs (`ecu_steering`, `ecu_braking`, `ecu_powertrain`), and all 6 verification/benchmark binaries (`test_linter_qnx`, `test_resmgr_client`, `test_stress_1000hz`, `test_autosar_e2e`, `test_durability_1m`, `benchmark_wcet`, `test_ffi_isolation`) using `qcc -Vgcc_ntox86_64`.

### Step 2: Generate the QNX Boot Image (`ifs.bin` & `disk-qemu.vmdk`)
```cmd
cd guest\qemu_vm
mkqnximage --type=qemu --clean --force
cd ..\..
```
> **Note on Credentials & Keys**: Files under `guest/qemu_vm/local/misc_files/` (`qtd_private_key.pem`, `qtd_public_key.pem`, `ssh_host_*`, `shadow`) and `guest/qemu_vm/local/ssh-ident` are **automatically generated by `mkqnximage`** during image creation and are excluded from Git tracking via `.gitignore`.

### Step 3: Run Autonomous E2E & Chaos Verification Suites
1. **Host Protocol & Logic Unit Test** (runs without QEMU):
   ```cmd
   python specs/test_protocol_and_logic.py
   ```
2. **8-Gate Comprehensive E2E Verification** (boots QNX 8.0 on QEMU):
   ```cmd
   python host/test_comprehensive_e2e.py
   ```
   *Writes `_reports/final_evaluation_report.md`.*
3. **50-Crash Multi-Fault Chaos Marathon**:
   ```cmd
   python host/test_chaos_marathon.py
   ```
   *Writes `_reports/chaos_marathon_report.md`.*

### Step 4: Launch Live 3D Digital Twin & Dashboard
```cmd
python run_safepulse.py
```
*Boots QNX 8.0 in the background, starts the telemetry bridge on port 8765, and opens [http://127.0.0.1:8765](http://127.0.0.1:8765) in your default web browser (pass `--no-browser` to suppress browser launch).*

---

## 6. Project Directory Layout

```text
qnx-safepulse-lab/
├── guest/                     # QNX 8.0 C-Native Microkernel Components
│   ├── Makefile               # Native QNX qcc build rules (plus build.bat / build.sh)
│   ├── include/               # Protocol, Linter, FSM, and Devctl Headers
│   ├── resmgr/                # /dev/safepulse Resource Manager, 2MB SHM Ringbuffer & FSM
│   ├── linter/                # Inline Linter Engine & AUTOSAR E2E Library
│   ├── ecu/                   # Virtual ECUs (ecu_steering, ecu_braking, ecu_powertrain)
│   ├── safepulse_watcher.c    # posix_spawn-based Self-Healing Supervisor
│   ├── safepulse_bridge.c     # Guest VirtIO TCP 9876 Bridge & Chaos Handler
│   └── qemu_vm/               # mkqnximage Build Environment (keys/shadow auto-generated by mkqnximage)
├── host/                      # Host Tooling (Windows / Linux)
│   ├── bridge/qnx_bridge.py   # Packet unpacker & HTTP/REST Server
│   ├── dashboard/             # Three.js 3D Vehicle Digital Twin Cockpit (index.html, app.js)
│   ├── test_comprehensive_e2e.py # 8-Gate Autonomous Verification Suite
│   └── test_chaos_marathon.py # 50-Crash 4-Domain Chaos Marathon Suite
├── specs/                     # Protocol & Safety Rules Specifications
│   ├── protocol_v1.md         # 64-byte Wire Protocol Specification
│   ├── safety_rules.md        # 5-Rule Linter & Safe State FSM Rules
│   └── test_protocol_and_logic.py # Host-side Wire Protocol Unit Tests
├── _reports/                  # Empirical Verification Logs & Audit Reports
│   ├── final_evaluation_report.md # Autonomous Verification Matrix Report (8/8 Gates)
│   └── chaos_marathon_report.md   # 50-Crash 4-Domain Chaos Marathon Evidence
├── run_safepulse.py           # One-Click Background Launcher & Auto-Browser
├── SUBMISSION.md              # Hackathon Technical Whitepaper
└── README.md                  # Architecture & Replication Guide
```

---

## 7. QNX 8.0 Native Architecture & Code Traceability

| QNX 8.0 Native Capability | Source Implementation Reference | Engineering Guarantee & Verification |
|:---|:---|:---|
| **User-Space Resource Manager** | [`guest/resmgr/safepulse_resmgr.c`](guest/resmgr/safepulse_resmgr.c#L418-L438) | `dispatch_create()`, `resmgr_attach()` establishing `/dev/safepulse` in user space |
| **Lock-Free Atomic POSIX SHM** | [`guest/resmgr/safepulse_resmgr.c`](guest/resmgr/safepulse_resmgr.c#L353-L377) | `shm_open()` + `mmap()` on `/safepulse_ringbuf` with `stdatomic.h` |
| **Deterministic Priority Scheduling**| [`guest/resmgr/safepulse_resmgr.c`](guest/resmgr/safepulse_resmgr.c#L449-L474) | `SCHED_SPORADIC` / `SCHED_FIFO` configuration at Priority 50 |
| **Nanosecond Hardware Timing** | [`guest/benchmark_wcet.c`](guest/benchmark_wcet.c#L93-L120) | `ClockCycles()` hardware profiling across 100,000 continuous frames |
| **Fault Injection & Signal Trap** | [`guest/resmgr/safepulse_resmgr.c`](guest/resmgr/safepulse_resmgr.c#L155-L170) | Induces SIGSEGV, SIGFPE, SIGABRT, SIGILL to verify `posix_spawn` watcher resilience |
| **6.0s Blackbox Scrubber** | [`host/dashboard/app.js`](host/dashboard/app.js#L66-L122) | Circular buffer scanner caching historical frames for post-fault replay |
| **7-Stage Diagnostic BIST** | [`guest/resmgr/safepulse_resmgr.c`](guest/resmgr/safepulse_resmgr.c#L226-L327) | Startup self-test of range, plausibility, software CRC-32, shadow state, and E-STOP latch |

---

## 8. Regulatory Notice & Intellectual Property Notice
- **Trademark Notice**: BlackBerry, QNX, Neutrino, and Momentics are registered trademarks of BlackBerry Limited and/or its subsidiaries. This project is independently developed for the BlackBerry QNX Global Hackathon 2026.
- **Functional Safety Engineering Scope**: SafePulse Lab is designed as an ASIL-D-targeted functional safety architectural prototype and developer diagnostic suite. It demonstrates Freedom from Interference (FFI) and deterministic recovery concepts pursuant to ISO 26262-9:2018. It does not constitute a certified Safety Element out of Context (SEooC) until undergoing accredited third-party validation.
- **License**: Released under the Permissive MIT License. QNX SDP 8.0 toolchains and header files are proprietary to BlackBerry Limited and must be obtained directly via BlackBerry Developer Channels.
