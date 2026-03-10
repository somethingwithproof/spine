#!/usr/bin/env python3
"""Generate snmpsim .snmprec files for N fake devices.

Each file is named after the device IP and placed under --outdir.
IPs are drawn from the RFC 5737 documentation ranges:
  192.0.2.1..192.0.2.254   (devices 1-254)
  198.51.100.1..198.51.100.254 (devices 255-508)
"""
import argparse
import os
import random


def device_ip(n: int) -> str:
    """Return the RFC 5737 documentation IP for device number n (1-based)."""
    if n <= 254:
        return f"192.0.2.{n}"
    # second range: offset by 254
    offset = n - 254
    if offset > 254:
        raise ValueError(f"device number {n} exceeds supported range (max 508)")
    return f"198.51.100.{offset}"


def snmprec_lines(n: int) -> list[str]:
    """Return the .snmprec content lines for device number n."""
    ip = device_ip(n)
    # sysDescr
    descr = f"Linux device-{n} 5.15.0 #1 SMP x86_64"
    # sysUpTime: random timeticks (hundredths of a second since boot)
    uptime = random.randint(0, 999999)
    # ifNumber: random 1-32 interfaces
    if_count = random.randint(1, 32)

    return [
        f"1.3.6.1.2.1.1.1.0|4|{descr}",
        "1.3.6.1.2.1.1.2.0|6|1.3.6.1.4.1.8072.3.2.10",
        f"1.3.6.1.2.1.1.3.0|67|{uptime}",
        f"1.3.6.1.2.1.1.5.0|4|device-{n}.test.local",
        f"1.3.6.1.2.1.2.1.0|2|{if_count}",
    ]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--count",
        type=int,
        default=500,
        metavar="N",
        help="number of fake devices to generate (max 508, default: 500)",
    )
    parser.add_argument(
        "--outdir",
        default="tests/load/snmprec",
        metavar="DIR",
        help="output directory for .snmprec files (default: tests/load/snmprec)",
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=None,
        metavar="SEED",
        help="random seed for reproducible output",
    )
    args = parser.parse_args()

    if args.count < 1 or args.count > 508:
        parser.error("--count must be between 1 and 508")

    if args.seed is not None:
        random.seed(args.seed)

    os.makedirs(args.outdir, exist_ok=True)

    for n in range(1, args.count + 1):
        ip = device_ip(n)
        # snmpsim uses the filename (without extension) as the community string
        # when the request source IP matches. We name files by IP so snmpsim
        # can serve them via the public/ data directory mount.
        path = os.path.join(args.outdir, f"{ip}.snmprec")
        lines = snmprec_lines(n)
        with open(path, "w", encoding="ascii") as fh:
            fh.write("\n".join(lines) + "\n")

    print(f"Generated {args.count} .snmprec files in {args.outdir}")


if __name__ == "__main__":
    main()
