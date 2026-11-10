import socket
import struct
import zlib
import time
import json
import threading
from http.server import HTTPServer, SimpleHTTPRequestHandler
import os
import sys

# Console output
LOG_FILE = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "_reports", "bridge_debug.log"))

# Wire format (Exact 64-byte QNX wire alignment):
# magic(I=4), seq_num(I=4), timestamp_ns(Q=8), task_id(I=4), sensor_type(H=2), status_flags(H=2),
# val_primary(d=8), val_secondary(d=8), safety_score(f=4), violation_code(I=4), reserved(12s=12), crc32(I=4)
PACKET_FORMAT = "<IIQIHHddfI12sI"
PACKET_SIZE = 64

CHAOS_FORMAT = "<IIII"
CHAOS_MAGIC = 0x4348414F

QNX_HOST = "127.0.0.1"
QNX_PORT = 9876
HTTP_PORT = 8765
BIND_HOST = "127.0.0.1"

latest_telemetry = {
    "status": "DISCONNECTED",
    "seq": 0,
    "steering": 0.0,
    "steering_vel": 0.0,
    "throttle": 0.0,
    "speed": 0.0,
    "safety_score": 100.0,
    "violation_code": 0,
    "violation_name": "NONE",
    "status_flags": 0,
    "uptime_s": 0.0,
    "total_packets": 0,
    "fsm_state": "NORMAL",
    "fsm_color": "#10b981",
    "sched_policy": "Target: SCHED_SPORADIC / FIFO (Pri 50)",
    "history": []
}

qnx_socket = None
socket_lock = threading.Lock()

def get_violation_name(code):
    if code == 0x00001001:
        return "OUT_OF_BOUNDS_LIMIT"
    elif code == 0x00001002:
        return "JERK_RATE_EXCEEDED"
    elif code == 0x00001003:
        return "DEADLINE_MISS_TIMEOUT"
    elif code == 0x00001004:
        return "CRC32_CORRUPTED"
    elif code == 0x00001005:
        return "SENSOR_PLAUSIBILITY_FAULT"
    elif code == 0x00001006:
        return "AUTOSAR_E2E_COUNTER_REPEATED"
    elif code == 0x00001007:
        return "AUTOSAR_E2E_SEQUENCE_LOSS"
    elif code == 0:
        return "NORMAL"
    return f"UNKNOWN_0x{code:X}"

def qnx_tcp_client_worker():
    global qnx_socket, latest_telemetry
    
    while True:
        try:
            print(f"[Bridge] Attempting connection to QNX RTOS on {QNX_HOST}:{QNX_PORT}...")
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.connect((QNX_HOST, QNX_PORT))
            with socket_lock:
                qnx_socket = s
            latest_telemetry["status"] = "CONNECTED"
            print("[Bridge] Connected successfully to QNX RTOS /dev/safepulse stream!")

            buf = bytearray()
            start_time = time.time()

            while True:
                data = s.recv(4096)
                if not data:
                    print("[Bridge] QNX connection closed.")
                    latest_telemetry["status"] = "DISCONNECTED"
                    time.sleep(1)
                    break
                buf.extend(data)

                while len(buf) >= PACKET_SIZE:
                    raw = bytes(buf[:PACKET_SIZE])
                    del buf[:PACKET_SIZE]

                    fields = struct.unpack(PACKET_FORMAT, raw)
                    magic, seq, ts, task_id, sensor_type, flags, v1, v2, score, viol, _, crc = fields

                    if magic != 0x53504C53:
                        continue

                    latest_telemetry["total_packets"] += 1
                    latest_telemetry["seq"] = seq
                    latest_telemetry["safety_score"] = round(score, 1)
                    latest_telemetry["violation_code"] = viol
                    latest_telemetry["violation_name"] = get_violation_name(viol)
                    latest_telemetry["status_flags"] = flags
                    latest_telemetry["uptime_s"] = round(time.time() - start_time, 1)

                    if "seen_sensors" not in latest_telemetry:
                        latest_telemetry["seen_sensors"] = {}
                    latest_telemetry["seen_sensors"][str(sensor_type)] = latest_telemetry["seen_sensors"].get(str(sensor_type), 0) + 1

                    # Update ISO 26262 ASIL-D Safe State FSM representation
                    if score <= 0.0 or viol in (0x1004, 0x1005):
                        latest_telemetry["fsm_state"] = "EMERGENCY_STOP"
                        latest_telemetry["fsm_color"] = "#ef4444"
                    elif score < 70.0 or viol == 0x1002:
                        latest_telemetry["fsm_state"] = "LIMP_HOME"
                        latest_telemetry["fsm_color"] = "#f97316"
                    elif score < 95.0 or viol == 0x1003:
                        latest_telemetry["fsm_state"] = "DEGRADED"
                        latest_telemetry["fsm_color"] = "#f59e0b"
                    else:
                        latest_telemetry["fsm_state"] = "NORMAL"
                        latest_telemetry["fsm_color"] = "#10b981"

                    if sensor_type == 1: # Steering
                        latest_telemetry["raw_v1"] = v1
                        latest_telemetry["steering"] = round(v1, 1)
                        latest_telemetry["steering_vel"] = round(v2, 1)
                        with open(LOG_FILE, "a", encoding="utf-8") as f_dbg:
                            f_dbg.write(f"[STEER DEBUG] task={task_id}, v1={v1}, v2={v2}, score={score}, viol=0x{viol:X}, seq={seq}\n")
                    elif sensor_type == 3: # Throttle / Speed
                        latest_telemetry["throttle"] = round(v1, 1)
                        latest_telemetry["speed"] = round(v2, 1)

                    # Append to ring history (1500 frames = 6+ seconds at 240Hz aggregate for Blackbox Scrubber)
                    latest_telemetry["history"].append({
                        "t": latest_telemetry["uptime_s"],
                        "seq": latest_telemetry["seq"],
                        "steering": latest_telemetry["steering"],
                        "throttle": latest_telemetry["throttle"],
                        "speed": latest_telemetry["speed"],
                        "score": latest_telemetry["safety_score"],
                        "viol": latest_telemetry["violation_name"],
                        "fsm": latest_telemetry["fsm_state"],
                        "fsm_color": latest_telemetry["fsm_color"]
                    })
                    if len(latest_telemetry["history"]) > 1500:
                        latest_telemetry["history"].pop(0)

        except Exception as e:
            latest_telemetry["status"] = "WAITING_FOR_QNX"
            with socket_lock:
                qnx_socket = None
            time.sleep(2)

def inject_chaos(fault_type, duration_ms):
    global qnx_socket
    with socket_lock:
        if qnx_socket is None:
            return False, "QNX not connected"
        try:
            checksum = CHAOS_MAGIC ^ fault_type ^ duration_ms
            cmd_bytes = struct.pack(CHAOS_FORMAT, CHAOS_MAGIC, fault_type, duration_ms, checksum)
            qnx_socket.sendall(cmd_bytes)
            return True, f"Injected fault type {fault_type}"
        except Exception as e:
            return False, str(e)

class SafePulseHTTPHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        dashboard_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "dashboard"))
        super().__init__(*args, directory=dashboard_dir, **kwargs)

    def do_GET(self):
        if self.path == "/api/telemetry":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(json.dumps(latest_telemetry).encode("utf-8"))
        else:
            super().do_GET()

    def do_POST(self):
        content_length = int(self.headers.get("Content-Length", 0))
        if content_length > 0:
            self.rfile.read(content_length)

        if self.path.startswith("/api/chaos/"):
            fault_str = self.path.split("/")[-1]
            try:
                fault_type = int(fault_str)
                ok, msg = inject_chaos(fault_type, 1000)
                self.send_response(200 if ok else 500)
                self.send_header("Content-Type", "application/json")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(json.dumps({"success": ok, "message": msg}).encode("utf-8"))
            except ValueError:
                self.send_response(400)
                self.end_headers()
        else:
            self.send_response(404)
            self.end_headers()

def run_server():
    # If stdout/stderr are detached or None, route to log file
    try:
        sys.stdout.flush()
    except Exception:
        log_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "_reports", "bridge_live.log"))
        sys.stdout = open(log_path, "a", buffering=1, encoding="utf-8")
        sys.stderr = sys.stdout

    t = threading.Thread(target=qnx_tcp_client_worker, daemon=True)
    t.start()

    HTTPServer.allow_reuse_address = True
    server = HTTPServer((BIND_HOST, HTTP_PORT), SafePulseHTTPHandler)
    print(f"==================================================")
    print(f" SafePulse Lab Host Dashboard Web Server Active! ")
    print(f" URL: http://{BIND_HOST}:{HTTP_PORT}")
    print(f"==================================================")
    server.serve_forever()

import atexit
atexit.register(lambda: print(f"[Bridge] Process {os.getpid()} exiting now!"))

if __name__ == "__main__":
    try:
        run_server()
    except BaseException as e:
        import traceback
        traceback.print_exc()
