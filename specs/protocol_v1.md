# QNX SafePulse Protocol Specification (`v2` — Wire & Kernel Interfaces)

## 1. Transport & Binary Framing

- **Guest TCP Multiplexed Stream**: `safepulse_bridge` serves port `9876` (connected to Host `host/bridge/qnx_bridge.py`, which exposes HTTP/REST telemetry on `127.0.0.1:8765`).
- **Endianness & Packing**: All binary frames use Little-Endian (`<`) wire ordering (`#pragma pack(1)` on QNX guest).
- **Frame Sizes**:
  - Primary Telemetry Packet (`safepulse_packet_t`): **64 bytes** (`<IIQIHHddfI12sI`)
  - Multiplexed Control/Diagnostic Frames: **64 bytes** (`ROGUE`, `LOAD`, `PROC`, `TIME`), dispatched by the leading `uint32 magic` field.
  - Chaos Fault-Injection Command (`chaos_command_t`): **16 bytes** (`<IIII`)

---

## 2. Primary Telemetry Packet (`safepulse_packet_t`, 64 Bytes)

Python struct format: `<IIQIHHddfI12sI`

| Offset | Size | Type | Field | Description |
| :--- | :--- | :--- | :--- | :--- |
| `0` | `4` | `uint32` | `magic` | `0x53504C53` (`"SPLS"`) |
| `4` | `4` | `uint32` | `seq_num` | Monotonic telemetry sequence number |
| `8` | `8` | `uint64` | `timestamp_ns` | Sensor / event timestamp in nanoseconds |
| `16` | `4` | `uint32` | `task_id` | Producer ECU / QNX task identifier |
| `20` | `2` | `uint16` | `sensor_type` | `1=STEERING`, `2=BRAKE`, `3=THROTTLE/SPEED`, `4=IMU_YAW` |
| `22` | `2` | `uint16` | `status_flags` | Status/starvation bitmask (see below) |
| `24` | `8` | `double` | `val_primary` | Primary value (e.g., Steering deg, Throttle %, Brake pressure) |
| `32` | `8` | `double` | `val_secondary` | Secondary value (e.g., Steering angular vel, Vehicle speed km/h) |
| `40` | `4` | `float` | `safety_score` | Computed safety health score (`0.0 .. 100.0`) |
| `44` | `4` | `uint32` | `violation_code` | Active fault / violation code (`0` = `NONE`, `0x1001..0x100C`) |
| `48` | `12` | `uint8[12]` | `reserved` | E2E metadata (`[0..7]`) + Kernel/FSM trailer (`[8..11]`) |
| `60` | `4` | `uint32` | `crc32` | IEEE 802.3 CRC-32 computed over bytes `0..59` |

### `status_flags` Layout
- Bits `4..5` (`(flags >> 4) & 0x03`): `starved_sensor` ECU channel (`1=STEERING`, `2=BRAKE`, `3=THROTTLE`) reported on `STARVATION_WATCHDOG (0x1008)`.
- Lower & upper bits (`0..3`, `8..15`): Linter / frame status flags.

### `reserved[12]` Layout (Offsets `48..59`)
- `reserved[0..1]`: 16-bit AUTOSAR E2E Data ID (little-endian, base `0x1000`, `SAFEPULSE_E2E_DATA_ID_BASE`)
- `reserved[2]`: 4-bit AUTOSAR E2E Alive Counter (`0..15`)
- `reserved[3]`: `0xE2` marker (`SAFEPULSE_E2E_MARKER`)
- `reserved[4..7]`: 32-bit AUTOSAR E2E CRC-32 (`0xF4ACFB13` over payload bytes `0x00..0x27`)
- `reserved[8]`: `fsm_id` (`0=NORMAL`, `1=DEGRADED`, `2=LIMP_HOME`, `3=EMERGENCY_STOP`).
- `reserved[9]`: `sched_id` (QNX scheduling policy identifier from `safepulse_resmgr`: `1=SCHED_FIFO`, `2=SCHED_RR`, `4=SCHED_SPORADIC`).
- `reserved[10]`: `sched_prio` (QNX thread priority, nominal `50`).
- `reserved[11]`: Sentinel byte `0xA5` (`SAFEPULSE_STATE_MARKER`) confirming the trailer was stamped by live guest kernel logic.

---

## 3. Multiplexed 64-Byte Control & Diagnostic Frames

- **`ROGUE` Result (`0x524F4755` `SAFEPULSE_ROGUE_RESULT_MAGIC`, `<I6i36s`, 64B)**: Security isolation verification output (`open_write`, `write`, `open_reset`, `reset`, `signal`, `launch` `errno`/status codes from `Chaos 14`).
- **`LOAD` Attack Result (`0x4C4F4144` `SAFEPULSE_LOAD_RESULT_MAGIC`, `<IIIIQIIIIIIi12s`, 64B)**: Low-priority QM CPU saturation run summary (`duration_ms`, `threads`, `load_priority`, `load_iterations`, `packets_inspected`, `violations`, `max_inspect_ns`, `max_gap_ns`, `fsm_state`, `safety_priority`, `error` from `Chaos 15`).
- **`PROC` Snapshot Entry (`0x50524F43` `SAFEPULSE_PROC_MAGIC`, `<IIHHiHBBBBBBQ24s8s`, 64B)**: Guest `/proc` process & thread table entry (`cycle`, `index`, `count`, `pid`, `threads`, `prio_max`, `prio_min`, `pol_max`, `state_main`, `pol_min`, `cpu_ns`, `name`).
- **`TIME` Timing Window (`0x54494D45` `SAFEPULSE_TIMING_MAGIC`, `<IIIIIIII32s`, 64B)**: Per-second worst-case timing telemetry (`window_ms`, `max_inspect_ns`, `max_gap_ns`, `inspected`, `fsm_state`, `load_active`, `restarted`).
  - `reserved[0..3]` at byte offsets `32..35` (`<BBH`):
    - byte `32`: `active_profile` (`0=PASSENGER`, `1=TRUCK`, `2=CUSTOM`)
    - byte `33`: `starved_sensor` (`0=NONE`, `1=STEERING`, `2=BRAKE`, `3=THROTTLE`)
    - bytes `34..35` (`uint16`): `starvation_limit_ms` (`50` Passenger, `100` Truck)

---

## 4. Chaos Command Frame (`chaos_command_t`, 16 Bytes, `<IIII`, `CHAOS_MAGIC = 0x4348414F`)

| Command (`fault_type`) | Action / Target Fault |
| :--- | :--- |
| `0` | Safety FSM Reset (blocked if `EMERGENCY_STOP` speed `> 0.0 km/h`; keeps active rule profile) |
| `1` | Steering Out-of-Bounds (`0x1001`) |
| `3`, `6`, `7`, `8` | `safepulse_resmgr` Crash Signal Injection (`SIGSEGV 0x1009`, `SIGFPE 0x100A`, `SIGABRT 0x100B`, `SIGILL 0x100C`) |
| `4` | AUTOSAR CRC-32 Bitflip (`0x1004`) |
| `5` | Cross-Sensor Plausibility Fault (`0x1005`) |
| `9` | E2E Alive Counter Skip (`0x1007`) |
| `10` | E2E Alive Counter Repeat (`0x1006`) |
| `11` | All-ECU Bus Silence Starvation (`80ms` Passenger / `130ms` Truck -> `0x1008`) |
| `14` | Rogue Process Privilege Probe (`POST /api/rogue-attack`) |
| `15` | Multi-Thread QM CPU Overload Test (`POST /api/load-attack`) |
| `16` | Single-ECU Steering `SIGSTOP` Silence (`150ms` Passenger / `260ms` Truck -> Per-Ch `STEERING 0x1008`) |
| `17` | Dynamic Vehicle Rule Profile Switch (`POST /api/profile/0` Passenger / `POST /api/profile/1` Truck) |

---

## 5. QNX Resource Manager (`/dev/safepulse`) & Kernel Integration

- **`devctl()` Control Surface (`0x01..0x08`)**:
  - `DCMD_SAFEPULSE_GET_STATS (0x01)`: Returns `safepulse_stats_t` (counts, violation, score, uptime, scheduler policy/priority, FSM state, worst latency/gap, `active_profile`, `starved_sensor`, `starvation_limit_ms`).
  - `DCMD_SAFEPULSE_RESET_STATS (0x02)`: Resets stats counters (blocked from untrusted processes under `g_secure_mode`).
  - `DCMD_SAFEPULSE_INJECT_CHAOS (0x03)`: Dispatches `chaos_command_t` fault trigger to resmgr.
  - `DCMD_SAFEPULSE_SYNC_EDGE (0x04)`: Synchronizes period/deadline edges.
  - `DCMD_SAFEPULSE_GET_PID (0x05)`: Queries current `safepulse_resmgr` `pid_t`.
  - `DCMD_SAFEPULSE_RESET_LATENCY (0x06)`: Resets per-window `max_inspect_ns` and `max_gap_ns`.
  - `DCMD_SAFEPULSE_LOAD_PROFILE (0x07)`: Hot-switches between `SAFEPULSE_PROFILE_PASSENGER`, `SAFEPULSE_PROFILE_TRUCK`, and `SAFEPULSE_PROFILE_CUSTOM` (restricted to `SAFEPULSE_UID_BRIDGE` under `g_secure_mode`).
  - `DCMD_SAFEPULSE_LOAD_RULES (0x08)`: Loads a custom rule blob (`safepulse_rules_blob_t`, restricted to `SAFEPULSE_UID_BRIDGE` under `g_secure_mode`).
- **Event-Driven Notification (`_IO_NOTIFY`)**:
  - `safepulse_resmgr` implements `iofunc_notify` / `safepulse_io_notify` / `safepulse_io_close_ocb` and triggers clients via `iofunc_notify_trigger`.
  - `safepulse_bridge` uses `ionotify(..., _NOTIFY_ACTION_POLLARM, _NOTIFY_COND_INPUT, &sev)` + `poll()` instead of blind 5 ms polling.
- **System Logging (`slog2`)**:
  - `safepulse_resmgr`, `safepulse_watcher`, and `safepulse_bridge` link `-lslog2` and record scheduler, startup BIST, FSM transitions, starvation trips, crash detection, and respawn recovery timings (`channel_connect:slogger_t` permitted in `guest/secpol/safepulse.secpol`).
