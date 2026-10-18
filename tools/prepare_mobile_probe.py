#!/usr/bin/env python3
"""Stage a minimal UE 5.8.3 mobile client probe with explicit dependency identity."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import re
import shutil

import mobile_artifacts
import prepare_profile
import ios_simulator_support


def stage(output: Path, profile: str, artifact_paths: list[Path], app_id: str, ios_team: str = '', simulator_support: Path | None = None) -> dict:
    if profile not in ('direct-only', 'full'):
        raise ValueError('Use direct-only or full to check Janus-disabled/enabled packages.')
    if not re.fullmatch(r'[A-Za-z][A-Za-z0-9]*(\.[A-Za-z][A-Za-z0-9]*){2,}', app_id):
        raise ValueError('Use an application ID with at least three alphanumeric components.')
    if ios_team and not re.fullmatch(r'[A-Z0-9]{10}', ios_team):
        raise ValueError('The Apple team ID must contain ten uppercase letters/digits.')
    artifacts = {}
    for path in artifact_paths:
        report = mobile_artifacts.validate(path)
        platform = report['platform']
        if platform in artifacts:
            raise ValueError('Only one artifact per platform can be staged.')
        artifacts[platform] = report
    if not artifacts:
        raise ValueError('Provide at least one checked mobile artifact.')
    support_identity = None
    if simulator_support is not None:
        if 'IOSSimulator' not in artifacts:
            raise ValueError('Simulator support requires an IOSSimulator artifact.')
        support_identity = ios_simulator_support.validate(simulator_support)
    prepare_profile.stage(profile, output, artifact_paths)
    if 'IOSSimulator' in artifacts:
        # The iOS NullRHI inherits the generic SM5 shader name. Supply the
        # metadata through the supported project platform extension path.
        extension = output / 'Platforms/IOS/Config'
        extension.mkdir(parents=True)
        shutil.copy2(Path(__file__).resolve().parents[1] / 'Config/IOSSimulatorProbe/DataDrivenPlatformInfo.ini',
                     extension / 'DataDrivenPlatformInfo.ini')
        discovery = output / 'Config/IOS'
        discovery.mkdir()
        (discovery / 'IOSEngine.ini').write_text('; Discover the IOS project platform extension for the headless probe.\n')
    if simulator_support is not None:
        shutil.copytree(simulator_support, output / 'Build/IOSSimulator')
    config = output / 'Config/DefaultEngine.ini'
    with config.open('a') as stream:
        stream.write('\n[/Script/AndroidRuntimeSettings.AndroidRuntimeSettings]\n'
                     f'PackageName={app_id}\nMinSDKVersion=26\nTargetSDKVersion=36\n'
                     'bBuildForArm64=True\nbBuildForX8664=False\nbPackageDataInsideApk=True\n'
                     'bEnableBundle=False\nbBuildForES31=True\nbSupportsVulkan=False\n'
                     'bSupportsVulkanSM5=False\nbUseExternalFilesDir=True\n'
                     '\n[/Script/IOSRuntimeSettings.IOSRuntimeSettings]\n'
                     f'BundleIdentifier={app_id}\nBundleName=SimurghRTCProbe\n'
                     'MinimumiOSVersion=IOS_15\nbSupportsIPad=True\nbSupportsIPhone=True\n'
                     'bUseSceneBasedLifecycle=True\n'
                     f'bAutomaticSigning={"True" if ios_team else "False"}\n')
        if ios_team:
            stream.write(f'IOSTeamID={ios_team}\n')
        # UE's modern Xcode generator reads this section for iOS as well as Mac.
        stream.write('\n[/Script/MacTargetPlatform.XcodeProjectSettings]\n'
                     f'BundleIdentifier={app_id}\n'
                     f'bUseAutomaticCodeSigning={"True" if ios_team else "False"}\n')
        if ios_team:
            stream.write(f'CodeSigningTeam={ios_team}\n')
    record = {'schema': 1, 'engine_version': '5.8.3', 'profile': profile, 'app_id': app_id,
              'with_janus': profile == 'full', 'artifacts': artifacts,
              'runtime_validation': 'unverified', 'ios_signing_configured': bool(ios_team)}
    if support_identity is not None:
        record['simulator_support'] = support_identity
    (output / 'mobile-probe.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', choices=['direct-only', 'full'], required=True)
    parser.add_argument('--artifact', type=Path, action='append', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--app-id', required=True)
    parser.add_argument('--ios-team', default='')
    parser.add_argument('--simulator-support', type=Path, help='Checked output from ios_simulator_support.py.')
    args = parser.parse_args()
    try:
        print(json.dumps(stage(args.output.resolve(), args.profile, args.artifact, args.app_id, args.ios_team,
                               args.simulator_support), indent=2))
    except (ValueError, OSError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
