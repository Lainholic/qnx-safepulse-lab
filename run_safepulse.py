import subprocess
import time
import os
import sys
import socket
import webbrowser
import http.client
import json

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
reports_dir = os.path.join(BASE_DIR, "_reports")
os.makedirs(reports_dir, exist_ok=True)

HTTP_PORT = int(os.environ.get("SAFEPULSE_HTTP_PORT", "8765"))
QNX_PORT = int(os.environ.get("SAFEPULSE_QNX_PORT", "9876"))

def is_port_listening(port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(0.5)
        return s.connect_ex(("127.0.0.1", port)) == 0

print("==================================================")
print(" BlackBerry QNX SafePulse Lab Launching System...")
print("==================================================")

# 1. Start QEMU (QNX 8.0 Microkernel)
if not is_port_listening(QNX_PORT):
    print("[1/3] Starting QNX 8.0 Microkernel (QEMU)...")
    qemu_ps1 = os.path.join(BASE_DIR, "guest", "qemu_vm", "start_qemu_daemon.ps1")
    subprocess.run(["powershell", "-ExecutionPolicy", "Bypass", "-File", qemu_ps1], cwd=BASE_DIR)
    print("      -> QNX 8.0 Guest starting in background.")
else:
    print(f"[1/3] QNX 8.0 Guest already running on port {QNX_PORT}.")

# 2. Start Web Bridge
if not is_port_listening(HTTP_PORT):
    print("[2/3] Starting Telemetry Bridge & Dashboard Server...")
    bridge_py = os.path.join(BASE_DIR, "host", "bridge", "qnx_bridge.py")
    if os.name == "nt":
        pythonw = os.path.join(os.path.dirname(sys.executable), "pythonw.exe")
        if not os.path.isfile(pythonw):
            pythonw = sys.executable
        command_line = f'"{pythonw}" "{bridge_py}"'.replace("'", "''")
        cmd = ("$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create "
               f"-Arguments @{{CommandLine = '{command_line}'}}; Write-Output $r.ReturnValue")
        result = subprocess.run(["powershell", "-NoProfile", "-Command", cmd], capture_output=True, text=True)
        spawned = result.returncode == 0 and result.stdout.strip().splitlines()[-1:] == ["0"]
    else:
        subprocess.Popen([sys.executable, bridge_py], start_new_session=True)
        spawned = True
    if not spawned:
        print("ERROR: Could not start the dashboard bridge process.", file=sys.stderr)
        sys.exit(1)
    print(f"      -> Bridge daemon spawned on port {HTTP_PORT}.")
else:
    print(f"[2/3] Bridge already listening on port {HTTP_PORT}.")

# 3. Wait for a successful dashboard HTTP response and live QNX stream
print("[3/3] Synchronizing Dashboard link & QNX RTOS live stream...", flush=True)
dashboard_url = f"http://127.0.0.1:{HTTP_PORT}"
for attempt in range(25):
    connection = http.client.HTTPConnection("127.0.0.1", HTTP_PORT, timeout=0.5)
    try:
        connection.request("GET", "/")
        response = connection.getresponse()
        if 200 <= response.status < 300:
            print(f"      -> Dashboard HTTP {response.status} received.", flush=True)
            break
    except (OSError, http.client.HTTPException):
        pass
    finally:
        connection.close()
    time.sleep(1.0)
else:
    print(f"ERROR: Dashboard did not respond successfully at {dashboard_url}.", file=sys.stderr, flush=True)
    sys.exit(1)

# Wait for guest RTOS telemetry link to become CONNECTED
print("      -> Waiting for QNX 8.0 RTOS LIVE LINK...", flush=True)
for attempt in range(60):
    connection = http.client.HTTPConnection("127.0.0.1", HTTP_PORT, timeout=0.5)
    try:
        connection.request("GET", "/api/telemetry")
        response = connection.getresponse()
        if 200 <= response.status < 300:
            payload = json.loads(response.read().decode("utf-8"))
            if payload.get("status") == "CONNECTED":
                print("      -> QNX 8.0 RTOS LIVE LINK connected.", flush=True)
                break
    except (OSError, http.client.HTTPException, ValueError):
        pass
    finally:
        connection.close()
    time.sleep(1.0)
else:
    print(f"ERROR: QNX 8.0 RTOS telemetry link failed to connect within timeout.", file=sys.stderr, flush=True)
    sys.exit(1)

print("==================================================", flush=True)
print(" QNX SafePulse Lab READY!", flush=True)
print(f" Dashboard URL: {dashboard_url}", flush=True)
print("==================================================", flush=True)

if "--no-browser" not in sys.argv and os.environ.get("SAFEPULSE_NO_BROWSER") != "1":
    try:
        webbrowser.open(dashboard_url)
    except Exception:
        pass
