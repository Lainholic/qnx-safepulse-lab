import subprocess
import time
import os
import sys
import socket

reports_dir = r"C:\AI\projects\qnx-safepulse-lab\_reports"
os.makedirs(reports_dir, exist_ok=True)

HTTP_PORT = 8765
QNX_PORT = 9876

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
    qemu_ps1 = r"C:\AI\projects\qnx-safepulse-lab\guest\qemu_vm\start_qemu_daemon.ps1"
    subprocess.run(["powershell", "-ExecutionPolicy", "Bypass", "-File", qemu_ps1])
    print("      -> QNX 8.0 Guest starting in background.")
else:
    print("[1/3] QNX 8.0 Guest already running on port 9876.")

# 2. Start Web Bridge
if not is_port_listening(HTTP_PORT):
    print("[2/3] Starting Telemetry Bridge & Dashboard Server...")
    bridge_py = r"C:\AI\projects\qnx-safepulse-lab\host\bridge\qnx_bridge.py"
    cmd = f"Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{{CommandLine = 'pythonw.exe {bridge_py}'}}"
    subprocess.run(["powershell", "-Command", cmd], capture_output=True)
    print("      -> Bridge daemon spawned on port 8765.")
else:
    print(f"[2/3] Bridge already listening on port {HTTP_PORT}.")

# 3. Wait for services ready
print("[3/3] Synchronizing Dashboard link...")
for attempt in range(25):
    if is_port_listening(HTTP_PORT) and is_port_listening(QNX_PORT):
        break
    time.sleep(1.0)

print("==================================================")
print(f" QNX SafePulse Lab READY!")
print(f" Dashboard URL: http://127.0.0.1:{HTTP_PORT}")
print("==================================================")
