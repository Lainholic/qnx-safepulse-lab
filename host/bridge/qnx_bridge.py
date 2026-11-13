import socket
import struct
import zlib
import time
import json
import threading
from http.server import HTTPServer, SimpleHTTPRequestHandler
import os
import sys
import subprocess
import re

# Console output
LOG_FILE = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "_reports", "bridge_debug.log"))

# Wire format (Exact 64-byte QNX wire alignment):
# magic(I=4), seq_num(I=4), timestamp_ns(Q=8), task_id(I=4), sensor_type(H=2), status_flags(H=2),
# val_primary(d=8), val_secondary(d=8), safety_score(f=4), violation_code(I=4), reserved(12s=12), crc32(I=4)
PACKET_FORMAT = "<IIQIHHddfI12sI"
PACKET_SIZE = 64

CHAOS_FORMAT = "<IIII"
CHAOS_MAGIC = 0x4348414F
ROGUE_RESULT_MAGIC = 0x524F4755

QNX_HOST = "127.0.0.1"
QNX_PORT = int(os.environ.get("SAFEPULSE_QNX_PORT", "9876"))
HTTP_PORT = int(os.environ.get("SAFEPULSE_HTTP_PORT", "8765"))
BIND_HOST = "127.0.0.1"

latest_telemetry = {
    "status": "DISCONNECTED",
    "seq": 0,
    "steering": 0.0,
    "steering_vel": 0.0,
    "throttle": 0.0,
    "speed": 0.0,
    "score": None,
    "safety_score": None,
    "violation_code": 0,
    "violation_name": "NONE",
    "status_flags": 0,
    "uptime_s": 0.0,
    "total_packets": 0,
    "fsm_state": "알 수 없음 (UNKNOWN)",
    "fsm_color": "#94a3b8",
    "sched_policy": "알 수 없음 (UNKNOWN)",
    "history": []
}

FSM_STATE_MAP = {
    0: ("NORMAL", "#10b981"),
    1: ("DEGRADED", "#f59e0b"),
    2: ("LIMP_HOME", "#f97316"),
    3: ("EMERGENCY_STOP", "#ef4444"),
}

SCHED_POLICY_MAP = {
    1: "SCHED_FIFO",
    2: "SCHED_RR",
    3: "SCHED_OTHER",
    4: "SCHED_SPORADIC",
}

qnx_socket = None
socket_lock = threading.Lock()
rogue_lock = threading.Lock()
rogue_event = threading.Event()
rogue_result = None
replay_lock = threading.Lock()
replay_state = {"state": "idle", "message": "", "packets": 0, "max_gap_ms": None}
replay_process = None
replay_control_socket = None

def replay_worker(listener, guest_socket, fault):
    global replay_process, replay_control_socket
    proc = None
    entered = False
    exited = False
    output = []
    error = None
    try:
        port = listener.getsockname()[1]
        root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
        cmd = [sys.executable, "-u", os.path.join(root, "host", "replay", "can_replay.py"),
               "--trace", os.path.join(root, "_work", "datasets"), "--target", f"127.0.0.1:{port}",
               "--duration", "10", "--inject-fault", fault]
        proc = subprocess.Popen(cmd, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, encoding="utf-8", errors="replace",
                                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        with replay_lock:
            replay_process = proc
            if replay_state["state"] == "stopping":
                proc.terminate()

        def collect_output():
            for line in proc.stdout:
                output.append(line)
                print("[Replay] " + line.rstrip(), flush=True)
        output_thread = threading.Thread(target=collect_output, daemon=True)
        output_thread.start()
        listener.settimeout(15)
        client, _ = listener.accept()
        listener.close()
        with client:
            with replay_lock:
                replay_control_socket = client
            buf = bytearray()
            while True:
                data = client.recv(4096)
                if not data:
                    break
                buf.extend(data)
                while len(buf) >= 4:
                    magic = struct.unpack_from("<I", buf)[0]
                    size = 16 if magic == CHAOS_MAGIC else 64 if magic == 0x53504C53 else 0
                    if not size:
                        raise ValueError("Invalid replay packet header")
                    if len(buf) < size:
                        break
                    packet = bytes(buf[:size])
                    del buf[:size]
                    with socket_lock:
                        if qnx_socket is not guest_socket:
                            raise ConnectionError("QNX connection changed during replay")
                        guest_socket.sendall(packet)
                    if magic == 0x53504C53:
                        with replay_lock:
                            replay_state["packets"] = int(replay_state.get("packets") or 0) + 1
                    if magic == CHAOS_MAGIC:
                        command = struct.unpack_from("<I", packet, 4)[0]
                        if command == 12:
                            entered = True
                            with replay_lock:
                                if replay_state["state"] != "stopping":
                                    replay_state.update(state="running", message="Real CAN replay running")
                        elif command == 13:
                            exited = True
            if buf:
                raise ValueError("Incomplete replay packet")
        # Resume virtual ECUs before waiting on the replay process.
        # Windows terminate() skips can_replay's own command 13, and
        # waiting on stdout first left the bus silent long enough to
        # trip STARVATION_WATCHDOG 0x1008.
        if entered and not exited:
            with socket_lock:
                if qnx_socket is guest_socket:
                    try:
                        guest_socket.sendall(struct.pack(CHAOS_FORMAT, CHAOS_MAGIC, 13, 0, CHAOS_MAGIC ^ 13))
                    except OSError:
                        pass
            exited = True
        proc.wait(timeout=5)
        output_thread.join(timeout=2)
        if proc.returncode != 0 and replay_state["state"] != "stopping":
            raise RuntimeError(f"Replay process exited with code {proc.returncode}")
    except Exception as exc:
        error = str(exc)
    finally:
        listener.close()
        if proc and proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=5)
        if entered and not exited:
            with socket_lock:
                if qnx_socket is guest_socket:
                    try:
                        guest_socket.sendall(struct.pack(CHAOS_FORMAT, CHAOS_MAGIC, 13, 0, CHAOS_MAGIC ^ 13))
                    except OSError:
                        pass
        text = "".join(output)
        count = re.search(r"Total replayed: (\d+) packets", text)
        gap = re.search(r"Max send gap: ([0-9.]+) ms", text)
        with replay_lock:
            stopped = replay_state["state"] == "stopping"
            parsed = int(count.group(1)) if count else None
            live = int(replay_state.get("packets") or 0)
            replay_state.update(state="error" if error and not stopped else "idle",
                                message=error if error and not stopped else ("Replay stopped" if stopped else "Replay finished"),
                                packets=parsed if parsed is not None else live,
                                max_gap_ms=float(gap.group(1)) if gap else None)
            replay_process = None
            replay_control_socket = None
        print(f"[Replay] Result: {json.dumps(replay_state)}", flush=True)

def request_replay_starvation():
    with replay_lock:
        if replay_state["state"] == "idle" or replay_state["state"] == "error":
            return None
        if replay_state["state"] != "running" or replay_control_socket is None:
            return False
        try:
            replay_control_socket.sendall(b"STARVATION\n")
            print("[Replay] Fault 11: requested 80 ms replay silence", flush=True)
            return True
        except OSError:
            return False

def start_replay(fault="none"):
    if fault not in ("none", "starvation"):
        return False, "Unsupported replay fault"
    with replay_lock:
        if replay_state["state"] in ("starting", "running", "stopping"):
            return False, "Replay is already active"
        with socket_lock:
            guest_socket = qnx_socket
        if guest_socket is None or latest_telemetry["status"] != "CONNECTED":
            return False, "QNX is not connected"
        global history_hold, history_prime
        latest_telemetry["history"] = []
        history_hold = False
        history_prime = set()
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        replay_state.update(state="starting", message="Starting real CAN replay", packets=0, max_gap_ms=None)
        threading.Thread(target=replay_worker, args=(listener, guest_socket, fault), daemon=True).start()
    return True, "Starting replay"

def stop_replay():
    with replay_lock:
        if replay_state["state"] not in ("starting", "running"):
            return False, "Replay is not active"
        replay_state.update(state="stopping", message="Stopping real CAN replay")
        proc = replay_process
    global history_hold
    history_hold = True
    with socket_lock:
        if qnx_socket is not None:
            try:
                qnx_socket.sendall(struct.pack(CHAOS_FORMAT, CHAOS_MAGIC, 13, 0, CHAOS_MAGIC ^ 13))
            except OSError:
                pass
    if proc and proc.poll() is None:
        proc.terminate()
    return True, "Stopping replay"

def get_violation_name(code):
    if code == 0x00001001:
        return "OUT_OF_BOUNDS 0x1001"
    elif code == 0x00001002:
        return "JERK_EXCEEDED 0x1002"
    elif code == 0x00001003:
        return "DEADLINE_MISS 0x1003"
    elif code == 0x00001004:
        return "CRC32_CORRUPTED 0x1004"
    elif code == 0x00001005:
        return "PLAUSIBILITY 0x1005"
    elif code == 0x00001006:
        return "E2E_REPEATED 0x1006"
    elif code == 0x00001007:
        return "E2E_SEQ_LOSS 0x1007"
    elif code == 0x00001008:
        return "STARVATION_WATCHDOG 0x1008"
    elif code == 0x00001009:
        return "CRASH_SIGSEGV 0x1009"
    elif code == 0x0000100A:
        return "CRASH_SIGFPE 0x100A"
    elif code == 0x0000100B:
        return "CRASH_SIGABRT 0x100B"
    elif code == 0x0000100C:
        return "CRASH_SIGILL 0x100C"
    elif code == 0:
        return "NONE"
    return f"UNKNOWN 0x{code:X}"

latched_violation_code = 0
latched_violation_name = "NONE"
crash_latched = False
fsm_latched = False
chaos_grace_until = 0.0
reset_grace_until = 0.0
history_hold = False
history_prime = set()

def qnx_tcp_client_worker():
    global qnx_socket, latest_telemetry, rogue_result, history_hold, history_prime
    
    while True:
        try:
            print(f"[Bridge] Attempting connection to QNX RTOS on {QNX_HOST}:{QNX_PORT}...")
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
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
                    latest_telemetry["score"] = None
                    latest_telemetry["safety_score"] = None
                    latest_telemetry["fsm_state"] = "알 수 없음 (UNKNOWN)"
                    latest_telemetry["fsm_color"] = "#94a3b8"
                    latest_telemetry["sched_policy"] = "알 수 없음 (UNKNOWN)"
                    time.sleep(1)
                    break
                buf.extend(data)

                while len(buf) >= PACKET_SIZE:
                    raw = bytes(buf[:PACKET_SIZE])
                    del buf[:PACKET_SIZE]

                    if struct.unpack_from("<I", raw)[0] == ROGUE_RESULT_MAGIC:
                        values = struct.unpack("<I6i36s", raw)
                        rogue_result = dict(zip(("open_write", "write", "open_reset", "reset", "signal", "launch"), values[1:7]))
                        rogue_event.set()
                        continue

                    fields = struct.unpack(PACKET_FORMAT, raw)
                    magic, seq, ts, task_id, sensor_type, flags, v1, v2, score, viol, reserved_bytes, crc = fields

                    if magic != 0x53504C53:
                        continue

                    # In ASIL-D Safe State EMERGENCY_STOP, score is 0.0; otherwise packet score
                    fsm_id = None
                    if len(reserved_bytes) == 12 and reserved_bytes[11] == 0xA5:
                        fsm_id = reserved_bytes[8]
                        sched_id = reserved_bytes[9]
                        sched_prio = reserved_bytes[10]
                        fsm_name, fsm_col = FSM_STATE_MAP.get(fsm_id, (f"UNKNOWN({fsm_id})", "#94a3b8"))
                        latest_telemetry["fsm_state"] = fsm_name
                        latest_telemetry["fsm_color"] = fsm_col
                        sched_name = SCHED_POLICY_MAP.get(sched_id, f"POLICY_{sched_id}")
                        latest_telemetry["sched_policy"] = f"{sched_name} (Pri {sched_prio})"
                    else:
                        latest_telemetry["fsm_state"] = "알 수 없음 (UNKNOWN)"
                        latest_telemetry["fsm_color"] = "#94a3b8"
                        latest_telemetry["sched_policy"] = "알 수 없음 (UNKNOWN)"

                    global latched_violation_code, latched_violation_name, crash_latched, fsm_latched, chaos_grace_until, reset_grace_until

                    already_estop_hold = crash_latched or fsm_latched
                    in_reset_grace = time.monotonic() < reset_grace_until
                    effective_score = round(score, 1)
                    if (fsm_id == 3 and not in_reset_grace) or crash_latched or fsm_latched: # EMERGENCY_STOP stays until Reset
                        effective_score = 0.0
                    elif fsm_id in (1, 2) and effective_score > 60.0:
                        effective_score = 60.0

                    # Once the guest has entered EMERGENCY_STOP, keep that state
                    # and the root-cause code until Reset. A later packet stamped
                    # NORMAL must not snap the cockpit back (CRC 0x1004 race).
                    # After Reset, ignore in-flight EMERGENCY_STOP packets briefly so they cannot re-latch.
                    if fsm_id == 3 and not in_reset_grace:
                        fsm_latched = True
                    if in_reset_grace and fsm_id == 3:
                        latest_telemetry["fsm_state"] = "NORMAL"
                        latest_telemetry["fsm_color"] = "#10b981"
                    if in_reset_grace:
                        # In-flight emergency packets after Reset must not re-arm the code the feed already cleared.
                        effective_viol = 0
                        effective_viol_name = "NONE"
                    elif crash_latched or fsm_latched:
                        latest_telemetry["fsm_state"] = "EMERGENCY_STOP"
                        latest_telemetry["fsm_color"] = "#ef4444"
                        if viol == 0x1008 and latched_violation_code != 0x1008:
                            latched_violation_code = 0x1008
                            latched_violation_name = "STARVATION_WATCHDOG 0x1008"
                        elif latched_violation_code == 0 and viol != 0:
                            latched_violation_code = viol
                            latched_violation_name = get_violation_name(viol)
                        effective_viol = latched_violation_code
                        effective_viol_name = latched_violation_name
                    elif fsm_id == 0: # In NORMAL
                        if viol == 0:
                            if time.monotonic() >= chaos_grace_until:
                                latched_violation_code = 0
                                latched_violation_name = "NONE"
                                effective_viol = 0
                                effective_viol_name = "NONE"
                            else:
                                effective_viol = latched_violation_code
                                effective_viol_name = latched_violation_name
                        else:
                            latched_violation_code = viol
                            latched_violation_name = get_violation_name(viol)
                            effective_viol = viol
                            effective_viol_name = latched_violation_name
                    else:
                        if viol != 0:
                            latched_violation_code = viol
                            latched_violation_name = get_violation_name(viol)
                        effective_viol = latched_violation_code if latched_violation_code != 0 else viol
                        effective_viol_name = latched_violation_name if latched_violation_code != 0 else get_violation_name(viol)

                    latest_telemetry["total_packets"] += 1
                    latest_telemetry["seq"] = seq
                    latest_telemetry["score"] = effective_score
                    latest_telemetry["safety_score"] = effective_score
                    latest_telemetry["violation_code"] = effective_viol
                    latest_telemetry["violation_name"] = effective_viol_name
                    latest_telemetry["status_flags"] = flags
                    latest_telemetry["uptime_s"] = round(time.time() - start_time, 1)

                    if "seen_sensors" not in latest_telemetry:
                        latest_telemetry["seen_sensors"] = {}
                    latest_telemetry["seen_sensors"][str(sensor_type)] = latest_telemetry["seen_sensors"].get(str(sensor_type), 0) + 1

                    if not already_estop_hold:
                        if sensor_type == 1: # Steering
                            latest_telemetry["raw_v1"] = v1
                            latest_telemetry["steering"] = round(v1, 1)
                            latest_telemetry["steering_vel"] = round(v2, 1)
                        elif sensor_type == 3: # Throttle / Speed
                            latest_telemetry["throttle"] = round(v1, 1)
                            latest_telemetry["speed"] = round(v2, 1)

                    # Keep about 10 seconds of blackbox history. A fixed 1500-frame cap shrank to ~7.5s during real CAN (~250Hz).
                    # After real CAN stops, hold that take until LIVE starts a fresh virtual window.
                    # After a source change, wait until steering and speed have both arrived so the first point is not a leftover from the previous source.
                    if not history_hold and not already_estop_hold:
                        history_prime.add(sensor_type)
                        if not ({1, 3} <= history_prime):
                            continue
                        latest_telemetry["history"].append({
                            "t": round(time.time() - start_time, 3),
                            "seq": latest_telemetry["seq"],
                            "steering": latest_telemetry["steering"],
                            "throttle": latest_telemetry["throttle"],
                            "speed": latest_telemetry["speed"],
                            "score": latest_telemetry["safety_score"],
                            "viol": latest_telemetry["violation_name"],
                            "fsm": latest_telemetry["fsm_state"],
                            "fsm_color": latest_telemetry["fsm_color"]
                        })
                        hist = latest_telemetry["history"]
                        while len(hist) > 1 and (
                            (hist[-1]["t"] - hist[0]["t"]) > 10.0
                            or len(hist) > 4000
                        ):
                            hist.pop(0)

        except Exception as e:
            latest_telemetry["status"] = "WAITING_FOR_QNX"
            latest_telemetry["score"] = None
            latest_telemetry["safety_score"] = None
            latest_telemetry["fsm_state"] = "알 수 없음 (UNKNOWN)"
            latest_telemetry["fsm_color"] = "#94a3b8"
            latest_telemetry["sched_policy"] = "알 수 없음 (UNKNOWN)"
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
        elif self.path == "/api/replay":
            with replay_lock:
                payload = dict(replay_state)
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(payload).encode("utf-8"))
        else:
            super().do_GET()

    def do_POST(self):
        global history_prime
        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length) if content_length > 0 else b""

        if self.path in ("/api/replay/start", "/api/replay/stop"):
            try:
                options = json.loads(body) if body else {}
                ok, message = start_replay(options.get("fault", "none")) if self.path.endswith("/start") else stop_replay()
            except (ValueError, AttributeError):
                ok, message = False, "Invalid replay request"
            self.send_response(200 if ok else 409)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"success": ok, "message": message}).encode("utf-8"))
        elif self.path == "/api/history/live":
            global history_hold, history_prime
            latest_telemetry["history"] = []
            history_hold = False
            history_prime = set()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"success": True, "message": "Live history resumed"}).encode("utf-8"))
        elif self.path == "/api/rogue-attack":
            global rogue_result
            with rogue_lock:
                rogue_result = None
                rogue_event.clear()
                ok, message = inject_chaos(14, 0)
                if ok and rogue_event.wait(5):
                    response = {"success": True, "result": rogue_result}
                else:
                    response = {"success": False, "message": message if not ok else "No result received from guest"}
            self.send_response(200 if response["success"] else 503)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(response).encode("utf-8"))
        elif self.path.startswith("/api/chaos/"):
            fault_str = self.path.split("/")[-1]
            try:
                fault_type = int(fault_str)
                if fault_type == 11:
                    replay_injected = request_replay_starvation()
                    if replay_injected is not None:
                        message = "Requested replay starvation" if replay_injected else "Replay not ready for starvation"
                        self.send_response(200 if replay_injected else 409)
                        self.send_header("Content-Type", "application/json")
                        self.end_headers()
                        self.wfile.write(json.dumps({"success": replay_injected, "message": message}).encode("utf-8"))
                        return
                global latched_violation_code, latched_violation_name, crash_latched, fsm_latched, chaos_grace_until, reset_grace_until
                # Do not pre-latch: the screen changes only when guest telemetry reports the fault
                dur_ms = 0 if fault_type == 0 else (80 if fault_type == 11 else 1000)
                ok, msg = inject_chaos(fault_type, dur_ms)
                if ok:
                    if fault_type == 0:
                        crash_latched = False
                        fsm_latched = False
                        chaos_grace_until = 0.0
                        reset_grace_until = time.monotonic() + 0.4
                        latched_violation_code = 0
                        latched_violation_name = "NONE"
                        latest_telemetry["violation_code"] = 0
                        latest_telemetry["violation_name"] = "NONE"
                        latest_telemetry["fsm_state"] = "NORMAL"
                        latest_telemetry["fsm_color"] = "#10b981"
                        latest_telemetry["score"] = 100.0
                        latest_telemetry["safety_score"] = 100.0
                        latest_telemetry["history"] = []
                        history_prime = set()
                    else:
                        chaos_grace_until = time.monotonic() + 0.15
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
