#!/usr/bin/env python3
"""Boot the default live menu entry with a writable sentinel disk attached."""
import hashlib
import pathlib
import subprocess
import socket
import sys
import tempfile
import time

iso = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "rockosdualbootv.1.iso").resolve()
with tempfile.TemporaryDirectory() as directory:
    disk = pathlib.Path(directory) / "sentinel.img"
    serial = pathlib.Path(directory) / "serial.log"
    # A blank disk would be automatically formatted in the normal boot path.
    monitor = pathlib.Path(directory) / "monitor.sock"
    with disk.open("wb") as image:
        image.truncate(16 * 1024 * 1024)
    before = hashlib.sha256(disk.read_bytes()).digest()
    process = subprocess.Popen([
        "qemu-system-x86_64", "-machine", "pc", "-m", "256",
        "-boot", "order=d", "-cdrom", str(iso),
        "-display", "none", "-monitor", f"unix:{monitor},server=on,wait=off",
        "-serial", f"file:{serial}",
        "-drive", f"file={disk},format=raw,if=ide,index=0",
        "-no-reboot", "-no-shutdown",
    ])
    try:
        deadline = time.monotonic() + 120
        log = ""
        while time.monotonic() < deadline and process.poll() is None:
            if serial.exists():
                log = serial.read_text(errors="replace")
                if "[boot] shell initialized; enabling interrupts" in log:
                    break
            time.sleep(0.25)
        else:
            raise RuntimeError("live boot did not reach shell:\n" + log)
        if "[boot] live mode: physical disks disabled" not in log:
            raise RuntimeError("live mode was not selected:\n" + log)
        # Exercise shell writes after startup as well as automatic initialization.
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.connect(str(monitor))
            for command in ("create probe", "write probe live"):
                for character in command:
                    key = "spc" if character == " " else character
                    connection.sendall(f"sendkey {key}\n".encode())
                    time.sleep(0.12)
                connection.sendall(b"sendkey ret\n")
                time.sleep(1)
    finally:
        process.terminate()
        process.wait(timeout=10)
    after = hashlib.sha256(disk.read_bytes()).digest()
    if before != after:
        raise RuntimeError("live boot modified the attached disk")
print("live boot reached the shell without modifying the attached disk")
