#!/usr/bin/env python3
"""Run the disposable HVF composer fixture and verify captured scanout pixels."""
import argparse
import json
import pathlib
import socket
import subprocess
import time
from PIL import Image

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("kernel", type=pathlib.Path)
parser.add_argument("initrd", type=pathlib.Path)
parser.add_argument("output", type=pathlib.Path, help="new evidence directory")
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
serial = args.output.resolve() / "serial.log"
screenshot = args.output.resolve() / "scanout.png"
qmp_path = pathlib.Path(f"/tmp/aurora-composer-{__import__('os').getpid()}.sock")
command = ["qemu-system-aarch64", "-machine", "virt", "-accel", "hvf", "-cpu", "host",
           "-m", "2048", "-smp", "2", "-device", "virtio-gpu-pci", "-display", "none",
           "-serial", f"file:{serial}", "-qmp", f"unix:{qmp_path},server=on,wait=off", "-no-reboot",
           "-kernel", str(args.kernel.resolve()), "-initrd", str(args.initrd.resolve()),
           "-append", "console=ttyAMA0 earlycon=pl011,0x9000000 rdinit=/init user4k=on loglevel=6"]
captured = False
with (args.output / "qemu.log").open("w") as output:
    process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
    deadline = time.monotonic() + 90
    try:
        while process.poll() is None:
            log = serial.read_text(errors="replace") if serial.exists() else ""
            if "Kernel panic - not syncing" in log:
                process.terminate()
                process.wait(timeout=5)
                break
            if not captured and "COMPOSER_KMS_VISIBLE" in log:
                subprocess.run(["python3", str(pathlib.Path(__file__).with_name("qmp.py")), str(qmp_path),
                                json.dumps({"execute": "screendump", "arguments": {
                                    "filename": str(screenshot), "format": "png"}})], check=True)
                captured = True
            if time.monotonic() >= deadline:
                raise TimeoutError("composer fixture exceeded 90 seconds")
            time.sleep(0.1)
        result = process.returncode
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        qmp_path.unlink(missing_ok=True)
log = serial.read_text(errors="replace")
markers = ["DMABUF_HEAP_PASS page=4096", "OHOS_DMA_HEAP_RESULT=0", "COMPOSER_FULL_PROBE_PASS",
           "ORIGINAL_COMPOSER_SERVICE_PASS", "OHOS_ORIGINAL_COMPOSER_RESULT=0",
           "COMPOSER_FRAME_PASS frame=3", "OHOS_COMPOSER_RESULT=0", "OHOS_KMS_TEST_RESULT=0"]
report = {"command": command, "qemu_exit": result, "captured": captured,
          "markers": {marker: marker in log for marker in markers}}
if captured:
    frame = Image.open(screenshot).convert("RGB")
    width, height = frame.size
    pixels = frame.load()
    mismatches = 0
    for y in range(height):
        green = 255 if 1 + y * 5 // height < 3 else 0
        for x in range(width):
            red = 255 if 2 + x * 9 // width < 6 else 0
            if pixels[x, y] != (red, green, 64):
                mismatches += 1
    report.update(width=width, height=height, pixel_mismatches=mismatches)
report["passed"] = (result == 0 and captured and all(report["markers"].values()) and
                    report.get("pixel_mismatches") == 0)
(args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
raise SystemExit(0 if report["passed"] else 1)
