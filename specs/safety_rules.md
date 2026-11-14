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
- **Process & Scheduler Hardening**: `safepulse_resmgr` runs at QNX real-time priority (`SCHED_SPORADIC`/`SCHED_FIFO` prio `50`, `10 ms` `dispatch_timeout`, startup 8/8 BIST), supervised by `posix_spawn` `safepulse_watcher` with shared-memory `seqlock` + `CRC32` state restoration and `safepulse.secpol` MAC isolation.
