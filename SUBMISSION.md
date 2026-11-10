# BlackBerry QNX SafePulse Lab
### ASIL-D Targeted Microkernel Telemetry Linter & Self-Healing Safe State Controller for QNX Neutrino 8.0

> **Official Demonstration Video**: [https://youtu.be/DGCus_hPJjc](https://youtu.be/DGCus_hPJjc) (YouTube, 1080p60)  
> *Demonstrating 7-stage startup BIST, 50Hz telemetry linting, software CRC-32 fault injection, 6.0s blackbox scrubber, and self-healing watcher recovery on QNX Neutrino 8.0.*

---

## 1. Executive Summary & Mission
Modern Software-Defined Vehicles (SDVs) demand microsecond-level deterministic safety verification across multi-ECU zonal architectures. A single sensor glitch, communication fault, or process crash in autonomous steering or braking can lead to severe failure.

**QNX SafePulse Lab** is a zero-copy, microkernel-native safety firewall and self-healing telemetry arbiter architected for **BlackBerry QNX Neutrino RTOS 8.0**. Operating directly at the `/dev/safepulse` Resource Manager layer (hardware MMU-isolated user-space service, protecting the microkernel core against process faults), SafePulse intercepts, validates, clamps, and journals real-time vehicle telemetry against **ISO 26262 ASIL-D** targeted safety rules. (An **AUTOSAR E2E Profile 1** validator is also implemented in library form and verified in standalone unit/WCET benchmarks, though not applied on the live `/dev/safepulse` packet path.)

When an unrecoverable fault or process crash occurs, SafePulse’s custom **`posix_spawn`-based Self-Healing Watcher daemon** (`safepulse_watcher.c`) achieves **5.0 ms mean crash-to-service-ready recovery across a 50-crash marathon (and 46.7 ms in the single-crash E2E gate, well within the 500ms automotive SLA) with 100.0% zero data loss**, backed by atomic lock-free shared memory and persistent FSM safe state latching.

---

## 2. Quantitative Engineering Benchmark (Empirical Evidence)

All metrics were empirically measured on **QNX SDP 8.0 (x86_64)** on QEMU 11.1.0 with hardware cycle profiling (`ClockCycles()`):

| Engineering Attribute | Target / Industry Standard | SafePulse Lab Measured | Safety Margin |
|:---|:---:|:---:|:---:|
| **Worst-Case Latency (WCET)** | `< 10,000 ns` (< 10 µs) | **0.97 µs** (p50) / **1.05 µs** (Mean) / **1.28 µs** (p99) | **87.2% ~ 90.3% Margin** |
| **ISO 26262 FTTI Budget (10ms)** | `< 10.0%` CPU Budget | **3.11%** of 10ms FTTI (Peak) | **96.89% Headroom** |
| **Memory Leakage across Bursts** | 0 bytes (Zero Growth) | **Heap Delta: 0 bytes** (`mallinfo`) | **100% Deterministic** |
| **Single-Crash E2E Recovery** | `< 500 ms` (Automotive SLA) | **46.7 ms** (`test_comprehensive_e2e.py`) | **10.7x Faster** |
| **50-Crash Multi-Fault Marathon** | `< 500 ms` / 0% Loss | **5.0 ms** (Mean) / **244.1 ms** (Worst), **0.00% Loss** | **2.0x ~ 100x Faster** |
| **Fault Signal Diversity** | Multi-Signal Resilience | **SIGSEGV, SIGFPE, SIGABRT, SIGILL** (4 Domains) | **50/50 Recovered** |
| **Anti-Jerk Actuator Slew Rate** | Anti-Rollover Clamping | **Ramped 250 deg/s Steering, 100%/s Throttle** | **Smooth Safe-Stop** |
| **Temporal FFI under 100% DoS** | 0 Deadline Misses | **0 Misses across 5,000 pkts (0.00%)** | **Target Met** |

---

## 3. Core Architecture & QNX Microkernel Implementation

```
 +-------------------------------------------------------------------------------+
 |                        HOST SYSTEM (Linux / Windows)                          |
 |  +-------------------------------------+  +--------------------------------+  |
 |  |    3D Digital Twin Web Dashboard    |  |   Autonomous E2E & Chaos Test  |  |
 |  |  (50Hz Telemetry & Estimated FSM)   |  |   Harness Suite (8/8 Gates)    |  |
 |  +------------------^------------------+  +----------------^---------------+  |
 +---------------------|--------------------------------------|------------------+
                       | VirtIO Network (Port 9876)           |
 +---------------------v--------------------------------------v------------------+
 |                     BLACKBERRY QNX NEUTRINO 8.0 MICROKERNEL                   |
 |                                                                               |
 |   [Multi-ECU Autonomous Nodes]                                                |
 |   +-----------------+  +-----------------+  +-----------------+               |
 |   |  ecu_steering   |  |   ecu_braking   |  | ecu_powertrain  |               |
 |   |  (50Hz / Pri21) |  | (100Hz / Pri22) |  |  (20Hz / Pri19) |               |
 |   +--------+--------+  +--------+--------+  +--------+--------+               |
 |            |                    |                    |                        |
 |            +-----------> [ POSIX write() ] <---------+                        |
 |                                 |                                             |
 |   [Microkernel Resource Manager /dev/safepulse]                               |
 |   +-----------------------------------------------------------------------+   |
 |   |  1. ISO 26262 5-Rule Deterministic Linter (Bounds, Jerk, Software CRC)|   |
 |   |  2. Dual-Rail Safe State FSM (Shadow State & 10ms FTTI Clamping)      |   |
 |   |  3. QNX Real-Time Scheduling (SCHED_SPORADIC / SCHED_FIFO Pri 50)     |   |
 |   |  * AUTOSAR E2E Profile 1 (0x1D / 0xF4ACFB13) provided as library      |   |
 |   +---------------------------------+-------------------------------------+   |
 |                                     | Zero-Copy Push                          |
 |   [POSIX Shared Memory /safepulse_ringbuf]                                    |
 |   +-----------------------------------------------------------------------+   |
 |   | 2MB Lock-Free SPSC Ringbuffer (32,768 packed 64-byte telemetry frames)|   |
 |   | Independent Life-Cycle: Survives Process Termination & SIGSEGV        |   |
 |   +---------------------------------^-------------------------------------+   |
 |                                     |                                         |
 |   [posix_spawn Self-Healing Watcher Daemon (safepulse_watcher)]               |
 |   +-----------------------------------------------------------------------+   |
 |   | Spawns and monitors /dev/safepulse via posix_spawn() & waitpid().     |   |
 |   | Re-attaches to existing SHM buffer -> Zero packet loss guaranteed.    |   |
 |   +-----------------------------------------------------------------------+   |
 +-------------------------------------------------------------------------------+
```

### 1. Software CRC-32 & AUTOSAR E2E Profile 1 Library
- **Live Wire Path**: Uses a deterministic software CRC-32 (`0xEDB88320` nibble lookup table in `guest/resmgr/ringbuffer.c`) over bytes `0x00..0x3B` of every 64-byte `safepulse_packet_t`.
- **AUTOSAR E2E Library (`guest/linter/autosar_e2e.c`)**: Implements table-driven CRC-8 (`0x1D` SAE J1850) and CRC-32 (`0xF4ACFB13`), 16-bit Data ID protection, and 4-bit modulo-16 rolling counter checks. Currently verified as a library in `test_autosar_e2e` and `benchmark_wcet` (not wired into the live `/dev/safepulse` I/O path).

### 2. Dual-Rail Safe State FSM & Starvation Watchdog
- 4-Tier safety escalation: `NORMAL` $\to$ `DEGRADED` $\to$ `LIMP_HOME` $\to$ `EMERGENCY_STOP`.
- **Dual-Rail Shadow State**: Maintains inverted shadow state registers (`~current_state`) and validates an XOR integrity checksum (`0x5A5A5A5A`) on every state transition.
- **10ms FTTI Escalation & 150ms Starvation Timeout**: Degraded states unresolved within 10ms (`FTTI_THRESHOLD_NS`) escalate to `EMERGENCY_STOP`. In addition, `safe_state_fsm_check_starvation()` monitors I/O silence and forces `EMERGENCY_STOP` if no sensor packet arrives for **150ms** (3x the nominal 50Hz period).
- **Actuator Safety Clamping**:
  - `EMERGENCY_STOP`: Steering clamped toward $0.0^\circ$ (ramped at $250^\circ/\text{s}$), Throttle clamped toward $0.0\%$ ($100\%/\text{s}$), Brake enforced to $\ge 100.0\text{ bar}$.

### 3. Safety Traceability Matrix

| Safety Goal (SG) | Target ASIL | Technical Safety Requirement (TSR) | Implementation Artefact | Verification |
|:---|:---:|:---|:---|:---:|
| **SG-01: Prevent Unintended Acceleration / Jerk** | ASIL D | Rate-of-change and physical boundary validation `< 10µs` | [`guest/linter/linter_engine.c`](guest/linter/linter_engine.c) | E2E Gate 3 & 6 (PASS) |
| **SG-02: Frame Integrity & E2E Library** | ASIL D | Software CRC-32 on live frames; AUTOSAR E2E library | [`guest/resmgr/ringbuffer.c`](guest/resmgr/ringbuffer.c), [`guest/linter/autosar_e2e.c`](guest/linter/autosar_e2e.c) | E2E Gate 5 & Unit Test (PASS) |
| **SG-03: Timely Transition to Safe State** | ASIL D | Escalate to Safe State within 10ms FTTI / 150ms starvation | [`guest/resmgr/safe_state_fsm.c`](guest/resmgr/safe_state_fsm.c) | E2E Gate 4 & Unit Test (PASS) |
| **SG-04: Resilience Against Transient RAM Faults** | ASIL D | Dual-rail inverted register checks on every step | [`guest/resmgr/safe_state_fsm.c`](guest/resmgr/safe_state_fsm.c) | BIST Stage 6 & WCET (PASS) |
| **SG-05: Self-Healing Driver Recovery** | ASIL D | Autonomous restart via `posix_spawn` watcher with zero data loss | [`guest/safepulse_watcher.c`](guest/safepulse_watcher.c) + POSIX SHM | E2E Gate 7 & 50-Crash Marathon |

### 4. QNX 8.0 Native Architecture & Code Traceability

| QNX 8.0 Native Capability | Source Implementation Reference | Engineering Guarantee & Verification |
|:---|:---|:---|
| **User-Space Resource Manager** | [`guest/resmgr/safepulse_resmgr.c`](guest/resmgr/safepulse_resmgr.c#L418-L438) | `dispatch_create()`, `resmgr_attach()` establishing `/dev/safepulse` in user space |
| **Lock-Free Atomic POSIX SHM** | [`guest/resmgr/safepulse_resmgr.c`](guest/resmgr/safepulse_resmgr.c#L353-L377) | `shm_open()` + `mmap()` on `/safepulse_ringbuf` with `stdatomic.h` |
| **Deterministic Priority Scheduling**| [`guest/resmgr/safepulse_resmgr.c`](guest/resmgr/safepulse_resmgr.c#L449-L474) | `SCHED_SPORADIC` / `SCHED_FIFO` enforcement at Priority 50 |
| **Nanosecond Hardware Timing** | [`guest/benchmark_wcet.c`](guest/benchmark_wcet.c#L93-L120) | `ClockCycles()` hardware profiling across 100,000 continuous frames |
| **Fault Injection & Signal Trap** | [`guest/resmgr/safepulse_resmgr.c`](guest/resmgr/safepulse_resmgr.c#L155-L170) | Induces SIGSEGV, SIGFPE, SIGABRT, SIGILL to verify `posix_spawn` watcher resilience |
| **6.0s Blackbox Scrubber** | [`host/dashboard/app.js`](host/dashboard/app.js#L66-L122) | Circular buffer scanner caching historical frames for post-fault replay |
| **7-Stage Diagnostic BIST** | [`guest/resmgr/safepulse_resmgr.c`](guest/resmgr/safepulse_resmgr.c#L226-L327) | Startup self-test of range, plausibility, software CRC-32, shadow state, and E-STOP latch |

### 5. Host Dashboard Telemetry & Estimation Notes
- The host bridge (`host/bridge/qnx_bridge.py`) unpacks the 64-byte telemetry stream from TCP port 9876 and serves the 3D cockpit on HTTP port 8765.
- Because the 64-byte wire packet carries `safety_score` and `violation_code` rather than the raw kernel `fsm_state` or `sched_policy` fields, the dashboard displays an **estimated FSM state** (`FSM (Est.)`) derived from the packet's score/violation and shows the **target scheduler configuration** (`Target: SCHED_SPORADIC / FIFO (Pri 50)`). On the QNX guest itself, the exact kernel scheduler policy and FSM state are exposed via `devctl(fd, DCMD_SAFEPULSE_GET_STATS, ...)`.

---

## 4. Directory Structure
```text
qnx-safepulse-lab/
├── guest/                     # QNX Native C11 Source Files
│   ├── Makefile               # QNX qcc Makefile (alongside build.bat and build.sh)
│   ├── include/               # System headers (safepulse.h, autosar_e2e.h, safe_state_fsm.h, ringbuffer.h)
│   ├── linter/                # Deterministic 5-rule linter & AUTOSAR E2E library
│   ├── resmgr/                # /dev/safepulse Resource Manager, 2MB Ringbuffer & Safe State FSM
│   ├── ecu/                   # Steering, Braking, Powertrain virtual ECUs
│   ├── benchmark_wcet.c       # 100k iteration nanosecond WCET profiler
│   ├── test_ffi_isolation.c   # Freedom from Interference benchmark under CPU/cache DoS
│   ├── test_durability_1m.c   # Zero-leak durability stress tool
│   ├── safepulse_watcher.c    # posix_spawn supervisor with E2E open() service-ready barrier
│   └── qemu_vm/               # QNX 8.0 SDP IFS image configuration & boot scripts
├── host/                      # Python Host Automation & Test Harnesses
│   ├── test_comprehensive_e2e.py # 8-Gate autonomous verification suite
│   ├── test_chaos_marathon.py    # 50-Crash continuous chaos marathon harness
│   ├── dashboard/             # Real-time Web Cockpit & 3D Digital Twin (index.html, app.js)
│   └── bridge/                # Host-to-Guest VirtIO TCP bridge (qnx_bridge.py)
├── specs/                     # Wire Protocol & Safety Rules Specifications
│   ├── protocol_v1.md         # 64-Byte Packet Format Specification
│   ├── safety_rules.md        # Linter & FSM Rules Specification
│   └── test_protocol_and_logic.py # Host-side Wire Protocol Unit Tests
├── _reports/                  # Empirical Test & Audit Artifacts
│   ├── chaos_marathon_report.md  # 50-crash survival evidence
│   └── final_evaluation_report.md # 8-gate comprehensive verification report
├── SUBMISSION.md              # Hackathon Technical Whitepaper
└── README.md                  # Quickstart & Replication Guide
```

---

## 5. How to Build & Run Autonomous Verification

### Prerequisites
- BlackBerry QNX Software Development Platform (SDP) 8.0 (`C:\qnx800` or `$QNX_SDP`)
- QEMU 11.1.0 (`qemu-system-x86_64`, `C:\Program Files\qemu` or `$QEMU_DIR`)
- Python 3.10+

### Step 1: Rebuild All QNX Native Binaries & IFS Image
```cmd
call C:\qnx800\qnxsdp-env.bat
guest\build.bat
cd guest\qemu_vm
mkqnximage --type=qemu --clean --force
cd ..\..
```

### Step 2: Run Autonomous 8-Gate Comprehensive E2E Verification
```cmd
python host/test_comprehensive_e2e.py
```

### Step 3: Run 50-Crash High-Availability Chaos Marathon
```cmd
python host/test_chaos_marathon.py
```

---

## 6. Regulatory Notice & Intellectual Property Notice
- **Trademark Notice**: BlackBerry, QNX, Neutrino, and Momentics are registered trademarks of BlackBerry Limited and/or its subsidiaries. This project is independently developed for the BlackBerry QNX Global Hackathon 2026.
- **Functional Safety Engineering Scope**: SafePulse Lab is designed as an ASIL-D-targeted functional safety architectural prototype and developer diagnostic suite. It demonstrates Freedom from Interference (FFI) and deterministic recovery concepts pursuant to ISO 26262-9:2018. It does not constitute a certified Safety Element out of Context (SEooC) until undergoing accredited third-party validation.
- **License**: Released under the Permissive MIT License. QNX SDP 8.0 toolchains and header files are proprietary to BlackBerry Limited and must be obtained directly via BlackBerry Developer Channels.
