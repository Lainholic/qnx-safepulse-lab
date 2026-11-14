#!/usr/bin/env python3
"""
Unit verification suite for safepulse-cli trace linter engine.
Validates:
1. Nominal trace compliance (0 violations, FSM=NORMAL) on sample_trace.json
2. IEEE-754 NaN / Inf rejection as 0x1001 without poisoning jerk state
3. Physical bounds (0x1001) and jerk rate (0x1002) detection
4. Periodic deadline miss (0x1003) detection
5. Multi-sensor plausibility contradiction (0x1005) detection
6. AUTOSAR E2E sequence and wire CRC corruption detection (0x1004, 0x1006, 0x1007)
7. Global and per-ECU starvation watchdog detection (0x1008) with EMERGENCY_STOP
"""

import sys
import os
import math
import tempfile

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if ROOT_DIR not in sys.path:
    sys.path.insert(0, ROOT_DIR)

from host.tools.safepulse_cli import load_rules, evaluate_frames, cmd_check, cmd_report


def run_cli_tests():
    print("==================================================")
    print(" SafePulse Host CLI Trace Linter Verification     ")
    print("==================================================")

    rules_yaml = os.path.join(ROOT_DIR, "rules.yaml")
    rules_json = os.path.join(ROOT_DIR, "rules.json")
    truck_yaml = os.path.join(ROOT_DIR, "examples", "custom_rules", "truck_rules.yaml")
    sample_trace = os.path.join(ROOT_DIR, "examples", "sample_trace.json")

    # TEST 1: Nominal sample_trace.json compliance across all 3 rule specifications
    print("[TEST 1] Verifying sample_trace.json under rules.yaml/json/truck... ", end="")
    rc_yaml = cmd_check(sample_trace, rules_yaml)
    rc_json = cmd_check(sample_trace, rules_json)
    rc_truck = cmd_check(sample_trace, truck_yaml)
    assert rc_yaml == 0, f"rules.yaml failed: rc={rc_yaml}"
    assert rc_json == 0, f"rules.json failed: rc={rc_json}"
    assert rc_truck == 0, f"truck_rules.yaml failed: rc={rc_truck}"

    with tempfile.NamedTemporaryFile("w", delete=False, suffix=".md") as tmp_md:
        tmp_report = tmp_md.name
    try:
        assert cmd_report(tmp_report) == 0
        with open(tmp_report, "r", encoding="utf-8") as f:
            content = f.read()
            assert "SafePulse Trace Verification Report" in content
    finally:
        if os.path.exists(tmp_report):
            os.remove(tmp_report)
    print("PASS (0 violations across all 3 rule profiles)")

    rules = load_rules(rules_yaml)

    # TEST 2: IEEE-754 NaN / Inf rejection without poisoning jerk history
    print("[TEST 2] Verifying IEEE-754 NaN rejection & jerk isolation... ", end="")
    nan_frames = [
        {"type": 1, "val_primary": 10.0, "val_secondary": 0.0, "t": 1.000},
        {"type": 1, "val_primary": float("nan"), "val_secondary": 0.0, "t": 1.020},
        {"type": 1, "val_primary": 10.5, "val_secondary": 0.0, "t": 1.040},
    ]
    res_nan = evaluate_frames(nan_frames, rules)
    assert len(res_nan["violations"]) == 1
    assert "0x1001" in res_nan["violations"][0]["violation"]
    assert res_nan["violations"][0]["frame"] == 1
    print("PASS (trapped 0x1001 at frame 1, frame 2 jerk unpoisoned)")

    # TEST 3: Physical Bounds (0x1001) & Jerk Rate (0x1002)
    print("[TEST 3] Verifying Physical Bounds (0x1001) & Jerk (0x1002)... ", end="")
    bounds_jerk_frames = [
        {"type": 1, "val_primary": 0.0, "val_secondary": 0.0, "t": 2.000},
        {"type": 1, "val_primary": 600.0, "val_secondary": 0.0, "t": 2.020},  # > 540 deg bounds
        {"type": 2, "val_primary": 0.0, "val_secondary": 0.0, "t": 2.000},
        {"type": 2, "val_primary": 80.0, "val_secondary": 0.0, "t": 2.020},  # delta 80 bar in 20ms = 4000 bar/s > 500 bar/s jerk
    ]
    res_bj = evaluate_frames(bounds_jerk_frames, rules)
    v_codes = [v["violation"] for v in res_bj["violations"]]
    assert any("0x1001" in vc for vc in v_codes), f"Missing 0x1001 in {v_codes}"
    assert any("0x1002" in vc for vc in v_codes), f"Missing 0x1002 in {v_codes}"
    print("PASS (bounds & jerk detected)")

    # TEST 4: Periodic Deadline Miss (0x1003)
    print("[TEST 4] Verifying periodic deadline miss (0x1003)... ", end="")
    deadline_frames = [
        {"type": 1, "val_primary": 10.0, "val_secondary": 0.0, "t": 3.000},
        {"type": 2, "val_primary": 5.0, "val_secondary": 0.0, "t": 3.020},
        {"type": 2, "val_primary": 5.0, "val_secondary": 0.0, "t": 3.040},
        {"type": 1, "val_primary": 10.0, "val_secondary": 0.0, "t": 3.090},  # dt = 90ms > 80ms deadline
    ]
    res_dl = evaluate_frames(deadline_frames, rules)
    assert any("0x1003" in v["violation"] for v in res_dl["violations"])
    print("PASS (deadline miss detected)")

    # TEST 5: Plausibility Contradiction (0x1005)
    print("[TEST 5] Verifying multi-sensor plausibility (0x1005)... ", end="")
    plaus_frames = [
        {"type": 3, "val_primary": 95.0, "val_secondary": 0.0, "t": 4.000},  # 95% throttle, 0 km/h speed
    ]
    res_plaus = evaluate_frames(plaus_frames, rules)
    assert any("0x1005" in v["violation"] for v in res_plaus["violations"])
    assert res_plaus["fsm_state"] == 3  # Immediate EMERGENCY_STOP
    print("PASS (plausibility contradiction forced E-STOP)")

    # TEST 6: AUTOSAR E2E / CRC wire verification (0x1004, 0x1006, 0x1007)
    print("[TEST 6] Verifying E2E & wire CRC fault detection... ", end="")
    e2e_frames = [
        {"type": 1, "val_primary": 5.0, "val_secondary": 0.0, "t": 5.000, "alive_counter": 1, "crc_valid": True},
        {"type": 1, "val_primary": 5.1, "val_secondary": 0.0, "t": 5.020, "alive_counter": 1, "crc_valid": True},  # repeat (0x1006)
        {"type": 1, "val_primary": 5.2, "val_secondary": 0.0, "t": 5.040, "alive_counter": 6, "crc_valid": True},  # skip 5 (0x1007)
        {"type": 1, "val_primary": 5.3, "val_secondary": 0.0, "t": 5.060, "alive_counter": 7, "crc_valid": False}, # CRC fail (0x1004)
    ]
    res_e2e = evaluate_frames(e2e_frames, rules)
    e_codes = [v["violation"] for v in res_e2e["violations"]]
    assert any("0x1006" in ec for ec in e_codes), f"Missing 0x1006 in {e_codes}"
    assert any("0x1007" in ec for ec in e_codes), f"Missing 0x1007 in {e_codes}"
    assert any("0x1004" in ec for ec in e_codes), f"Missing 0x1004 in {e_codes}"
    print("PASS (0x1004, 0x1006, 0x1007 verified)")

    # TEST 7: Global & Per-ECU Starvation Watchdog (0x1008)
    print("[TEST 7] Verifying Global & Single-ECU Starvation Watchdog (0x1008)... ", end="")
    # Global bus silence: 60ms gap >= 50ms limit
    bus_silence_frames = [
        {"type": 1, "val_primary": 5.0, "val_secondary": 0.0, "t": 6.000},
        {"type": 1, "val_primary": 5.0, "val_secondary": 0.0, "t": 6.060},
    ]
    res_bus = evaluate_frames(bus_silence_frames, rules)
    assert any("0x1008" in v["violation"] and "BUS_GLOBAL" in v["sensor"] for v in res_bus["violations"])
    assert res_bus["fsm_state"] == 3

    # Single-ECU Steering silence: Steering stops at t=7.020 while Brake ticks every 20ms to t=7.160 (gap=140ms >= 120ms)
    ecu_silence_frames = [
        {"type": 1, "val_primary": 5.0, "val_secondary": 0.0, "t": 7.000},
        {"type": 1, "val_primary": 5.0, "val_secondary": 0.0, "t": 7.020},
        {"type": 2, "val_primary": 10.0, "val_secondary": 0.0, "t": 7.040},
        {"type": 2, "val_primary": 10.0, "val_secondary": 0.0, "t": 7.070},
        {"type": 2, "val_primary": 10.0, "val_secondary": 0.0, "t": 7.100},
        {"type": 2, "val_primary": 10.0, "val_secondary": 0.0, "t": 7.130},
        {"type": 2, "val_primary": 10.0, "val_secondary": 0.0, "t": 7.160},
    ]
    res_ecu = evaluate_frames(ecu_silence_frames, rules)
    assert any("0x1008" in v["violation"] and "STEERING" in v["sensor"] for v in res_ecu["violations"])
    assert res_ecu["fsm_state"] == 3
    assert res_ecu["starved_sensor"] == 1
    print("PASS (both global and single-ECU starvation detected with E-STOP)")

    print("==================================================")
    print(" All SafePulse CLI linter verification tests PASSED.")
    print("==================================================")
    return 0


if __name__ == "__main__":
    sys.exit(run_cli_tests())
