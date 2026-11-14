import urllib.request
import urllib.error
import json
import time
import sys

BASE_URL = "http://127.0.0.1:8765"

def get_telemetry():
    req = urllib.request.Request(f"{BASE_URL}/api/telemetry")
    with urllib.request.urlopen(req, timeout=2.0) as resp:
        return json.loads(resp.read().decode())

def post_chaos(fault_type):
    url = f"{BASE_URL}/api/chaos/{fault_type}"
    req = urllib.request.Request(url, data=b"", method="POST")
    try:
        with urllib.request.urlopen(req, timeout=2.0) as resp:
            return resp.status, json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        body = e.read().decode()
        try:
            return e.code, json.loads(body)
        except Exception:
            return e.code, {"error": body}

def post_profile(prof_id):
    url = f"{BASE_URL}/api/profile/{prof_id}"
    req = urllib.request.Request(url, data=b"", method="POST")
    try:
        with urllib.request.urlopen(req, timeout=2.0) as resp:
            return resp.status, json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode())

print("=" * 60)
print(" BlackBerry QNX SafePulse Lab Live QEMU Verification")
print("=" * 60)

# 1. Baseline Telemetry Verification
t0 = get_telemetry()
print(f"[Check 1] Baseline Telemetry: Status={t0['status']}, FSM={t0['fsm_state']}, Packets={t0['total_packets']}, Speed={t0['speed']:.1f} km/h")
assert t0['status'] == "CONNECTED", f"Expected CONNECTED, got {t0['status']}"
assert t0['total_packets'] > 100, f"Expected packets > 100, got {t0['total_packets']}"
assert t0['fsm_state'] == "NORMAL", f"Expected NORMAL, got {t0['fsm_state']}"
print("   -> PASS: Baseline streaming healthy.")

# 2. Dynamic Rule Profile Switch (Passenger -> Truck -> Passenger)
status, data = post_profile(1)
print(f"[Check 2a] Profile Switch to TRUCK: HTTP {status}, Profile={data.get('profile')}, Watchdog={data.get('watchdog_ms')}ms")
assert status == 200 and data.get('profile') == "TRUCK" and data.get('watchdog_ms') == 100
time.sleep(0.5)

status, data = post_profile(0)
print(f"[Check 2b] Profile Switch to PASSENGER: HTTP {status}, Profile={data.get('profile')}, Watchdog={data.get('watchdog_ms')}ms")
assert status == 200 and data.get('profile') == "PASSENGER" and data.get('watchdog_ms') == 50
print("   -> PASS: Dynamic rule profile switching verified.")

# 3. Fault Injection: Starvation Watchdog (Fault 11) -> Autonomous Emergency Brake Injection -> Slew to 0
print("[Check 3] Injecting Fault 11 (Total ECU Bus Silence / Starvation)...")
status, data = post_chaos(11)
print(f"   -> Fault 11 Injected: HTTP {status}, Response={data}")

# Observe FSM transition to EMERGENCY_STOP and autonomous 100 bar brake injection
time.sleep(0.3)
t_starve = get_telemetry()
print(f"   -> State after starvation: FSM={t_starve['fsm_state']}, Violation={t_starve['violation_name']} (0x{t_starve['violation_code']:04X}), Speed={t_starve['speed']:.1f} km/h")
assert t_starve['fsm_state'] == "EMERGENCY_STOP", f"Expected EMERGENCY_STOP, got {t_starve['fsm_state']}"
assert t_starve['violation_code'] == 0x1008, f"Expected 0x1008 STARVATION, got 0x{t_starve['violation_code']:04X}"
print("   -> PASS: Kernel FSM escalated to EMERGENCY_STOP on ECU starvation (0x1008).")

# 4. Verify Reset Interlock (Braking in progress: Reset should be rejected)
if t_starve['speed'] > 0.05:
    status_rst, data_rst = post_chaos(0)
    print(f"[Check 4] Reset Attempt while Braking (Speed={t_starve['speed']:.1f} km/h): HTTP {status_rst}, locked={data_rst.get('locked')}, message='{data_rst.get('message')}'")
    assert status_rst == 409 or data_rst.get('locked') is True, "Expected Reset Interlock rejection while vehicle braking!"
    print("   -> PASS: Reset Interlock active (Reset rejected while vehicle in motion).")

# 5. Wait for Full Stop (< 0.05 km/h) and Reset to NORMAL
print("Waiting for vehicle deceleration to complete (0 km/h)...")
stopped = False
for _ in range(30):
    t_cur = get_telemetry()
    if t_cur['speed'] < 0.05:
        stopped = True
        print(f"   -> Vehicle reached full stop: Speed={t_cur['speed']:.2f} km/h")
        break
    time.sleep(0.2)

assert stopped, "Vehicle failed to decelerate to full stop within timeout!"

# Attempt Reset at full stop
status_rst2, data_rst2 = post_chaos(0)
print(f"[Check 5] Reset Attempt at Full Stop: HTTP {status_rst2}, success={data_rst2.get('success')}")
assert status_rst2 == 200 and data_rst2.get('success') is True, f"Reset failed at 0 km/h: {data_rst2}"

time.sleep(0.5)
t_restored = get_telemetry()
print(f"   -> Telemetry after Reset: FSM={t_restored['fsm_state']}, Violation={t_restored['violation_name']}, Speed={t_restored['speed']:.1f} km/h")
assert t_restored['fsm_state'] == "NORMAL", f"Expected NORMAL after reset, got {t_restored['fsm_state']}"
print("   -> PASS: Successful recovery to NORMAL state at 0 km/h.")

print("=" * 60)
print(" ALL LIVE QEMU VERIFICATION CHECKS PASSED (100%)!")
print("=" * 60)
