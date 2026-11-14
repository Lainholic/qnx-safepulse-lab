# QNX SafePulse Safety Rules, Watchdog & FSM Specification

## 1. Violation Taxonomy & Deterministic Linter Rules

`linter_engine.c` + `safe_state_fsm.c` enforce 7 real-time validation checks, E2E sequence verification, numeric `NaN`/`Inf` traps, and multi-tier watchdogs across every packet written to `/dev/safepulse`:

| Code | Identifier | Trigger Condition |
| :--- | :--- | :--- |
| `0x1001` | `OUT_OF_BOUNDS` | Primary/secondary sensor value outside active profile `min_val` .. `max_val` |
| `0x1002` | `JERK_EXCEEDED` | Time-normalized derivative exceeds active profile `max_jerk` |
| `0x1003` | `DEADLINE_MISS` | Packet inter-arrival latency exceeds sensor deadline (`80/80/100/80 ms`, tolerances `60/70/50/60 ms`) |
| `0x1004` | `CRC32_CORRUPTED` | IEEE 802.3 CRC-32 mismatch over header/payload bytes `0..59` |
| `0x1005` | `PLAUSIBILITY` | Cross-channel physical contradiction (e.g., high throttle at zero speed) |
| `0x1006` | `E2E_REPEATED` | Repeated E2E alive sequence counter |
| `0x1007` | `E2E_SEQ_LOSS` | Unexpected jump/gap in E2E alive sequence counter |
| `0x1008` | `STARVATION_WATCHDOG` | Global bus silence or independent per-ECU channel timeout exceeded |
| `0x1009..0x100C` | `CRASH_SIG*` | Daemon fatal signals (`SIGSEGV`, `SIGFPE`, `SIGABRT`, `SIGILL`) tracked across self-healing |

---

## 2. Two-Tier Starvation Watchdog (Global + Per-ECU Channel)

1. **Global Starvation Watchdog**:
   - Enforces maximum bus silence across all safety sensors.
   - Active limit depends on loaded vehicle profile: `50 ms` (`PASSENGER` in `rules.json`) or `100 ms` (`TRUCK` in `examples/custom_rules/truck_rules.yaml`).

2. **Independent Per-ECU Starvation Watchdog (`safe_state_fsm.c`)**:
   - Detects silent hang of a single ECU even while sibling ECUs continue transmitting (e.g., Steering ECU frozen via `Chaos 16` `SIGSTOP` while Brake and Throttle remain active).
   - Baseline nominal per-channel timeouts:
     - `BRAKE` (`sensor_type = 2`): **100 ms**
     - `STEERING` (`sensor_type = 1`): **120 ms**
     - `THROTTLE` (`sensor_type = 3`): **200 ms**
     - (`IMU_YAW = 4` excluded from per-channel trip)
   - **Arming Guard**: Channel `ch` arms only once `channel_rx_count[ch] >= 2`, preventing false trips during initial bring-up, single-frame SDK checks, or replay resets.
   - **Profile Scaling**: When the global limit `limit_ns > 50 ms` (such as `TRUCK = 100 ms`), per-channel thresholds scale proportionally by `limit_ns / 50 ms` (2x under Truck -> `200 / 240 / 400 ms`).
   - **Reporting**: Tripping records `starved_sensor_type`, sets bits `4..5` of `status_flags`, updates `safepulse_stats_t` and `TIME` `reserved[1]`, and forces `EMERGENCY_STOP`.

---

## 3. Staged ASIL-D Safe State FSM & Actuator Control

- **FSM States**:
  - `0 — NORMAL`: Healthy operation (`safety_score` un-capped, up to `100.0`).
  - `1 — DEGRADED`: Single or transient non-critical fault (`safety_score` capped at `<= 60.0`).
  - `2 — LIMP_HOME`: Escalated from `DEGRADED` after sustained degradation (`DEGRADED_HOLD_NS = 3 s`), retaining capped degraded control (`<= 60.0`).
  - `3 — EMERGENCY_STOP`: Latched immediately on critical faults (`0x1004`, `0x1005`, `0x1008`, `0x1009..0x100C`); `safety_score` drops to `0.0`.
- **Emergency Stop Vehicle Physics & Reset Interlock**:
  - Under `EMERGENCY_STOP`, actuator logic enforces controlled deceleration at `25.0 km/h/s` (`ESTOP_BRAKE_DECEL_KMH_S`) and steering slew centering.
  - Fault **Reset (`Chaos 0`) is interlocked** until vehicle speed reaches `0.0 km/h` (enforced by both guest FSM and host bridge HTTP `409`).

---

## 4. Dynamic Vehicle Rule Profiles & Security Isolation

- **Shared Rule Parser (`guest/linter/linter_engine.c`)**: Parses JSON (`rules.json`) and YAML (`rules.yaml`, `examples/custom_rules/truck_rules.yaml`), packaged in `/proc/boot/`.
- **Hot-Reload**: Triggered at runtime via `DCMD_SAFEPULSE_LOAD_PROFILE (0x07)` or `DCMD_SAFEPULSE_LOAD_RULES (0x08)`, restricted to `SAFEPULSE_UID_BRIDGE` when `g_secure_mode` is enabled.
- **Persistence**: The active rule profile (`PASSENGER` vs `TRUCK`) persists across FSM `Reset`.
- **Process & Scheduler Hardening**: `safepulse_resmgr` runs at QNX real-time priority (`SCHED_SPORADIC`/`SCHED_FIFO` prio `50`, `10 ms` `dispatch_timeout`, startup 10/10 BIST), supervised by `posix_spawn` `safepulse_watcher` (elevated to `SCHED_FIFO` Priority 55) with shared-memory `seqlock` + `CRC32` state restoration and `safepulse.secpol` MAC isolation.

---

## 5. ISO 26262 ASIL-D Power-On Built-In Self-Test (BIST)

Before registering `/dev/safepulse` and accepting sensor inputs, `safepulse_resmgr` executes a mandatory 10-stage offline self-test sequence. If any invariant fails, registration is denied immediately:

1. `[BIST 1/10]`: Linter state initialization & zero-counter invariant.
2. `[BIST 2/10]`: Nominal sensor validation & wire CRC-32 verification (Score = 100.0, Code = 0x0000).
3. `[BIST 3/10]`: Rule 1 physical bounds violation detection (`0x1001`).
4. `[BIST 4/10]`: Rule 5 multi-sensor plausibility contradiction detection (`0x1005`).
5. `[BIST 5/10]`: Rule 4 hardware CRC-32 corruption trap (`0x1004`).
6. `[BIST 6/10]`: Dual-rail SEU inverted shadow state bit-flip verification.
7. `[BIST 7/10]`: Unconditional ASIL-D upward escalation to latched `EMERGENCY_STOP`.
8. `[BIST 8/10]`: AUTOSAR E2E Profile 1 inline verification (nominal, CRC bitflip, alive repeat, alive skip).
9. `[BIST 9/10]`: IEEE-754 `NaN`/`Inf` input trap & per-ECU single-channel starvation trip.
10. `[BIST 10/10]`: Inline dynamic JSON and YAML rule parser syntax and threshold self-test.

## 6. High-Availability Watcher & Embedded C SDK

- **`safepulse_watcher` Supervisor**: Runs under real-time `SCHED_FIFO` at Priority 55 (strictly elevated above the resource manager's Priority 50 and virtual ECUs' Priorities 19–22) to prevent preemption during crash detection and respawn.
- **Embedded C SDK (`libsafepulse.a`)**:
  - `safepulse_inspect()`: Inline verification producing `safepulse_verdict_t` with `starved_sensor` attribution.
  - `safepulse_apply_actuator_limits()`: Clamps actuators to ASIL safe bounds, applies `ESTOP_BRAKE_DECEL_KMH_S` (25 km/h/s) deterministic deceleration ramp, and enforces minimum hydraulic brake pressure in emergency stops.
  - `safepulse_try_reset_fsm()`: Enforces the 0 km/h vehicle stop interlock before allowing fault latches to reset back to `NORMAL`.
- **Command-Line Linter (`safepulse-cli`)**:
  - Offline static verification (`check`), trace streaming replay (`replay`), and Markdown reporting (`report`).
  - Checks bounds (`0x1001`), jerk (`0x1002`), periodic deadlines (`0x1003`), CRC/E2E (`0x1004`/`0x1006`/`0x1007`), plausibility (`0x1005`), and global/per-ECU starvation (`0x1008`).
