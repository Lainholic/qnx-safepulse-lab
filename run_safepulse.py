import subprocess
import time
import os
import sys
import socket
import webbrowser

qemu_exe = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
vm_dir = r"C:\AI\projects\qnx-safepulse-lab\guest\qemu_vm\output"
kernel_bin = r"C:\AI\projects\qnx-safepulse-lab\guest\qemu_vm\output\ifs.bin"
disk_vmdk = r"C:\AI\projects\qnx-safepulse-lab\guest\qemu_vm\output\disk-qemu.vmdk"
bridge_py = r"C:\AI\projects\qnx-safepulse-lab\host\bridge\qnx_bridge.py"
reports_dir = r"C:\AI\projects\qnx-safepulse-lab\_reports"
HTTP_PORT = 8765

os.makedirs(reports_dir, exist_ok=True)

# Detach flags for Windows so processes run permanently in the background
DETACHED_PROCESS = 0x00000008
CREATE_NEW_PROCESS_GROUP = 0x00000200
FLAGS = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP

def is_port_listening(port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(1.0)
        return s.connect_ex(("127.0.0.1", port)) == 0

print("==================================================")
print(" BlackBerry QNX SafePulse Lab 구동 준비 중...")
print("==================================================")

# 1. Start QEMU (QNX 8.0 Microkernel Guest) if port 9876 not active
if not is_port_listening(9876):
    print("[1/3] QNX 8.0 마이크로커널 (QEMU VM) 부팅 시작...")
    qemu_log_file = os.path.abspath(os.path.join(reports_dir, "qemu_live.log")).replace("\\", "/")
    qemu_cmd = [
        qemu_exe,
        "-m", "2048",
        "-smp", "2",
        "-cpu", "max",
        "-drive", f"file={disk_vmdk},if=ide,id=drv0",
        "-netdev", "user,id=net0,hostfwd=tcp:127.0.0.1:9876-:9876",
        "-device", "virtio-net-pci,netdev=net0",
        "-kernel", kernel_bin,
        "-nographic",
        "-serial", f"file:{qemu_log_file}"
    ]
    p_qemu = subprocess.Popen(
        qemu_cmd,
        cwd=vm_dir,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        creationflags=FLAGS
    )
    print(f"      -> QEMU VM 가동 완료 (PID: {p_qemu.pid})")
else:
    print("[1/3] QNX 8.0 마이크로커널 (QEMU VM)이 이미 정상 가동 중입니다 (포트 9876).")

# 2. Start Web Bridge if port 8765 not active
if not is_port_listening(HTTP_PORT):
    print("[2/3] 호스트 텔레메트리 브리지 및 웹서버 구동 시작...")
    p_bridge = subprocess.Popen(
        [sys.executable, "-u", bridge_py],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        creationflags=FLAGS
    )
    print(f"      -> 브리지 웹서버 가동 완료 (PID: {p_bridge.pid}, 포트 {HTTP_PORT})")
else:
    print(f"[2/3] 브리지 웹서버가 이미 정상 가동 중입니다 (포트 {HTTP_PORT}).")

# 3. Wait for HTTP ready & Launch Browser
print("[3/3] 대시보드 연결 대기 중...")
for i in range(20):
    if is_port_listening(HTTP_PORT):
        break
    time.sleep(0.5)

url = f"http://localhost:{HTTP_PORT}"
print("==================================================")
print(" QNX SafePulse Lab 가동 완료 (Headless Background)")
print(f" 대시보드 엔드포인트: {url}")
print("==================================================")
