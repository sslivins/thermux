#!/usr/bin/env python3
"""
Discover and read a Thermux BACnet/IP server.

    python scripts/thermux_bacnet.py --discover
    python scripts/thermux_bacnet.py 192.168.1.205
    python scripts/thermux_bacnet.py 192.168.1.205 --port 47809 --cov 60 --try-write

Needs BAC0 2025.x or later (async API): pip install BAC0
Use --local 192.168.1.32/24 if BAC0 picks the wrong network interface.
"""
import argparse
import asyncio
import sys
import time

try:
    import BAC0
    from bacpypes3.pdu import Address
    from bacpypes3.primitivedata import ObjectIdentifier, Real
except ImportError:
    sys.exit("BAC0 is not installed. Run: pip install BAC0")

def start(local):
    BAC0.log_level("error")
    return BAC0.start(ip=local) if local else BAC0.start()


async def discover(local):
    async with start(local) as bacnet:
        devices = await bacnet.who_is()
        if not devices:
            print("No BACnet devices found.")
        for iam in devices or []:
            print(f"{iam.pduSource}  device {iam.iAmDeviceIdentifier[1]}  vendor {iam.vendorID}")


async def read_device(host, port, local, cov, try_write):
    target = host if port == 47808 else f"{host}:{port}"
    addr = Address(target)
    async with start(local) as bacnet:
        app = bacnet.this_application.app

        async def rp(obj, prop):
            return await asyncio.wait_for(app.read_property(addr, ObjectIdentifier(obj), prop), 5)

        # Instance 4194303 means "whichever device answers"; unlike Who-Is it
        # also works when the device uses a non-standard port.
        try:
            device_id = await rp("device,4194303", "object-identifier")
        except BaseException as exc:
            sys.exit(f"No BACnet response from {target}: {exc!r}")
        dev = f"device,{device_id[1]}"
        print(f"Thermux BACnet/IP at {target}")
        print(f"  Device {device_id[1]}: {await rp(dev, 'object-name')}")
        print(f"  Vendor: {await rp(dev, 'vendor-name')}; Model: {await rp(dev, 'model-name')}; "
              f"Firmware: {await rp(dev, 'firmware-revision')}")
        print(f"  Description: {await rp(dev, 'description')}\n")

        objects = await rp(dev, "object-list")
        ais = sorted(inst for typ, inst in objects if str(typ) == "analog-input")
        if not ais:
            print("  No Analog Input objects (no sensor channels assigned).")
        else:
            print(f"  {'AI':>3}  {'Name':<32}  {'Value':>9}  {'Reliability':<24}  ROM ID")
            for inst in ais:
                obj = f"analog-input,{inst}"
                print(f"  {inst:>3}  {str(await rp(obj, 'object-name')):<32.32}  "
                      f"{await rp(obj, 'present-value'):>9.2f}  "
                      f"{str(await rp(obj, 'reliability')):<24.24}  {await rp(obj, 'description')}")

        if try_write and ais:
            try:
                await app.write_property(addr, ObjectIdentifier(f"analog-input,{ais[0]}"),
                                         "present-value", Real(12.34))
                print("\nWriteProperty unexpectedly succeeded")
            except BaseException as exc:  # bacpypes3 raises RejectPDU, not an Exception
                print(f"\nWriteProperty rejected as expected: {exc}")

        if cov and ais:
            print(f"\nCOV on analogInput {ais[0]} for {cov} s (Ctrl+C to stop)...")
            end = time.time() + cov
            async with app.change_of_value(addr, ObjectIdentifier(f"analog-input,{ais[0]}"),
                                           None, False, cov + 30) as scm:
                while (left := end - time.time()) > 0:
                    try:
                        prop, value = await asyncio.wait_for(scm.get_value(), left)
                    except asyncio.TimeoutError:
                        break
                    print(f"  {time.strftime('%H:%M:%S')}  {prop} = {value}")


def main():
    parser = argparse.ArgumentParser(description="Discover/read Thermux BACnet/IP.")
    parser.add_argument("host", nargs="?", help="device IP address")
    parser.add_argument("--port", type=int, default=47808, help="device UDP port (default 47808)")
    parser.add_argument("--local", help="local address/prefix to bind, e.g. 192.168.1.32/24")
    parser.add_argument("--discover", action="store_true", help="broadcast Who-Is and list devices on port 47808")
    parser.add_argument("--cov", type=int, metavar="SECONDS", help="subscribe to COV for the first AI")
    parser.add_argument("--try-write", action="store_true", help="check that WriteProperty is rejected")
    args = parser.parse_args()
    if args.discover:
        asyncio.run(discover(args.local))
    elif args.host:
        asyncio.run(read_device(args.host, args.port, args.local, args.cov, args.try_write))
    else:
        parser.error("host is required unless --discover is used")


if __name__ == "__main__":
    main()