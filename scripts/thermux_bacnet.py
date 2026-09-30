#!/usr/bin/env python3
"""
Discover and read a Thermux BACnet/IP server.

    python scripts/thermux_bacnet.py --discover
    python scripts/thermux_bacnet.py 192.168.1.205
    python scripts/thermux_bacnet.py 192.168.1.205 --port 47809 --cov 60 --try-write

Uses BAC0 (recommended): pip install BAC0
"""
import argparse
import sys
import time

try:
    import BAC0
except ImportError:
    sys.exit("BAC0 is not installed. Run: pip install BAC0")

UNITS = {62: "degreesCelsius"}
RELIABILITY = {
    0: "noFaultDetected",
    1: "noSensor",
    7: "unreliableOther",
    12: "communicationFailure",
}

def prop(bacnet, addr, obj, name):
    return bacnet.read(f"{addr} {obj} {name}")

def object_list(bacnet, addr):
    objs = prop(bacnet, addr, "device", "objectList")
    if not isinstance(objs, (list, tuple)):
        return []
    return objs

def read_device(host, port, cov, try_write):
    ip = f"0.0.0.0/24:{port}"
    bacnet = BAC0.lite(ip=ip)
    addr = f"{host}:{port}"
    try:
        device_id = prop(bacnet, addr, "device", "objectIdentifier")
        name = prop(bacnet, addr, "device", "objectName")
        vendor = prop(bacnet, addr, "device", "vendorName")
        model = prop(bacnet, addr, "device", "modelName")
        firmware = prop(bacnet, addr, "device", "firmwareRevision")
        app = prop(bacnet, addr, "device", "applicationSoftwareVersion")
        desc = prop(bacnet, addr, "device", "description")
        print(f"Thermux BACnet/IP at {addr}")
        print(f"  Device: {device_id} {name}")
        print(f"  Vendor: {vendor}; Model: {model}; Firmware: {firmware}; App: {app}")
        print(f"  Description: {desc}")
        print()

        ais = []
        for obj in object_list(bacnet, addr):
            typ = obj[0] if isinstance(obj, (list, tuple)) else str(obj).split()[0]
            inst = obj[1] if isinstance(obj, (list, tuple)) and len(obj) > 1 else None
            if str(typ) in ("analogInput", "analog-input", "0"):
                if inst is None:
                    inst = int(str(obj).split()[-1].strip('),'))
                ais.append(int(inst))
        ais.sort()
        if not ais:
            print("  No Analog Input objects found.")
        else:
            print(f"  {'AI':>3}  {'Name':<32}  {'Value':>9}  {'Units':<16}  {'Reliability':<24}  Status")
            for inst in ais:
                prefix = f"analogInput {inst}"
                name = prop(bacnet, addr, prefix, "objectName")
                value = prop(bacnet, addr, prefix, "presentValue")
                units = prop(bacnet, addr, prefix, "units")
                rel = prop(bacnet, addr, prefix, "reliability")
                flags = prop(bacnet, addr, prefix, "statusFlags")
                try:
                    value_s = f"{float(value):.2f}"
                except Exception:
                    value_s = str(value)
                print(f"  {inst:>3}  {str(name):<32.32}  {value_s:>9}  {UNITS.get(units, units)!s:<16.16}  {RELIABILITY.get(rel, rel)!s:<24.24}  {flags}")

        if try_write and ais:
            try:
                bacnet.write(f"{addr} analogInput {ais[0]} presentValue 12.34")
                print("WriteProperty unexpectedly succeeded")
            except Exception as exc:
                print(f"WriteProperty rejected as expected: {exc}")

        if cov and ais:
            print(f"\nSubscribing COV to analogInput {ais[0]} for {cov} seconds...")
            device = BAC0.device(host, device_id[1] if isinstance(device_id, tuple) else device_id, bacnet, poll=0)
            point = device.points.get(f"analogInput:{ais[0]}") or next(iter(device.points.values()))
            point.subscribe_cov(lifetime=cov)
            end = time.time() + cov
            last = None
            while time.time() < end:
                val = point.lastValue
                if val != last:
                    print(f"  COV/value: {val}")
                    last = val
                time.sleep(1)
    finally:
        bacnet.disconnect()

def discover(port):
    bacnet = BAC0.lite(ip=f"0.0.0.0/24:{port}")
    try:
        devices = bacnet.whois()
        if not devices:
            print("No BACnet devices found.")
            return
        for dev in devices:
            print(dev)
    finally:
        bacnet.disconnect()

def main():
    parser = argparse.ArgumentParser(description="Discover/read Thermux BACnet/IP.")
    parser.add_argument("host", nargs="?", help="device IP address")
    parser.add_argument("--port", type=int, default=47808, help="BACnet/IP UDP port (default 47808)")
    parser.add_argument("--discover", action="store_true", help="send Who-Is and print discovered devices")
    parser.add_argument("--cov", type=int, metavar="SECONDS", help="subscribe to COV for one AI")
    parser.add_argument("--try-write", action="store_true", help="verify WriteProperty is rejected")
    args = parser.parse_args()
    if args.discover:
        discover(args.port)
    elif args.host:
        read_device(args.host, args.port, args.cov, args.try_write)
    else:
        parser.error("host is required unless --discover is used")

if __name__ == "__main__":
    main()
