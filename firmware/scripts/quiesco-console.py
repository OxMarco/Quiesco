#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Talk to a debug build (make build-debug) over USB serial.

  scripts/quiesco-console.py info               device, config, log head
  scripts/quiesco-console.py measure            take a measurement now
  scripts/quiesco-console.py stress             45 s of back-to-back flash
                                                writes; unplug USB during it
  scripts/quiesco-console.py dump log.csv       whole on-flash log as CSV
  scripts/quiesco-console.py trace trace.txt    the flash trace ring, oldest
                                                first (make build-trace only)
  scripts/quiesco-console.py monitor [out.txt]  print (and save) every line,
                                                timestamped, until Ctrl-C

The port is found automatically when exactly one /dev/cu.usbmodem* exists;
pass --port otherwise. Standard library only (termios), macOS and Linux.
"""

import argparse
import datetime
import glob
import os
import select
import sys
import termios
import time
import tty


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    if len(ports) != 1:
        sys.exit(f"found {len(ports)} candidate ports ({', '.join(ports) or 'none'}); "
                 "pass --port")
    return ports[0]


class Port:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.fd)
        attrs = termios.tcgetattr(self.fd)
        attrs[4] = attrs[5] = termios.B115200
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        self.buffer = b""

    def send(self, command):
        os.write(self.fd, command.encode())

    def lines(self, timeout):
        """Yields complete lines; stops after `timeout` s of silence."""
        while True:
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                yield line.decode(errors="replace").rstrip("\r")
            ready, _, _ = select.select([self.fd], [], [], timeout)
            if not ready:
                return
            self.buffer += os.read(self.fd, 4096)


def info(port):
    port.send("i")
    for line in port.lines(timeout=2):
        if line.startswith("info "):
            print(line)


def dump(port, path):
    port.send("d")
    records = 0
    started = False
    with open(path, "w") as out:
        # The dump waits for the current cycle and a 1 s rail settle.
        for line in port.lines(timeout=30):
            if line.startswith("# begin"):
                started = True
                out.write(line + "\n")
                continue
            if not started:
                continue
            if line.startswith("# end"):
                out.write(line + "\n")
                print(f"{line[2:]} -> {path}")
                if "incomplete" in line:
                    sys.exit(1)
                return
            if line and line[0].isdigit() or line.startswith("sequence,"):
                out.write(line + "\n")
                records += line[0].isdigit()
                if records and records % 5000 == 0:
                    print(f"  {records} records", file=sys.stderr)
    sys.exit(f"dump stopped after {records} records without an end marker")


def trace(port, path):
    port.send("t")
    started = False
    lines = 0
    with open(path, "w") as out:
        # Like dump: waits for the current cycle and a 1 s rail settle.
        for line in port.lines(timeout=30):
            if line == "trace dump begin":
                started = True
                continue
            if not started:
                continue
            if line.startswith("trace dump end"):
                print(f"{lines} lines ({line.split('=')[-1]} bytes) -> {path}")
                return
            out.write(line + "\n")
            lines += 1
    sys.exit(f"trace stopped after {lines} lines without an end marker "
             "(is this a make build-trace image?)")


def monitor(port, path):
    out = open(path, "a") if path else None
    try:
        for line in port.lines(timeout=None):
            stamped = f"{datetime.datetime.now().isoformat(timespec='seconds')} {line}"
            print(stamped, flush=True)
            if out:
                out.write(stamped + "\n")
                out.flush()
    except KeyboardInterrupt:
        pass
    finally:
        if out:
            out.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port")
    parser.add_argument("command",
                        choices=["info", "measure", "stress", "dump", "trace",
                                 "monitor"])
    parser.add_argument("file", nargs="?")
    args = parser.parse_args()
    port = Port(args.port or find_port())
    time.sleep(0.2)
    if args.command == "info":
        info(port)
    elif args.command == "measure":
        port.send("m")
    elif args.command == "stress":
        port.send("w")
        for line in port.lines(timeout=60):
            if line.startswith("stress"):
                print(line, flush=True)
                if line.startswith("stress done"):
                    break
    elif args.command == "dump":
        if not args.file:
            sys.exit("dump needs an output file")
        dump(port, args.file)
    elif args.command == "trace":
        if not args.file:
            sys.exit("trace needs an output file")
        trace(port, args.file)
    else:
        monitor(port, args.file)


if __name__ == "__main__":
    main()
