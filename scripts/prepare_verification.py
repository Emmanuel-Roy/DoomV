#!/usr/bin/env python3
"""Compile ACT tests and generate Sail signatures before running regression."""
import argparse
import sys
from common import checkout_lock, entrypoint, wsl_script


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("extensions", nargs="?", default="", help="optional comma-separated extension subset")
    args = parser.parse_args()
    with checkout_lock():
        wsl_script("generate_references.sh", args.extensions)
    return 0


if __name__ == "__main__":
    sys.exit(entrypoint(main))
