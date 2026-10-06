"""UART capture, with optional active reset or passive POSIX attachment."""
import argparse
import os
import time
import select
import sys
from datetime import datetime
from pathlib import Path

import serial
from esptool.reset import HardReset

parser = argparse.ArgumentParser()
parser.add_argument("--port", default="/dev/ttyACM0")
parser.add_argument("--seconds", type=float, default=90, help="Capture duration; 0 keeps the port open until interrupted")
parser.add_argument("--log", default="monitor-phase01.log")
parser.add_argument("--no-reset", action="store_true", help="Avoid active reset; on POSIX do not change DTR/RTS or flush input (adapter/kernel open may still reset)")
parser.add_argument("--timestamps", action="store_true", help="Prefix UART lines with host wall-clock receive time")
parser.add_argument("--interactive", action="store_true", help="Send ASCII commands from stdin using this same UART connection")
parser.add_argument("--info-logs", action="store_true", help="Send log info on attach and after each Ready banner")
args = parser.parse_args()
if args.seconds < 0:
    parser.error("--seconds must be nonnegative")

if args.no_reset and os.name == "posix":
    from uart_passive import PassiveUart
    uart = PassiveUart(args.port)
else:
    uart = serial.Serial(baudrate=115200, timeout=0.2, exclusive=True)
    uart.dtr = False
    uart.rts = False
    uart.port = args.port
with uart:
    with Path(args.log).open("wb") as log:
        duration = f"for {args.seconds:g}s" if args.seconds else "until interrupted"
        print(f"Capturing {args.port} at 115200 {duration}", flush=True)
        if not args.no_reset:
            HardReset(uart)()
        deadline = time.monotonic() + args.seconds if args.seconds else float("inf")
        count = 0
        pending = b""
        ready_scan = b""
        info_due = time.monotonic() + 1 if args.info_logs else None
        stdin_open = args.interactive
        stdin_pending = b""
        stdin_discard = False
        last_command = 0.0
        while time.monotonic() < deadline:
            now = time.monotonic()
            if info_due is not None and now >= info_due and now-last_command >= 1.1:
                uart.write(b'log info\n');last_command=now;info_due=None
                print("[host] sent: log info", flush=True)
            if stdin_open and select.select([sys.stdin], [], [], 0)[0]:
                chunk = os.read(sys.stdin.fileno(), 256)
                if not chunk:
                    stdin_open = False
                else:
                    for byte in chunk:
                        if byte == 10:
                            if stdin_discard:
                                print("[host] Invalid command length or encoding.", flush=True)
                            else:
                                command = stdin_pending.strip()
                                if command:
                                    uart.write(command+b'\n');last_command=time.monotonic()
                                    print("[host] command sent", flush=True)
                            stdin_pending=b'';stdin_discard=False
                        elif not stdin_discard:
                            if byte>127 or len(stdin_pending)>=96:
                                stdin_discard=True
                            else:
                                stdin_pending+=bytes([byte])
            data = uart.read(uart.in_waiting or 1)
            if data:
                count += len(data)
                if args.info_logs:
                    ready_scan += data
                    if b'Ready. Type help.' in ready_scan:
                        info_due = time.monotonic()+0.3
                        ready_scan = b''
                    else:
                        ready_scan = ready_scan[-64:]
                if args.timestamps:
                    pending += data
                    lines = pending.split(b"\n")
                    pending = lines.pop()
                    # Bound buffering even if UART sends no newline.
                    if len(pending) >= 4096:
                        lines.append(pending)
                        pending = b""
                    for line in lines:
                        stamp = datetime.now().astimezone().isoformat(timespec="milliseconds")
                        rendered = stamp.encode() + b" " + line + b"\n"
                        log.write(rendered)
                        print(rendered.decode("utf-8", errors="replace"), end="", flush=True)
                else:
                    log.write(data)
                    print(data.decode("utf-8", errors="replace"), end="", flush=True)
                log.flush()
        if pending:
            log.write(pending)
            log.flush()
        print(f"\nCapture complete: {count} bytes", flush=True)
