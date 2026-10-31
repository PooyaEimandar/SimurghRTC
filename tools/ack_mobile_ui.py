#!/usr/bin/env python3
"""Record an operator's completed Device Hub action for an owned simulator probe."""
import argparse
import json
from pathlib import Path
import uuid

def acknowledge(path, observed, *, failed=False):
    request=json.loads(path.read_text())
    if request.get('action') not in ('select-device','lock','unlock') or not observed.strip():
        raise ValueError('Describe the completed UI action on the named fixture simulator.')
    uuid.UUID(request['device']);uuid.UUID(request['nonce'])
    response=dict(nonce=request['nonce'],device=request['device'],action=request['action'],verified=not failed,observed=observed)
    destination=path.with_name('ui-response.json');temporary=destination.with_suffix('.tmp')
    temporary.write_text(json.dumps(response,indent=2)+'\n');temporary.replace(destination)
    return response

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('request',type=Path)
    outcome=parser.add_mutually_exclusive_group(required=True)
    outcome.add_argument('--observed',help='Describe the completed, verified action.')
    outcome.add_argument('--failed',help='Describe why the UI action could not be completed.')
    args=parser.parse_args();print(json.dumps(acknowledge(args.request,args.observed or args.failed,failed=args.failed is not None)))
if __name__=='__main__':main()
