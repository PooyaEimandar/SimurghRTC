#!/usr/bin/env python3
"""Render ephemeral service configuration without logging credentials."""
import os
from pathlib import Path
import re
import sys
import socket

ROOT = Path(__file__).resolve().parent


def secret(name):
    value = (Path('/run/secrets') / name).read_text().strip()
    if not re.fullmatch(r'[A-Za-z0-9_-]{32,128}', value):
        raise ValueError(f'Invalid {name}; expected 32-128 safe ASCII characters')
    return value


def main():
    kind = sys.argv[1]
    os.umask(0o077)
    local_ip = socket.gethostbyname(socket.gethostname())
    if kind == 'janus':
        config = Path('/tmp/janus')
        config.mkdir(exist_ok=True)
        for source in (ROOT / 'config').glob('janus.*'):
            text = source.read_text().replace('@JANUS_SECRET@', secret('janus_api_secret')).replace('@LOCAL_IP@', local_ip)
            (config / source.name.removesuffix('.template')).write_text(text)
        os.execv('/opt/janus/bin/janus', ['janus', '-F', str(config)])
    elif kind == 'turn':
        config = Path('/tmp/turnserver.conf')
        config.write_text((ROOT / 'config/turnserver.conf.template').read_text().replace('@TURN_SECRET@', secret('turn_shared_secret')).replace('@LOCAL_IP@', local_ip))
        os.execv('/opt/coturn/bin/turnserver', ['turnserver', '-c', str(config)])
    elif kind == 'smoke':
        os.execv(sys.executable, [sys.executable, str(ROOT / 'smoke.py')])
    else:
        raise ValueError('Expected janus, turn or smoke')


if __name__ == '__main__':
    main()
