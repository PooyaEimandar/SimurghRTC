#!/usr/bin/env python3
"""Validate bounded PCM and real bidirectional Opus transport without audio hardware."""
import argparse
import json
from pathlib import Path
from run_native_abi_probe import run_probe

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifact', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--device')
    parser.add_argument('--link-only', action='store_true')
    parser.add_argument('--ndk', type=Path, default=Path.home() / 'Library/Android/sdk/ndk/27.2.12479018')
    parser.add_argument('--adb', type=Path, default=Path.home() / 'Library/Android/sdk/platform-tools/adb')
    args = parser.parse_args()
    result = run_probe(args.artifact.resolve(), args.output.resolve(), args.device, args.ndk, args.adb, args.link_only, probe='audio')
    print(json.dumps({'passed':result['passed'], 'error':result.get('error')}))
    raise SystemExit(0 if result['passed'] else 1)
