import subprocess
import os
import sys

qemu_exe = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
vm_dir = r"C:\AI\projects\qnx-safepulse-lab\guest\qemu_vm\output"
kernel_bin = r"C:\AI\projects\qnx-safepulse-lab\guest\qemu_vm\output\ifs.bin"
disk_vmdk = r"C:\AI\projects\qnx-safepulse-lab\guest\qemu_vm\output\disk-qemu.vmdk"
qemu_log_file = r"C:/AI/projects/qnx-safepulse-lab/_reports/qemu_live.log"

cmd = [
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

print("Starting QEMU supervisor...")
p = subprocess.Popen(
    cmd,
    cwd=vm_dir,
    stdin=subprocess.DEVNULL,
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL
)
print(f"QEMU PID: {p.pid}")
p.wait()
