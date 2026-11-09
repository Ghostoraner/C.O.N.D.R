#!/usr/bin/env python3
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


QEMU = os.environ.get("QEMU", "qemu-system-x86_64")


def read_log(path):
    try:
        return path.read_text(errors="replace")
    except FileNotFoundError:
        return ""


def wait_for_log(process, path, text, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        log = read_log(path)
        if text in log:
            return log
        if process.poll() is not None:
            raise RuntimeError(f"QEMU exited early while waiting for {text!r}\n{log}")
        time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for {text!r}\n{read_log(path)}")


def monitor_command(monitor, command):
    monitor.sendall((command + "\n").encode())
    response = bytearray()
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        try:
            chunk = monitor.recv(4096)
        except TimeoutError:
            continue
        if not chunk:
            break
        response.extend(chunk)
        if response.rstrip().endswith(b"(qemu)"):
            return response.decode(errors="replace")
    raise RuntimeError(f"QEMU monitor timed out for {command!r}: {response!r}")


def send_line(monitor, line):
    for character in line:
        key = "spc" if character == " " else character
        monitor_command(monitor, f"sendkey {key} 35")
        time.sleep(0.025)
    monitor_command(monitor, "sendkey ret 35")


def send_keys(monitor, keys):
    for character in keys:
        monitor_command(monitor, f"sendkey {character} 35")
        time.sleep(0.05)


def start_qemu(iso, disk, serial_path, monitor_path):
    command = [
        QEMU, "-M", "q35", "-m", "512M", "-smp", "1",
        "-cdrom", str(iso), "-boot", "d",
        "-drive", f"if=none,id=condr-data,format=raw,file={disk}",
        "-device", "virtio-blk-pci,disable-modern=on,drive=condr-data",
        "-display", "none", "-serial", f"file:{serial_path}",
        "-monitor", f"unix:{monitor_path},server,nowait",
        "-no-reboot", "-no-shutdown",
    ]
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    monitor.settimeout(0.2)
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        try:
            monitor.connect(str(monitor_path))
            break
        except (FileNotFoundError, ConnectionRefusedError):
            if process.poll() is not None:
                raise RuntimeError("QEMU exited before the monitor became available")
            time.sleep(0.05)
    else:
        process.terminate()
        raise RuntimeError("QEMU monitor did not become available")

    greeting = bytearray()
    while b"(qemu)" not in greeting:
        try:
            greeting.extend(monitor.recv(4096))
        except TimeoutError:
            continue
    return process, monitor


def stop_qemu(process, monitor):
    try:
        monitor_command(monitor, "quit")
    except (OSError, RuntimeError):
        process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
    monitor.close()


def main():
    iso = Path(sys.argv[1] if len(sys.argv) > 1 else "build/condr.iso").resolve()
    if not iso.is_file():
        raise RuntimeError(f"ISO not found: {iso}")

    with tempfile.TemporaryDirectory(prefix="condr-test-") as temp_dir:
        root = Path(temp_dir)
        disk = root / "data.img"
        disk.write_bytes(bytes(4 * 1024 * 1024))
        log = root / "serial.log"
        monitor_path = root / "monitor.sock"

        process, monitor = start_qemu(iso, disk, log, monitor_path)
        try:
            wait_for_log(process, log, "C.O.N.D.R user-mode IPC: pong")
            wait_for_log(process, log, "kbd-demo: ready")

            # Syscall 8 coverage: the kbd-demo user task reads five characters
            # one at a time and echoes each of them back.
            send_keys(monitor, "hello")
            wait_for_log(process, log, "kbd-demo: done")

            wait_for_log(process, log, "condr:/# ")
            send_line(monitor, "touch persisted")
            text = wait_for_log(process, log, "created: ")
            match = re.search(r"created: ([0-9a-f]{6})", text)
            if not match:
                raise RuntimeError(f"NVMFS did not print a six-character ID\n{text}")
            object_id = match.group(1)

            send_line(monitor, f"put {object_id} persistent")
            wait_for_log(process, log, "stream written: 10 bytes")
            send_line(monitor, "sync")
            wait_for_log(process, log, "NVMFS snapshot committed")
            stop_qemu(process, monitor)

        except Exception:
            stop_qemu(process, monitor)
            raise

        log.write_text("")
        process, monitor = start_qemu(iso, disk, log, monitor_path)
        try:
            wait_for_log(process, log, "[store] restored snapshot generation")
            wait_for_log(process, log, "C.O.N.D.R user-mode IPC: pong")
            wait_for_log(process, log, "condr:/# ")
            send_line(monitor, f"get {object_id}")
            restored = wait_for_log(process, log, "persistent")
            if "user-mode IPC: pong" not in restored:
                raise RuntimeError(f"Ring 3 IPC smoke test failed\n{restored}")
            send_line(monitor, "check")
            checks = wait_for_log(process, log, "IPC check: 7 passed")
            if "NVMFS check: 6 passed" not in checks:
                raise RuntimeError(f"NVMFS self-test failed\n{checks}")
            print(f"QEMU integration passed: short ID {object_id}, IPC, and disk restore")
        finally:
            stop_qemu(process, monitor)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"QEMU integration failed: {error}", file=sys.stderr)
        sys.exit(1)
