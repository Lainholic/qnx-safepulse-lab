# BlackBerry QNX SafePulse Lab
### ASIL-D Targeted Microkernel Telemetry Linter & Self-Healing Safe State Controller for QNX Neutrino 8.0

> **Official Demonstration Video**: [https://youtu.be/00Ga395L078](https://youtu.be/00Ga395L078) (YouTube, 1080p, Official English Narration)  
> *Demonstrating QNX 8.0 shell & 10/10 BIST nominal drive, real Toyota RAV4 CAN replay (Highway 280), dynamic truck YAML hot-reload, guided judge run & secpol type-id block, AUTOSAR E2E bitflip & 100-bar stop, and 4-domain crash marathon (31ms respawn) with blackbox flight scrubber on QNX Neutrino 8.0.*

---

## 1. Executive Summary & Mission
Modern Software-Defined Vehicles (SDVs) demand microsecond-level deterministic safety verification across multi-ECU zonal architectures. A single sensor glitch, communication fault, or process crash in autonomous steering or braking can lead to severe failure.

**QNX SafePulse Lab** is a zero-copy, microkernel-native safety firewall and self-healing telemetry arbiter architected for **BlackBerry QNX Neutrino RTOS 8.0**. Operating directly at the `/dev/safepulse` Resource Manager layer (hardware MMU-isolated user-space service, protecting the microkernel core against process faults), SafePulse intercepts, validates, clamps, and journals real-time vehicle telemetry against **ISO 26262 ASIL-D** targeted safety rules and **live inline AUTOSAR E2E Profile 1 protection (`autosar_e2e_protect_verify`)** on the real-time `/dev/safepulse` packet path.

When an unrecoverable fault or process crash occurs, SafePulse’s custom **`posix_spawn`-based Self-Healing Watcher daemon** (`safepulse_watcher.c`) achieves **47.8 ms best / 74.7 ms mean / 77.4 ms median (p50) / 94.6 ms p95 / 107.6 ms max crash-to-service-ready recovery across a 50-crash `--secpol=secure` marathon (and 62.5 ms host E2E / 36.395 ms watcher respawn with 622 SHM packets retained and 0 dropped in the single-crash E2E gate, well within the 500ms automotive SLA)**, backed by lock-free shared memory and seqlock + 64-bit timestamp CRC-protected FSM safe state persistence.

---

## 2. Quantitative Engineering Benchmark (Empirical Evidence)

All metrics were empirically measured on **QNX SDP 8.0 (x86_64)** on QEMU 11.1.0 with hardware cycle profiling (`ClockCycles()`):

| Engineering Attribute | Target / Industry Standard | SafePulse Lab Measured | Safety Margin |
|:---|:---:|:---:|:---:|
| **Worst-Case Latency (WCET)** | `< 10,000 ns` (< 10 µs) | **0.97 µs** (p50) / **1.05 µs** (Mean) / **1.28 µs** (p99) | **87.2% ~ 90.3% Margin** |
| **ISO 26262 FTTI Budget (10ms)** | `< 10.0%` CPU Budget | **3.11%** of 10ms FTTI (Peak) | **96.89% Headroom** |
| **Memory Leakage across Bursts** | 0 bytes (Zero Growth) | **Heap Delta: 0 bytes** (`mallinfo`) | **100% Deterministic** |
| **Single-Crash E2E Recovery** | `< 500 ms` (Automotive SLA) | **62.5 ms** E2E / **36.395 ms** Watcher, **622 SHM Retained (0 Dropped)** | **8.0x Faster** |
| **50-Crash Multi-Fault Marathon** | `< 500 ms` / 50 of 50 Recovered | **47.8 ms** (Best) / **74.7 ms** (Mean) / **77.4 ms** (p50) / **94.6 ms** (p95) / **107.6 ms** (Max), **50/50** (SHM loss not measured in chaos harness) | **4.6x ~ 10.4x Faster** |
| **Starvation Watchdog (`STARVATION_LIMIT_NS`)** | `< 50 ms` / 0 False Positives | **50.0 ms** (`50,000,000 ns`), **0 False Positives in 10-Min Soak** | **3x Faster than 150ms** |
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
 |  |  (~170Hz Telemetry & Kernel FSM)    |  |  Harness Suite (12/12 Gates)   |  |
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
 |   |  4. Live Inline AUTOSAR E2E Profile 1 (0xF4ACFB13 CRC & Alive Counter)|   |
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
 |   | Re-attaches existing SHM ringbuffer & seqlock/CRC FSM state.          |   |
 |   +-----------------------------------------------------------------------+   |
 +-------------------------------------------------------------------------------+
```

### 1. Software CRC-32 & Live Inline AUTOSAR E2E Profile 1 Protection
- **Live Wire Path**: Uses a deterministic software CRC-32 (`0xEDB88320` nibble lookup table in `guest/resmgr/ringbuffer.c`) over bytes `0x00..0x3B` of every 64-byte `safepulse_packet_t`, combined with **real-time inline AUTOSAR E2E Profile 1 verification** (`autosar_e2e_protect_verify()` in `guest/linter/linter_engine.c`).
- **AUTOSAR E2E Real-Time Enforcement (`guest/linter/autosar_e2e.c`)**: Implements table-driven CRC-8 (`0x1D` SAE J1850) and CRC-32 (`0xF4ACFB13`), 16-bit Data ID protection, and 4-bit modulo-16 rolling counter checks embedded in `pkt->reserved[0..7]`. Actively detects 1-bit E2E CRC corruption (`0x1004`), repeated alive counters (`0x1006`), and counter jump / sequence loss (`0x1007`) on the live `/dev/safepulse` I/O path.

### 2. Dual-Rail Safe State FSM, Two-Tier Starvation Watchdog & Dynamic Profiles
- 4-Tier safety escalation: `NORMAL` $\to$ `DEGRADED` $\to$ `LIMP_HOME` $\to$ `EMERGENCY_STOP`.
- **Dual-Rail Shadow State**: Maintains inverted shadow state registers (`~current_state`) and validates an XOR integrity checksum (`0x5A5A5A5A` with multiplicative state mixing) on every transition, backed by odd/even SHM **seqlock** and 64-bit timestamp CRC persistence across respawns.
- **Two-Tier Watchdog (Global + Per-ECU Independent)**: Alongside the global `50 ms` (Passenger) / `100 ms` (Truck) bus watchdog, `safe_state_fsm.c` tracks independent timeouts per safety ECU (`BRAKE 100 ms`, `STEERING 120 ms`, `THROTTLE 200 ms`, armed once `channel_rx_count[ch] >= 2`, scaling proportionally with active profile timeout). Any silenced single ECU (`Chaos 16` `SIGSTOP`) trips `EMERGENCY_STOP` even while sibling ECUs keep streaming, propagating `starved_sensor` through `status_flags` bits `4..5` and `TIMING` `reserved[1]`.
- **Staged Degradation, E-STOP Deceleration & `0 km/h` Reset Interlock**: Non-critical faults enter `DEGRADED` and tighten to `LIMP_HOME` if unresolved for 3s (`DEGRADED_HOLD_NS`), whereas critical CRC/plausibility/starvation faults (`0x1004`, `0x1005`, `0x1008`) escalate immediately to `EMERGENCY_STOP` with controlled `25.0 km/h/s` braking deceleration and Reset interlocked until `0.0 km/h`.
- **Dynamic Profile Hot-Reload**: `guest/linter/linter_engine.c` provides a unified JSON/YAML rule parser supporting runtime `DCMD_SAFEPULSE_LOAD_PROFILE (0x07)` / `DCMD_SAFEPULSE_LOAD_RULES (0x08)` (`SAFEPULSE_UID_BRIDGE`-restricted under `--secpol=secure`) to switch between `PASSENGER` (`rules.json`) and `TRUCK` (`examples/custom_rules/truck_rules.yaml`), preserving the active profile across FSM Reset. *(Note: `--secpol=secure` runtime metrics in Section 2 were measured on baseline `73a0d3b`; live QEMU re-verification of post-`4ee966b` per-ECU/profile/`_IO_NOTIFY`/`slog2` upgrades is pending.)*
- **Actuator Safety Clamping**:
  - `EMERGENCY_STOP`: Steering clamped toward $0.0^\circ$ (ramped at $250^\circ/\text{s}$), Throttle clamped toward $0.0\%$ ($100\%/\text{s}$), Brake enforced and vehicle decelerated smoothly at $25.0\text{ km/h/s}$.

### 3. Safety Traceability Matrix

| Safety Goal (SG) | Target ASIL | Technical Safety Requirement (TSR) | Implementation Artefact | Verification |
|:---|:---:|:---|:---|:---:|
| **SG-01: Prevent Unintended Acceleration / Jerk** | ASIL D | Rate-of-change, physical bounds, and `NaN`/`Inf` validation `< 10µs` | [`guest/linter/linter_engine.c`](guest/linter/linter_engine.c) | E2E Gate 3 & 6 (`73a0d3b`) + Offline Build |
| **SG-02: Frame Integrity & Live AUTOSAR E2E** | ASIL D | Software CRC-32 & live AUTOSAR E2E Profile 1 (`0xF4ACFB13` + Alive Counter) | [`guest/linter/linter_engine.c`](guest/linter/linter_engine.c), [`guest/linter/autosar_e2e.c`](guest/linter/autosar_e2e.c) | E2E Gates 5, 6, 7 (`73a0d3b`) + Spec Unit Test |
| **SG-03: Timely Transition to Safe State** | ASIL D | Global (`50/100 ms`) + per-ECU (`100/120/200 ms`) starvation, staged `DEGRADED`/`LIMP_HOME`, `25 km/h/s` E-STOP | [`guest/resmgr/safe_state_fsm.c`](guest/resmgr/safe_state_fsm.c) | Unit `Test 3.7` (`[UNVERIFIED — QEMU Pending]`) |
| **SG-04: Event-Driven IPC & Kernel Diagnostics** | ASIL D | QNX `_IO_NOTIFY` (`iofunc_notify`/`ionotify`), `slog2` (`slogger_t`), and `DCMD_SAFEPULSE_LOAD_PROFILE`/`RULES` | [`guest/resmgr/safepulse_resmgr.c`](guest/resmgr/safepulse_resmgr.c), [`guest/safepulse_bridge.c`](guest/safepulse_bridge.c) | `test_resmgr_client` `TEST 5` (`[UNVERIFIED — QEMU Pending]`) |
| **SG-05: Self-Healing Driver Recovery** | ASIL D | Autonomous `posix_spawn` restart + seqlock/64-bit CRC SHM restoration | [`guest/safepulse_watcher.c`](guest/safepulse_watcher.c) + POSIX SHM | 50-Crash Marathon (`73a0d3b`), Re-run Pending |

### 4. QNX 8.0 Native Architecture & Code Traceability

| QNX 8.0 Native Capability | Source Implementation Reference | Engineering Guarantee & Verification |
|:---|:---|:---|
| **User-Space Resource Manager** | [`guest/resmgr/safepulse_resmgr.c`](guest/resmgr/safepulse_resmgr.c#L418-L438) | `dispatch_create()`, `resmgr_attach()` establishing `/dev/safepulse` in user space |
| **Lock-Free Atomic POSIX SHM** | [`guest/resmgr/safepulse_resmgr.c`](guest/resmgr/safepulse_resmgr.c#L353-L377) | `shm_open()` + `mmap()` on `/safepulse_ringbuf` with `stdatomic.h` |
| **Deterministic Priority Scheduling**| [`guest/resmgr/safepulse_resmgr.c`](guest/resmgr/safepulse_resmgr.c#L449-L474) | `SCHED_SPORADIC` / `SCHED_FIFO` enforcement at Priority 50 |
| **Nanosecond Hardware Timing** | [`guest/benchmark_wcet.c`](guest/benchmark_wcet.c#L93-L120) | `ClockCycles()` hardware profiling across 100,000 continuous frames |
| **Fault Injection & Signal Trap** | [`guest/resmgr/safepulse_resmgr.c`](guest/resmgr/safepulse_resmgr.c#L155-L170) | Induces SIGSEGV, SIGFPE, SIGABRT, SIGILL to verify `posix_spawn` watcher resilience |
| **6.0s Blackbox Scrubber** | [`host/dashboard/app.js`](host/dashboard/app.js#L66-L122) | Circular buffer scanner caching historical frames for post-fault replay |
| **8-Stage Diagnostic BIST** | [`guest/resmgr/safepulse_resmgr.c`](guest/resmgr/safepulse_resmgr.c#L231-L394) | Startup self-test of range, plausibility, software CRC-32, AUTOSAR E2E Profile 1, shadow state, and E-STOP latch |

### 5. Host Dashboard Authoritative Kernel State Telemetry
- The host bridge (`host/bridge/qnx_bridge.py`) unpacks the 64-byte telemetry stream from TCP port 9876 and serves the 3D cockpit on HTTP port 8765.
- Both `safepulse_resmgr` (during inline packet admission) and `safepulse_bridge` (via `devctl(fd, DCMD_SAFEPULSE_GET_STATS, ...)`) stamp the **authoritative QNX kernel FSM state** (`NORMAL=0`, `DEGRADED=1`, `LIMP_HOME=2`, `EMERGENCY_STOP=3`) and **actual active kernel scheduler policy & priority** (`SCHED_SPORADIC (Pri 50)` or `SCHED_FIFO (Pri 50)`) into `pkt->reserved[8..11]` (with marker `0xA5`). The host bridge and dashboard render these exact kernel values without host-side estimation, and immediately transition to `알 수 없음 (UNKNOWN)` if the QNX link disconnects.

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
│   ├── test_comprehensive_e2e.py # 12-Gate autonomous verification suite
│   ├── test_chaos_marathon.py    # 50-Crash continuous chaos marathon harness
│   ├── test_starvation_soak.py   # 10-Minute nominal soak & 50ms starvation audit
│   ├── dashboard/             # Real-time Web Cockpit & 3D Digital Twin (index.html, app.js)
│   └── bridge/                # Host-to-Guest VirtIO TCP bridge (qnx_bridge.py)
├── specs/                     # Wire Protocol & Safety Rules Specifications
│   ├── protocol_v1.md         # 64-Byte Packet Format Specification
│   ├── safety_rules.md        # Linter & FSM Rules Specification
│   └── test_protocol_and_logic.py # Host-side Wire Protocol Unit Tests
├── _reports/                  # Empirical Test & Audit Artifacts
│   ├── chaos_marathon_report.md  # 50-crash survival evidence
│   ├── starvation_soak_report.md # 10-minute nominal soak & 50ms starvation evidence
│   └── final_evaluation_report.md # 12-gate comprehensive verification report
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

### Step 2: Run Autonomous 12-Gate Comprehensive E2E Verification
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
