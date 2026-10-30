#!/usr/bin/env python3
"""Run packaged C++/Blueprint reconnect, HTTPS loss and TURN loss recovery probes."""
from run_mobile_network_probe import main

if __name__=='__main__':raise SystemExit(main(recovery=True))
