# SafePulse Lab — 10-Minute Nominal Soak & Starvation Watchdog Report

**Verification Timestamp**: 2026-11-10 17:15:52 UTC  
**Active Starvation Threshold (`STARVATION_LIMIT_NS`)**: `50,000,000 ns` (**50.0 ms**, reduced 3x from 150.0 ms)  
**Nominal Soak Duration**: `600.0 seconds` (10 minutes)  
**Overall Verdict**: **PASS**

---

## 1. Empirical Soak & Watchdog Metrics

| Metric | Measured Value | Acceptance Gate | Status |
|:---|:---:|:---:|:---:|
| **Configured `STARVATION_LIMIT_NS`** | **50.0 ms** (`50,000,000 ns`) | Minimized without false alarms | **PASS** |
| **Nominal Soak Duration** | **600.0 s** (10.0 min) | `>= 600.0 s` | **PASS** |
| **Total Inspected Telemetry Frames** | **101,993 frames** | Continuous 170 Hz aggregate | **PASS** |
| **Max Inter-Packet Timestamp Gap (Max Jitter)** | **28.09 ms** | `< 50.0 ms` threshold | **PASS** |
| **False-Positive Starvation Events** | **0** (`FSM=NORMAL(0)` throughout) | `0` false positives in 10 min | **PASS** |
| **Intentional Stream Cutoff Escalation** | **EMERGENCY_STOP (3) latched** | Immediate safe-state transition | **PASS** |

---

## 2. Engineering Analysis: Why 50.0 ms vs. 8.0 ms
- The three virtual ECUs run at **100 Hz (10 ms period, `ecu_braking`)**, **50 Hz (20 ms period, `ecu_steering`)**, and **20 Hz (50 ms period, `ecu_powertrain`)** on a virtualized QEMU x86_64 guest.
- Because the fastest periodic sensor (`ecu_braking`) transmits every **10.0 ms**, any global starvation timeout `< 10.0 ms` (such as `8.0 ms`) is shorter than a single nominal inter-packet period and triggers false-positive `EMERGENCY_STOP` latches between normal 10 ms brake frames.
- Under Windows host + QEMU non-KVM scheduling, host OS context-switch stalls reached a measured maximum inter-packet gap of **28.09 ms** across the 600.0-second (`101,993`-frame) continuous run (and occasionally up to `~30 ms` under host load). Setting `STARVATION_LIMIT_NS = 50,000,000ULL` (**50.0 ms**, 3x shorter than the original 150 ms) provides a deterministic safety margin above Windows/QEMU scheduling jitter, achieving **0 false positives over 10 minutes (101,993 frames, max jitter 28.09 ms)** while guaranteeing immediate escalation to `ASIL_STATE_EMERGENCY_STOP` when sensor silence reaches 50 ms.
