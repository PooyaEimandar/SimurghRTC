#!/usr/bin/env python3
"""Run the native DataChannel fixture on Mac ARM64, Android or the iOS simulator."""
import argparse
import json
from pathlib import Path
from run_native_abi_probe import run_probe


def run(artifact, output, device=None, ndk=None, adb=None):
    return run_probe(artifact, output, device, ndk, adb, probe='data')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifact', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--device')
    parser.add_argument('--ndk', type=Path, default=Path.home() / 'Library/Android/sdk/ndk/27.2.12479018')
    parser.add_argument('--adb', type=Path, default=Path.home() / 'Library/Android/sdk/platform-tools/adb')
    args = parser.parse_args()
    result = run(args.artifact.resolve(), args.output.resolve(), args.device, args.ndk, args.adb)
    print(json.dumps({'passed': result['passed'], 'error': result.get('error')}))
    raise SystemExit(0 if result['passed'] else 1)
