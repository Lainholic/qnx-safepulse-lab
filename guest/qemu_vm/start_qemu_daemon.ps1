$qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$args = @(
    "-m", "2048",
    "-smp", "2",
    "-cpu", "max",
    "-drive", "file=C:/AI/projects/qnx-safepulse-lab/guest/qemu_vm/output/disk-qemu.vmdk,if=ide,id=drv0",
    "-netdev", "user,id=net0,hostfwd=tcp:127.0.0.1:9876-:9876",
    "-device", "virtio-net-pci,netdev=net0",
    "-kernel", "C:/AI/projects/qnx-safepulse-lab/guest/qemu_vm/output/ifs.bin",
    "-nographic",
    "-serial", "file:C:/AI/projects/qnx-safepulse-lab/_reports/qemu_live.log"
)
Start-Process -FilePath $qemu -ArgumentList $args -WorkingDirectory "C:\AI\projects\qnx-safepulse-lab\guest\qemu_vm\output" -WindowStyle Hidden
