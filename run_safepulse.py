import subprocess
import time
import os
import sys
import socket
import webbrowser

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
        cmd = f"Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{{CommandLine = 'pythonw.exe \"{bridge_py}\"'}}"
        subprocess.run(["powershell", "-Command", cmd], capture_output=True)
    else:
        subprocess.Popen([sys.executable, bridge_py], start_new_session=True)
    print(f"      -> Bridge daemon spawned on port {HTTP_PORT}.")
else:
    print(f"[2/3] Bridge already listening on port {HTTP_PORT}.")

# 3. Wait for services ready
print("[3/3] Synchronizing Dashboard link...")
for attempt in range(25):
    if is_port_listening(HTTP_PORT) and is_port_listening(QNX_PORT):
        break
    time.sleep(1.0)

dashboard_url = f"http://127.0.0.1:{HTTP_PORT}"
print("==================================================")
print(" QNX SafePulse Lab READY!")
print(f" Dashboard URL: {dashboard_url}")
print("==================================================")

if "--no-browser" not in sys.argv and os.environ.get("SAFEPULSE_NO_BROWSER") != "1":
    try:
        webbrowser.open(dashboard_url)
    except Exception:
        pass
