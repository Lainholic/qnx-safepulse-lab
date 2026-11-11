#!/usr/bin/env python3
"""
safepulse-cli: SafePulse Host Command-Line Verification & Replay Tool
Enables offline verification, live QNX replay, and Markdown report generation.
"""

import sys
import os
import json
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
            rules["watchdog"]["starvation_limit_ms"] = int(line.split(":")[1].strip())
    return rules


def cmd_check(trace_path: str, rules_path: str):
    """Checks a trace file against rules and outputs violations."""
    print(f"==================================================")
    print(f" SafePulse Static Trace Verification Linter       ")
    print(f" Target Trace : {trace_path}")
    print(f" Rules Config : {rules_path}")
    print(f"==================================================")

    rules = load_rules(rules_path)
    sensor_map = {s["type"]: s for s in rules.get("sensors", [])}

    reader = VehicleTraceReader(trace_path)
    print(f"[*] Loaded {len(reader.frames)} frames from trace.")

    violations = []
    prev_frames = {}

    for idx, f in enumerate(reader.frames):
        stype = f["type"]
        val = f["val_primary"]
        t = f["t"]

        srule = sensor_map.get(stype)
        if srule:
            # 1. Physical limit check
            min_val = srule.get("min_val", -1e9)
            max_val = srule.get("max_val", 1e9)
            if val < min_val or val > max_val:
                violations.append({
                    "frame": idx,
                    "timestamp": t,
                    "sensor": srule.get("name", str(stype)),
                    "violation": "OUT_OF_BOUNDS (0x1001)",
                    "value": val,
                    "limit": f"[{min_val}, {max_val}]"
                })

            # 2. Jerk rate-of-change check
            if stype in prev_frames:
                prev_t, prev_v = prev_frames[stype]
                dt = t - prev_t
                if dt > 1e-6:
                    jerk = abs(val - prev_v) / dt
                    max_jerk = srule.get("max_jerk", 1e9)
                    if jerk > max_jerk:
                        violations.append({
                            "frame": idx,
                            "timestamp": t,
                            "sensor": srule.get("name", str(stype)),
                            "violation": "JERK_EXCEEDED (0x1002)",
                            "value": f"{jerk:.2f}/s",
                            "limit": f"Max {max_jerk}/s"
                        })

        prev_frames[stype] = (t, val)

    # Save last check report for 'safepulse-cli report'
    last_report_file = os.path.join(ROOT_DIR, "_reports", "last_cli_check.json")
    os.makedirs(os.path.dirname(last_report_file), exist_ok=True)
    with open(last_report_file, "w", encoding="utf-8") as f:
        json.dump({
            "trace": trace_path,
            "rules": rules_path,
            "total_frames": len(reader.frames),
            "violations": violations,
            "timestamp": time.ctime()
        }, f, indent=2)

    if violations:
        print(f"\n[!] Detected {len(violations)} Safety Violations:")
        for v in violations[:10]:
            print(f"  - Frame {v['frame']} (T={v['timestamp']:.3f}s) | Sensor {v['sensor']} | {v['violation']} | Val={v['value']} vs Limit={v['limit']}")
        if len(violations) > 10:
            print(f"  ... and {len(violations) - 10} more.")
        return 1
    else:
        print("\n[+] Verification PASSED: 0 Violations Detected. Trace complies with ASIL-D rules.")
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
