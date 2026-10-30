#!/usr/bin/env python3
"""Validate actual selected TURN routes and DataChannel payloads through the isolated C ABI on Mac."""
import argparse
import json
from pathlib import Path
import subprocess
import mobile_artifacts as a
import native_artifacts
from network_fixtures import TurnFixture
ROOT=Path(__file__).resolve().parents[1]


def run(artifact, fixture, output):
    a.require(not output.exists(),'Use a fresh evidence directory.');output.mkdir(parents=True)
    identity=native_artifacts.validate(artifact);a.require(identity['platform']=='Mac','Mac fixture required.')
    record=dict(schema=1,passed=False,artifact=identity,fixture=a.read_json(fixture),cases={})
    sources=[ROOT/'Fixtures/native'/name for name in ('turn.c','data_channels.c','signaling.c')]
    record['sources']={str(p.relative_to(ROOT)):a.digest(p) for p in sources}
    commands=[]
    def command(args):
        args=list(map(str,args));r=subprocess.run(args,capture_output=True,text=True,timeout=120)
        commands.append(dict(command=args,exit_code=r.returncode,stdout=r.stdout,stderr=r.stderr))
        (output/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');a.require(r.returncode==0,'Native compile/link failed.')
        return r.stdout.strip()
    try:
        lock=a.read_json(artifact/'Build/source-lock.json');spec=lock['platforms']['Mac']
        a.require(command(['xcodebuild','-version'])==lock['mac']['xcode'],'Xcode drift.')
        sdk=command(['xcrun','--sdk','macosx','--show-sdk-path']);target='arm64-apple-macos'+spec['minimum_os']
        command(['xcrun','clang','-std=c11','-target',target,'-isysroot',sdk,'-I',artifact/'Include','-c',sources[0],'-o',output/'turn.o'])
        frameworks=[v for name in ('Foundation','Security','SystemConfiguration','CoreAudio','AudioToolbox') for v in ('-framework',name)]
        executable=output/'turn_probe'
        command(['xcrun','clang++','-target',target,'-isysroot',sdk,output/'turn.o',artifact/'Lib/libwebrtc.a',*frameworks,'-o',executable])
        with TurnFixture(fixture,output/'turn') as turn:
            for transport in ('udp','tcp'):
                c=turn.config_for(transport=transport)['servers'][0]
                r=subprocess.run([executable],input='\n'.join([c['urls'][0],c['username'],c['credential']])+'\n',capture_output=True,text=True,timeout=90)
                # The test prints only candidate types and counters, never credentials.
                a.require(c['credential'] not in r.stdout+r.stderr,'Credential appeared in native output.')
                (output/(transport+'.log')).write_text(r.stdout+r.stderr)
                record['cases'][transport]=dict(exit_code=r.returncode,stdout=r.stdout)
                a.require(r.returncode==0 and 'PASS: TURN relay-only text/binary round trips;' in r.stdout,'TURN '+transport+' probe failed.')
        record['passed']=True
    except (ValueError,OSError,subprocess.SubprocessError) as error:record['error']=str(error)
    (output/'result.json').write_text(json.dumps(record,indent=2)+'\n');return record


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('artifact','fixture','output'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();r=run(args.artifact.resolve(),args.fixture.resolve(),args.output.resolve())
    print(json.dumps(dict(passed=r['passed'],error=r.get('error'))));raise SystemExit(0 if r['passed'] else 1)
