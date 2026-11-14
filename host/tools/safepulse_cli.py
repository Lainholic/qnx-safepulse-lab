#!/usr/bin/env python3
"""
safepulse-cli: SafePulse Host Command-Line Verification & Replay Tool
Enables offline verification, live QNX replay, and Markdown report generation.
"""

import sys
import os
import json
import math
import time
import argparse
import numpy as np

# Ensure root is in path
ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if ROOT_DIR not in sys.path:
    sys.path.insert(0, ROOT_DIR)

from host.replay.can_replay import VehicleTraceReader, replay_trace, autosar_crc32


def load_rules(rules_path: str) -> dict:
    """Loads rules from YAML or JSON file."""
    if not os.path.exists(rules_path):
        raise FileNotFoundError(f"Rules file not found: {rules_path}")

    # Simple YAML / JSON loader without external dependencies
    with open(rules_path, "r", encoding="utf-8") as f:
        content = f.read()

    try:
        return json.loads(content)
    except Exception:
        pass

    # Parse simple yaml subset
    rules = {"watchdog": {"starvation_limit_ms": 50}, "sensors": []}
    cur_sensor = None
    for line in content.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("- type:"):
            cur_sensor = {"type": int(line.split(":")[1].strip())}
            rules["sensors"].append(cur_sensor)
        elif cur_sensor is not None and ":" in line:
            k, v = line.split(":", 1)
            k = k.strip()
            v = v.strip().strip('"').strip("'")
            if v.replace(".", "", 1).replace("-", "", 1).isdigit():
                cur_sensor[k] = float(v) if "." in v else int(v)
            else:
                cur_sensor[k] = v
        elif line.startswith("starvation_limit_ms:"):
            rules["watchdog"]["starvation_limit_ms"] = int(float(line.split(":")[1].strip()))

    for s in rules.get("sensors", []):
        if "deadline_ms" not in s and "period_ms" in s:
            s["deadline_ms"] = float(s["period_ms"]) + float(s.get("deadline_tolerance_ms", 0.0))

    return rules


def _resolve_loaded_rules(rules: dict) -> dict:
    for s in rules.get("sensors", []):
        if "deadline_ms" not in s and "period_ms" in s:
            s["deadline_ms"] = float(s["period_ms"]) + float(s.get("deadline_tolerance_ms", 0.0))
    return rules


_FSM_NAMES = {
    0: "NORMAL",
    1: "DEGRADED",
    2: "LIMP_HOME",
    3: "EMERGENCY_STOP",
}

_BASE_CHANNEL_STARVATION_MS = {
    1: 120.0,  # STEERING
    2: 100.0,  # BRAKE
    3: 200.0,  # THROTTLE
}

_DEFAULT_SENSOR_NAMES = {
    1: "STEERING",
    2: "BRAKE",
    3: "THROTTLE",
    4: "IMU_YAW",
}


def evaluate_frames(frames: list, rules: dict) -> dict:
    """Evaluates ordered telemetry frames against physical, timing, E2E, plausibility, and starvation rules."""
    _resolve_loaded_rules(rules)
    sensor_map = {s["type"]: s for s in rules.get("sensors", [])}

    starv_ms = float(rules.get("watchdog", {}).get("starvation_limit_ms", 50.0))
    if starv_ms <= 0.0:
        starv_ms = 50.0
    scale = max(1.0, starv_ms / 50.0)
    channel_limits_ms = {ch: limit * scale for ch, limit in _BASE_CHANNEL_STARVATION_MS.items()}

    violations = []
    prev_frames = {}
    prev_alive = {}
    last_global_t = None
    channel_last_t = {}
    channel_rx_count = {1: 0, 2: 0, 3: 0}
    starved_channels = set()
    global_starved = False

    fsm_state = 0
    fault_entered_t = None
    consecutive_faults = 0
    starved_sensor = 0

    def step_fsm(vcode: str, timestamp: float, ch_hint: int = 0):
        nonlocal fsm_state, fault_entered_t, consecutive_faults, starved_sensor
        if fsm_state == 3:
            return
        # 1. Critical faults trigger immediate EMERGENCY_STOP (0x1004 CRC, 0x1005 Plausibility, 0x1008 Starvation)
        if any(tag in vcode for tag in ("0x1004", "0x1005", "0x1008")):
            fsm_state = 3
            if "0x1008" in vcode and ch_hint in (1, 2, 3):
                starved_sensor = ch_hint
            return

        # 2. Non-critical faults enter DEGRADED, and escalate to LIMP_HOME after 3.0s hold
        consecutive_faults += 1
        if fault_entered_t is None:
            fault_entered_t = timestamp

        if fsm_state == 0:
            fsm_state = 1  # DEGRADED
        elif fsm_state == 1 and (timestamp - fault_entered_t) >= 3.0:
            fsm_state = 2  # LIMP_HOME

    for idx, f in enumerate(frames):
        stype = int(f["type"])
        val = float(f["val_primary"])
        val_sec = float(f.get("val_secondary", 0.0))
        t = float(f["t"])

        srule = sensor_map.get(stype)
        sname = srule.get("name", _DEFAULT_SENSOR_NAMES.get(stype, str(stype))) if srule else _DEFAULT_SENSOR_NAMES.get(stype, str(stype))

        # -1. IEEE-754 Timestamp Finite & Monotonicity Check
        if not math.isfinite(t):
            violations.append({
                "frame": idx,
                "timestamp": t,
                "sensor": "BUS_GLOBAL",
                "violation": "OUT_OF_BOUNDS (0x1001)",
                "value": f"non-finite timestamp {t}",
                "limit": "finite IEEE-754 timestamp",
            })
            step_fsm("OUT_OF_BOUNDS (0x1001)", 0.0, 0)
            continue

        # 0. Timestamp non-monotonic / rollback check & duplicate timestamp check (0x1003 DEADLINE_MISS)
        if last_global_t is not None and t < last_global_t:
            violations.append({
                "frame": idx,
                "timestamp": t,
                "sensor": "BUS_GLOBAL",
                "violation": "DEADLINE_MISS (0x1003)",
                "value": f"rollback {t:.4f} < {last_global_t:.4f}",
                "limit": "strictly monotonic time",
            })
            step_fsm("DEADLINE_MISS (0x1003)", t, 0)

        # 0-b. Same-channel zero-time delta check (Duplicate or conflicting sensor frame at identical timestamp)
        if stype in channel_last_t and t == channel_last_t[stype]:
            prev_t, prev_v = prev_frames.get(stype, (t, val))
            if abs(val - prev_v) > 1e-4:
                violations.append({
                    "frame": idx,
                    "timestamp": t,
                    "sensor": sname,
                    "violation": "PLAUSIBILITY_CONTRADICTION (0x1005)",
                    "value": f"conflicting sample val={val} vs prev={prev_v} at same t={t:.4f}",
                    "limit": "unique value per channel per timestamp",
                })
                step_fsm("PLAUSIBILITY_CONTRADICTION (0x1005)", t, stype)

        # 1. Global bus starvation & per-ECU channel starvation check (0x1008)
        if last_global_t is not None and not global_starved:
            gap_ms = (t - last_global_t) * 1000.0
            if gap_ms >= starv_ms:
                global_starved = True
                violations.append({
                    "frame": idx,
                    "timestamp": t,
                    "sensor": "BUS_GLOBAL",
                    "violation": "STARVATION_WATCHDOG (0x1008)",
                    "value": f"{gap_ms:.1f} ms",
                    "limit": f"< {starv_ms:.0f} ms",
                })
                step_fsm("STARVATION_WATCHDOG (0x1008)", t, 0)

        for ch, ch_lim_ms in channel_limits_ms.items():
            if channel_rx_count[ch] >= 2 and ch not in starved_channels and ch in channel_last_t:
                ch_gap_ms = (t - channel_last_t[ch]) * 1000.0
                if ch_gap_ms >= ch_lim_ms:
                    starved_channels.add(ch)
                    ch_name = sensor_map.get(ch, {}).get("name", _DEFAULT_SENSOR_NAMES.get(ch, str(ch)))
                    violations.append({
                        "frame": idx,
                        "timestamp": t,
                        "sensor": f"{ch_name} (ECU_{ch})",
                        "violation": "STARVATION_WATCHDOG (0x1008)",
                        "value": f"{ch_gap_ms:.1f} ms",
                        "limit": f"< {ch_lim_ms:.0f} ms",
                    })
                    step_fsm("STARVATION_WATCHDOG (0x1008)", t, ch)

        last_global_t = t
        channel_last_t[stype] = t
        if stype in channel_rx_count:
            channel_rx_count[stype] += 1

        # 2. Optional wire CRC & AUTOSAR E2E Profile 1 sequence checks
        if f.get("crc_valid") is False:
            violations.append({
                "frame": idx,
                "timestamp": t,
                "sensor": sname,
                "violation": "CRC_CORRUPTED (0x1004)",
                "value": "CRC mismatch",
                "limit": "CRC32 valid",
            })
            step_fsm("CRC_CORRUPTED (0x1004)", t)

        if "alive_counter" in f:
            cnt = int(f["alive_counter"]) & 0x0F
            if stype in prev_alive:
                delta = (cnt - prev_alive[stype]) & 0x0F
                if delta == 0:
                    violations.append({
                        "frame": idx,
                        "timestamp": t,
                        "sensor": sname,
                        "violation": "E2E_ALIVE_REPEATED (0x1006)",
                        "value": f"cnt={cnt}",
                        "limit": "delta >= 1",
                    })
                    step_fsm("E2E_ALIVE_REPEATED (0x1006)", t)
                elif delta > 2:
                    violations.append({
                        "frame": idx,
                        "timestamp": t,
                        "sensor": sname,
                        "violation": "E2E_ALIVE_SEQ_LOSS (0x1007)",
                        "value": f"jump={delta}",
                        "limit": "delta <= 2",
                    })
                    step_fsm("E2E_ALIVE_SEQ_LOSS (0x1007)", t)
            prev_alive[stype] = cnt

        # 3. IEEE-754 NaN/Inf check (do not update prev_frames so finite history stays unpoisoned)
        if not math.isfinite(val) or not math.isfinite(val_sec):
            violations.append({
                "frame": idx,
                "timestamp": t,
                "sensor": sname,
                "violation": "OUT_OF_BOUNDS (0x1001)",
                "value": str(val if not math.isfinite(val) else val_sec),
                "limit": "finite IEEE-754",
            })
            step_fsm("OUT_OF_BOUNDS (0x1001)", t)
            continue

        if srule:
            # 4. Physical bounds check (0x1001)
            min_val = srule.get("min_val", -1e9)
            max_val = srule.get("max_val", 1e9)
            if val < min_val or val > max_val:
                violations.append({
                    "frame": idx,
                    "timestamp": t,
                    "sensor": sname,
                    "violation": "OUT_OF_BOUNDS (0x1001)",
                    "value": val,
                    "limit": f"[{min_val}, {max_val}]",
                })
                step_fsm("OUT_OF_BOUNDS (0x1001)", t)

            # 5. Periodic deadline miss check (0x1003) & Jerk rate-of-change check (0x1002)
            if stype in prev_frames:
                prev_t, prev_v = prev_frames[stype]
                dt = t - prev_t
                if dt > 1e-6:
                    deadline_ms = float(srule.get("deadline_ms", 0.0))
                    dt_ms = dt * 1000.0
                    if deadline_ms > 0.0 and dt_ms > deadline_ms:
                        violations.append({
                            "frame": idx,
                            "timestamp": t,
                            "sensor": sname,
                            "violation": "DEADLINE_MISS (0x1003)",
                            "value": f"{dt_ms:.1f} ms",
                            "limit": f"<= {deadline_ms:.1f} ms",
                        })
                        step_fsm("DEADLINE_MISS (0x1003)", t)

                    # Continuous Jerk rate check: Clamp dt to minimum physical resolution (1ms) to prevent evasion
                    effective_dt = max(dt, 0.001)
                    jerk = abs(val - prev_v) / effective_dt
                    max_jerk = srule.get("max_jerk", 1e9)
                    if jerk > max_jerk:
                        violations.append({
                            "frame": idx,
                            "timestamp": t,
                            "sensor": sname,
                            "violation": "JERK_EXCEEDED (0x1002)",
                            "value": f"{jerk:.2f}/s",
                            "limit": f"Max {max_jerk}/s",
                        })
                        step_fsm("JERK_EXCEEDED (0x1002)", t)

        # 6. Cross-sensor plausibility contradiction check (0x1005)
        if stype == 3 and val > 85.0 and val_sec < 1.0:
            violations.append({
                "frame": idx,
                "timestamp": t,
                "sensor": sname,
                "violation": "PLAUSIBILITY_CONTRADICTION (0x1005)",
                "value": f"throttle={val:.1f}%, speed={val_sec:.1f} km/h",
                "limit": "speed >= 1.0 when throttle > 85%",
            })
            step_fsm("PLAUSIBILITY_CONTRADICTION (0x1005)", t)

        prev_frames[stype] = (t, val)

    return {
        "total_frames": len(frames),
        "violations": violations,
        "fsm_state": fsm_state,
        "fsm_state_name": _FSM_NAMES.get(fsm_state, "UNKNOWN"),
        "starved_sensor": starved_sensor,
    }


def cmd_check(trace_path: str, rules_path: str, json_out_path: str = None):
    """Checks a trace file against rules and outputs violations."""
    print(f"==================================================")
    print(f" SafePulse Static Trace Verification Linter       ")
    print(f" Target Trace : {trace_path}")
    print(f" Rules Config : {rules_path}")
    print(f"==================================================")

    rules = load_rules(rules_path)
    reader = VehicleTraceReader(trace_path)
    print(f"[*] Loaded {len(reader.frames)} frames from trace.")

    res = evaluate_frames(reader.frames, rules)
    violations = res["violations"]

    # Save last check report for 'safepulse-cli report'
    last_report_file = json_out_path or os.environ.get(
        "SAFEPULSE_CLI_CHECK_OUT",
        os.path.join(ROOT_DIR, "_reports", "last_cli_check.json"),
    )
    os.makedirs(os.path.dirname(last_report_file), exist_ok=True)
    with open(last_report_file, "w", encoding="utf-8") as f:
        json.dump({
            "trace": trace_path,
            "rules": rules_path,
            "total_frames": res["total_frames"],
            "fsm_state": res["fsm_state"],
            "fsm_state_name": res["fsm_state_name"],
            "starved_sensor": res["starved_sensor"],
            "violations": violations,
            "timestamp": time.ctime()
        }, f, indent=2)

    if violations:
        print(f"\n[!] Detected {len(violations)} Safety Violations (FSM={res['fsm_state_name']}):")
        for v in violations[:10]:
            print(f"  - Frame {v['frame']} (T={v['timestamp']:.3f}s) | Sensor {v['sensor']} | {v['violation']} | Val={v['value']} vs Limit={v['limit']}")
        if len(violations) > 10:
            print(f"  ... and {len(violations) - 10} more.")
        return 1
    else:
        print("\n[+] Verification PASSED: 0 Violations Detected (FSM=NORMAL). Trace complies with ASIL-D rules.")
        return 0


def cmd_replay(trace_path: str, target: str, speed: float = 1.0):
    """Replays trace to QNX microkernel."""
    print(f"[*] Streaming trace {trace_path} to {target} at {speed}x speed...")
    success = replay_trace(trace_path=trace_path, target=target, speed=speed, loop=False)
    return 0 if success else 1


def cmd_report(output_path: str = None):
    """Generates Markdown report from last CLI inspection."""
    last_check_file = os.path.join(ROOT_DIR, "_reports", "last_cli_check.json")
    if not os.path.exists(last_check_file):
        print(f"[!] No prior inspection found at {last_check_file}. Run 'safepulse-cli check' first.")
        return 1

    with open(last_check_file, "r", encoding="utf-8") as f:
        data = json.load(f)

    if output_path is None:
        output_path = os.path.join(ROOT_DIR, "_reports", "safepulse_cli_report.md")

    md = f"""# SafePulse Trace Verification Report

- **검사 일시**: {data.get('timestamp')}
- **대상 트레이스**: `{data.get('trace')}`
- **규칙 사양**: `{data.get('rules')}`
- **검사 프레임 수**: {data.get('total_frames'):,} 프레임
- **감지된 위반 수**: {len(data.get('violations', []))} 건

---

## 1. 검사 결과 요약

| 항목 | 수치 | 상태 |
|---|---|---|
| **총 프레임** | {data.get('total_frames'):,} | 정상 수집 |
| **위반 건수** | {len(data.get('violations', []))} | {'FAIL' if data.get('violations') else 'PASS'} |

## 2. 위반 세부 목록

"""
    if data.get("violations"):
        md += "| 프레임 | 타임스탬프 | 센서 | 위반 유형 | 측정값 | 기준 한계 |\n|---|---|---|---|---|---|\n"
        for v in data.get("violations")[:50]:
            md += f"| {v['frame']} | {v['timestamp']:.3f}s | {v['sensor']} | {v['violation']} | `{v['value']}` | `{v['limit']}` |\n"
    else:
        md += "위반 사항 없음. 지정된 모든 ISO 26262 ASIL-D 물리적 한계 및 Jerk 기준을 완벽히 준수함.\n"

    with open(output_path, "w", encoding="utf-8") as f:
        f.write(md)

    print(f"[+] Markdown report written to: {output_path}")
    return 0


def main():
    parser = argparse.ArgumentParser(prog="safepulse-cli", description="SafePulse Automotive Safety Tooling CLI")
    subparsers = parser.add_subparsers(dest="command", required=True)

    # Subcommand: check
    check_p = subparsers.add_parser("check", help="Verify trace file against rules")
    check_p.add_argument("trace", help="Path to trace file (.json or comma2k19 dir)")
    check_p.add_argument("--rules", default=os.path.join(ROOT_DIR, "rules.yaml"), help="Path to rules.yaml/json")

    # Subcommand: replay
    replay_p = subparsers.add_parser("replay", help="Replay trace to QNX target")
    replay_p.add_argument("trace", help="Path to trace file (.json or comma2k19 dir)")
    replay_p.add_argument("--target", default="127.0.0.1:9876", help="Target QNX bridge IP:Port")
    replay_p.add_argument("--speed", type=float, default=1.0, help="Replay speed (1.0, 2.0, 10.0)")

    # Subcommand: report
    report_p = subparsers.add_parser("report", help="Generate Markdown report of last check")
    report_p.add_argument("--out", default=None, help="Output markdown path")

    args = parser.parse_args()

    if args.command == "check":
        sys.exit(cmd_check(args.trace, args.rules))
    elif args.command == "replay":
        sys.exit(cmd_replay(args.trace, args.target, args.speed))
    elif args.command == "report":
        sys.exit(cmd_report(args.out))


if __name__ == "__main__":
    main()
