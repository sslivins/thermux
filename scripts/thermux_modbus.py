#!/usr/bin/env python3
"""
Read a Thermux (or any Modbus TCP device) over Modbus TCP.

    python scripts/thermux_modbus.py 192.168.1.205
    python scripts/thermux_modbus.py thermux.local --watch 5
    python scripts/thermux_modbus.py 192.168.1.50 --port 5020 --unit 3 raw 100 10
    python scripts/thermux_modbus.py 192.168.1.50 raw 40 4 --fc 3

With no sub-command it decodes the Thermux register map (see README,
"Modbus TCP"). `raw` reads any address range from any device and prints
each register as unsigned, signed and hex.

Needs pymodbus 3.x:  pip install pymodbus
"""
import argparse
import logging
import sys
import time
from datetime import timedelta

try:
    from pymodbus.client import ModbusTcpClient
    from pymodbus.exceptions import ModbusException
except ImportError:
    sys.exit("pymodbus is not installed. Run: pip install pymodbus")

logging.getLogger("pymodbus").setLevel(logging.CRITICAL)
for stream in (sys.stdout, sys.stderr):
    try:
        stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

UNASSIGNED_TEMP = 0x8000
MAX_READ = 125
STATUS = {0: "OK", 1: "Unassigned", 2: "Missing", 3: "Read error", 4: "Stale"}
CYCLE = {0: "OK", 1: "Some sensors failed", 2: "Bus failure", 3: "No sensors"}
EXCEPTIONS = {
    1: "illegal function",
    2: "illegal data address",
    3: "illegal data value",
    4: "server device failure",
    6: "server device busy",
    10: "gateway path unavailable",
    11: "gateway target device failed to respond",
}


class ReadError(Exception):
    pass


class Device:
    def __init__(self, host, port, unit, timeout):
        self.client = ModbusTcpClient(host, port=port, timeout=timeout, retries=1)
        self.host, self.port, self.unit = host, port, unit

    def __enter__(self):
        if not self.client.connect():
            raise ReadError(f"could not connect to {self.host}:{self.port}")
        return self

    def __exit__(self, *exc):
        self.client.close()

    def _call(self, fn, address, count):
        # pymodbus 3.9 renamed slave= to device_id=
        try:
            return fn(address, count=count, device_id=self.unit)
        except TypeError:
            return fn(address, count=count, slave=self.unit)

    def read(self, address, count, fc=4):
        fn = self.client.read_input_registers if fc == 4 else self.client.read_holding_registers
        regs = []
        while count > 0:
            n = min(count, MAX_READ)
            try:
                rr = self._call(fn, address, n)
            except ModbusException as e:
                raise ReadError(f"FC{fc:02d} read of {address}..{address + n - 1}: {e}") from e
            if rr.isError():
                code = getattr(rr, "exception_code", None)
                why = EXCEPTIONS.get(code, str(rr)) if code is not None else str(rr)
                raise ReadError(f"FC{fc:02d} read of {address}..{address + n - 1}: "
                                f"exception 0x{code or 0:02X} ({why})")
            regs.extend(rr.registers)
            address += n
            count -= n
        return regs


def signed(v):
    return v - 0x10000 if v & 0x8000 else v


def read_thermux(dev):
    """One consistent snapshot: retry if the read-cycle counter moves mid-read."""
    for _ in range(5):
        hdr = dev.read(0, 16)
        capacity = hdr[4] or 100
        temps = dev.read(100, capacity)
        status = dev.read(200, capacity)
        ages = dev.read(300, capacity)
        used = [ch for ch in range(capacity) if status[ch] != 1]
        roms = {}
        if used:
            rom_regs = dev.read(1000, 4 * capacity)
            for ch in used:
                roms[ch] = "".join(f"{r:04X}" for r in rom_regs[4 * ch:4 * ch + 4])
        if dev.read(5, 1)[0] == hdr[5]:
            return hdr, temps, status, ages, used, roms
    raise ReadError("values kept changing mid-read (read cycle counter moved 5 times)")


def print_thermux(dev):
    hdr, temps, status, ages, used, roms = read_thermux(dev)
    mac = ":".join(f"{b:02X}" for r in hdr[8:11] for b in (r >> 8, r & 0xFF))
    uptime = timedelta(seconds=(hdr[6] << 16) | hdr[7])
    print(f"Thermux at {dev.host}:{dev.port} unit {dev.unit}")
    print(f"  Firmware {hdr[1]}.{hdr[2]}.{hdr[3]}, map version {hdr[0]}, MAC {mac}, up {uptime}")
    print(f"  Channels in use {hdr[12]}/{hdr[4]} (firmware limit {hdr[11]}), "
          f"sensors on bus {hdr[13]}")
    print(f"  Last read cycle: {CYCLE.get(hdr[14], hdr[14])} (#{hdr[5]}), "
          f"read interval {hdr[15]} s")
    print()
    if not used:
        print("  No channels assigned.")
        return
    print(f"  {'Ch':>3}  {'Reg':>4}  {'Temp':>9}  {'Status':<10}  {'Age':>6}  ROM ID")
    for ch in used:
        t = "—" if temps[ch] == UNASSIGNED_TEMP else f"{signed(temps[ch]) / 100:.2f} °C"
        age = "never" if ages[ch] == 0xFFFF else f"{ages[ch]} s"
        print(f"  {ch:>3}  {100 + ch:>4}  {t:>9}  {STATUS.get(status[ch], status[ch]):<10}  "
              f"{age:>6}  {roms.get(ch, '')}")


def print_raw(dev, address, count, fc):
    regs = dev.read(address, count, fc)
    kind = "input" if fc == 4 else "holding"
    print(f"{dev.host}:{dev.port} unit {dev.unit}, {kind} registers (FC{fc:02d}) {address}..{address + count - 1}")
    print(f"  {'Addr':>5}  {'Unsigned':>8}  {'Signed':>7}  Hex")
    for i, v in enumerate(regs):
        print(f"  {address + i:>5}  {v:>8}  {signed(v):>7}  0x{v:04X}")


def main():
    p = argparse.ArgumentParser(description="Read a Thermux or any Modbus TCP device.")
    p.add_argument("host", help="IP address or hostname")
    p.add_argument("--port", type=int, default=502, help="TCP port (default 502)")
    p.add_argument("--unit", type=int, default=1, help="unit ID (default 1)")
    p.add_argument("--timeout", type=float, default=3.0, help="seconds (default 3)")
    p.add_argument("--watch", type=float, metavar="SECONDS",
                   help="repeat every SECONDS until Ctrl+C")
    sub = p.add_subparsers(dest="cmd")
    raw = sub.add_parser("raw", help="read any address range")
    raw.add_argument("address", type=int, help="zero-based start address")
    raw.add_argument("count", type=int, help="number of registers")
    raw.add_argument("--fc", type=int, choices=(3, 4), default=4,
                     help="4 = input registers (default), 3 = holding registers")
    args = p.parse_args()

    if args.cmd == "raw" and not (0 <= args.address <= 65535 and 1 <= args.count <= 65536 - args.address):
        p.error("address must be 0..65535 and the range must end at or before 65535")

    try:
        with Device(args.host, args.port, args.unit, args.timeout) as dev:
            while True:
                if args.watch:
                    print(time.strftime("%H:%M:%S"))
                if args.cmd == "raw":
                    print_raw(dev, args.address, args.count, args.fc)
                else:
                    print_thermux(dev)
                if not args.watch:
                    break
                print(flush=True)
                time.sleep(args.watch)
    except ReadError as e:
        sys.exit(f"error: {e}")
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
