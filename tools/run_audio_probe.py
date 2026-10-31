#!/usr/bin/env python3
"""Run the headless Unreal PCM/Opus transport probe and retain its build identity."""
import argparse
import json
import math
from pathlib import Path
import subprocess
import uuid
import mobile_artifacts as a
import native_artifacts

MARKER = 'SIMURGHRTC_AUDIO_REPORT '


def validate(report, run_id, identity, kind):
    expected = dict(schema=1, run_id=run_id, passed=True, error='', pairs=2, peers_closed=4,
                    loopback_only=True, audio_hardware_opened=False,
                    editor_build=kind.startswith('editor-'), server_build=kind=='server',
                    artifact_manifest_sha256=identity['manifest_sha256'],
                    native_backend='M155-5bd86a2f17a35dfc986b87d058f511e45805bd1d')
    for key, value in expected.items():
        a.require(type(report.get(key)) is type(value) and report[key] == value, 'Audio report differs: ' + key)
    a.require(report.get('engine_version','').startswith('5.8.3-'), 'Expected Unreal 5.8.3.')
    for key, low, high in [('minimum_correlation',.9,1.0001), ('maximum_muted_rms',0,.002),
                           ('seconds',14,90), ('max_tick_gap_seconds',0,2)]:
        value=report.get(key)
        a.require(type(value) in (int,float) and math.isfinite(value) and low <= value < high, 'Invalid audio measurement: '+key)
    for key, minimum in [('decoded_samples',192000),('heartbeats',30)]:
        value=report.get(key)
        a.require(type(value) is int and value>=minimum, 'Missing audio progress: '+key)


def run(executable, artifact, output, kind, project=None):
    identity=native_artifacts.validate(artifact)
    a.require(identity['platform']=='Mac', 'This packaged runner currently targets Mac ARM64.')
    a.require(executable.is_file() and (not kind.startswith('editor-') or (project and project.is_file())), 'Missing executable/project.')
    a.require(not output.exists(), 'Use a fresh evidence directory.')
    output.mkdir(parents=True);run_id=str(uuid.uuid4())
    command=[str(executable)]+([str(project)] if project else [])+['/Engine/Maps/Entry',
        '-server' if kind in ('server','editor-server') else '-game', '-NullRHI','-nosound','-unattended','-nosplash','-SkipZenStore',
        '-SimurghRTCAudioProbe','-SimurghRTCAudioProbeRunId='+run_id,'-stdout','-FullStdOutLogOutput']
    record=dict(schema=1,passed=False,run_id=run_id,kind=kind,command=command,artifact=identity,
                executable_sha256=a.digest(executable),scope='two loopback PCM/Opus peer pairs inside one Unreal process; no public media session, HTTP media signaling or hardware audio')
    try:
        with (output/'process.log').open('w') as log:
            result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,timeout=150)
        text=(output/'process.log').read_text(errors='replace')
        lines=[line.split(MARKER,1)[1] for line in text.splitlines() if MARKER in line]
        a.require(result.returncode==0 and len(lines)==1, 'Audio process failed or report is missing/duplicated.')
        report=json.loads(lines[0]);record['report']=report;validate(report,run_id,identity,kind)
        a.require(a.digest(executable)==record['executable_sha256'], 'Executable changed during probe.')
        record['passed']=True
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:
        record['error']=str(error)
    (output/'result.json').write_text(json.dumps(record,indent=2)+'\n')
    return record


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable',type=Path,required=True)
    parser.add_argument('--native-artifact',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--kind',choices=['game','server','editor-game','editor-server'],required=True)
    parser.add_argument('--project',type=Path)
    args=parser.parse_args()
    result=run(args.executable.resolve(),args.native_artifact.resolve(),args.output.resolve(),args.kind,args.project.resolve() if args.project else None)
    print(json.dumps(dict(passed=result['passed'],error=result.get('error'))))
    raise SystemExit(0 if result['passed'] else 1)
