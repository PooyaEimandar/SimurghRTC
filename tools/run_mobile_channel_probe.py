#!/usr/bin/env python3
"""Validate packaged C++/Blueprint DataChannel options, limits and queue recovery."""
from run_mobile_network_probe import main

if __name__ == '__main__':
    raise SystemExit(main(channels=True))
