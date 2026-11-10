# SafePulse Safety Linter Rules (ISO 26262 ASIL-D Targeted)

This document defines the inline deterministic safety rules evaluated inside the QNX Neutrino 8.0 `/dev/safepulse` resource manager (`guest/linter/linter_engine.c`) and the Safe State FSM (`guest/resmgr/safe_state_fsm.c`).

## 1. Inline Linter Rules (`linter_inspect_packet`)

| Rule ID | Violation Code | Name | Condition | Penalty / Flag |
|:---|:---:|:---|:---|:---|
| **Rule 1** | `0x00001001` | `VIOLATION_OUT_OF_BOUNDS` | Steering $\notin [-540.0^\circ, +540.0^\circ]$, Brake $\notin [0.0, 150.0]\text{ bar}$, or Throttle $\notin [0.0, 100.0]\%$ | $-50.0$ Score, `STATUS_FLAG_CRITICAL` |
| **Rule 2** | `0x00001002` | `VIOLATION_JERK_EXCEEDED` | Rate of change exceeds threshold ($\Delta t \ge 5\text{ ms}$): Steering $> 720^\circ/\text{s}$, Brake $> 300\text{ bar/s}$, Throttle $> 200\%/\text{s}$ | $-40.0$ Score, `STATUS_FLAG_CRITICAL` |
| **Rule 3** | `0x00001003` | `VIOLATION_DEADLINE_MISS` | Inter-arrival period $\Delta t > 80\text{ ms}$ (`LIMIT_DEADLINE_NS`) | $-30.0$ Score, `STATUS_FLAG_WARNING` |
| **Rule 4** | `0x00001004` | `VIOLATION_CRC_CORRUPTED` | Packet magic $\ne \text{0x53504C53}$, software CRC-32 (`0xEDB88320`) mismatch over bytes `0x00..0x3B`, or AUTOSAR E2E Profile 1 CRC-32 (`0xF4ACFB13`) mismatch (`autosar_e2e_protect_verify`) | Score $= 0.0$, `STATUS_FLAG_CRITICAL` |
| **Rule 5** | `0x00001005` | `VIOLATION_PLAUSIBILITY` | Sensor contradiction (Throttle $> 85.0\%$ while Speed $< 1.0\text{ km/h}$) or AUTOSAR E2E Data ID masquerade | $-35.0$ Score, `STATUS_FLAG_CRITICAL` |
| **Rule 6** | `0x00001006` | `VIOLATION_E2E_REPEATED` | AUTOSAR E2E Profile 1 4-bit Alive Counter repeated ($\Delta c = 0$, frozen sensor or replay attack) | $-40.0$ Score, `STATUS_FLAG_CRITICAL` |
| **Rule 7** | `0x00001007` | `VIOLATION_E2E_SEQ_LOSS` | AUTOSAR E2E Profile 1 4-bit Alive Counter jump exceeded ($\Delta c > 2$, dropped frames) | $-40.0$ Score, `STATUS_FLAG_WARNING` |

## 2. Safe State Finite State Machine (`safe_state_fsm.c`)

- **States**: `ASIL_STATE_NORMAL (0)` $\to$ `ASIL_STATE_DEGRADED (1)` $\to$ `ASIL_STATE_LIMP_HOME (2)` $\to$ `ASIL_STATE_EMERGENCY_STOP (3)`
- **Dual-Rail Redundancy**: Inverted shadow state (`~current_state`) and XOR integrity checksum (`0x5A5A5A5A`) checked on every step.
- **Immediate E-STOP Escalation**: Triggered immediately on `VIOLATION_CRC_CORRUPTED (0x1004)` or `VIOLATION_PLAUSIBILITY (0x1005)`.
- **FTTI Escalation**: Any degraded state persisting beyond $10\text{ ms}$ (`FTTI_THRESHOLD_NS`) without resolution escalates to `EMERGENCY_STOP`.
- **Packet Starvation Timeout (`STARVATION_LIMIT_NS`)**: I/O silence reaching $50\text{ ms}$ (`50,000,000 ns`, reduced 3x from $150\text{ ms}$) after initial packet reception immediately triggers `EMERGENCY_STOP`.
- **Actuator Clamping & Slew-Rate Limiting**:
  - `DEGRADED`: Max steering $\pm 180^\circ$, Max throttle $60\%$.
  - `LIMP_HOME`: Max steering $\pm 90^\circ$, Max throttle $30\%$, Min brake $5\text{ bar}$ (latched against downward recovery).
  - `EMERGENCY_STOP`: Max steering $0^\circ$, Max throttle $0\%$, Min brake $100\text{ bar}$ (permanently latched until operator reset).
  - Ramped slew rates: $250^\circ/\text{s}$ steering, $100\%/\text{s}$ throttle.
