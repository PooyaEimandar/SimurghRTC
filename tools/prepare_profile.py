#!/usr/bin/env python3
"""Stage an isolated plugin/contract host with a single, explicit module profile."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mobile_artifacts

ROOT = Path(__file__).resolve().parents[1]
PLUGIN = ROOT / 'Plugins' / 'SimurghRTC'
POLICY = ROOT / 'Config' / 'Profiles.json'

def documents(profile: str) -> tuple[dict, dict]:
    policy = json.loads(POLICY.read_text())
    names = policy['profiles'][profile]
    for name in names:
        spec = policy['modules'][name]
        for dep in spec['dependencies'] + spec.get('private_dependencies', []):
            if dep.startswith('SimurghRTC') and dep not in names:
                raise ValueError(f'{name} requires omitted module {dep}')
    entries = []
    for name in names:
        spec = policy['modules'][name]
        entry = {'Name': name, 'Type': 'Runtime', 'LoadingPhase': 'Default'}
        for key, field in [('platforms','PlatformAllowList'), ('targets','TargetAllowList'), ('deny_targets','TargetDenyList')]:
            if key in spec:
                entry[field] = spec[key]
        entries.append(entry)
    descriptor = {
        'FileVersion': 3, 'Version': 1, 'VersionName': '0.1.0-m0',
        'FriendlyName': 'SimurghRTC',
        'Description': 'Shared RTC sessions, Direct data channels and generated Opus audio with isolated native WebRTC. Device capture, video and Janus signaling are pending.',
        'Category': 'Networking', 'CreatedBy': 'SimurghRTC contributors',
        'EngineVersion': policy['descriptor_engine_version'],
        'CanContainContent': False, 'IsBetaVersion': True,
        'Installed': False, 'SupportedTargetPlatforms': ['Win64','Mac','Linux','Android','IOS'],
        'Modules': entries,
    }
    manifest = {'schema': 1, 'profile': profile, 'engine_version': policy['engine_version'],
                'with_direct': 'SimurghRTCDirect' in names,
                'with_janus': 'SimurghRTCJanus' in names,
                'with_janus_extensions': 'SimurghRTCJanusExtensions' in names,
                'with_media': 'SimurghRTCMedia' in names,
                'with_server': 'SimurghRTCServer' in names,
                'native_transport': 'shared-direct-data-audio' if 'SimurghRTCDirect' in names else 'internal-sdp-ice', 'desktop_native_factory': 'isolated-artifact-required',
                'mobile_native_factory': 'external-artifact-required', 'modules': names}
    if profile == 'server':
        descriptor['SupportedTargetPlatforms'] = ['Win64','Mac','Linux']
    return descriptor, manifest

def write_documents(destination: Path, profile: str) -> None:
    descriptor, manifest = documents(profile)
    (destination/'SimurghRTC.uplugin').write_text(json.dumps(descriptor, indent=2)+'\n')
    (destination/'SimurghRTC.profile.json').write_text(json.dumps(manifest, indent=2)+'\n')

def stage(profile: str, destination: Path, artifacts: list[Path] | None = None) -> None:
    destination = destination.resolve()
    # Never overwrite the source tree or an existing build/package.
    host = ROOT/'Samples'/'ContractHost'
    if (destination == ROOT or destination in ROOT.parents
            or PLUGIN == destination or PLUGIN in destination.parents
            or host == destination or host in destination.parents):
        raise ValueError('Choose a staging directory outside the plugin source tree.')
    if destination.exists() and any(destination.iterdir()):
        raise ValueError('Output must be new or empty; existing files are never removed.')
    _, manifest = documents(profile)
    selected = {}
    for artifact in artifacts or []:
        report = mobile_artifacts.validate(artifact)
        platform = report['platform']
        if profile == 'server' and platform != 'Mac':
            raise ValueError('The Server profile only accepts a desktop native artifact.')
        if platform in selected:
            raise ValueError(f'Only one {platform} artifact can be selected.')
        if artifact.resolve() == destination or artifact.resolve() in destination.parents:
            raise ValueError('Output must be outside the artifact input tree.')
        selected[platform] = artifact
    target = destination/'Plugins'/'SimurghRTC'
    target.mkdir(parents=True, exist_ok=True)
    for module in manifest['modules']:
        shutil.copytree(PLUGIN/'Source'/module, target/'Source'/module)
    write_documents(target, profile)
    for platform, artifact in selected.items():
        installed = target/'ThirdParty'/'WebRTC'/platform/'arm64'
        shutil.copytree(artifact, installed)
        mobile_artifacts.validate(installed)
    shutil.copy2(ROOT/'LICENSE', target/'LICENSE')
    for name in ['Source','Config']:
        shutil.copytree(host/name, destination/name)
    if (host/'Content').is_dir():
        content = host/'Content'
        rules = json.loads(POLICY.read_text())['sample_content']
        def excluded(directory, names):
            return [name for name in names
                    if (Path(directory)/name).relative_to(content).as_posix() in rules
                    and rules[(Path(directory)/name).relative_to(content).as_posix()] not in manifest['modules']]
        shutil.copytree(content, destination/'Content', ignore=excluded)
    shutil.copy2(ROOT/'Fixtures/signaling/v1/direct-session.json', destination/'Config/SimurghRTC-signaling-v1.json')
    project = json.loads((host/'ContractHost.uproject').read_text())
    project.pop('AdditionalPluginDirectories', None)
    (destination/'ContractHost.uproject').write_text(json.dumps(project, indent=2)+'\n')

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', choices=list(json.loads(POLICY.read_text())['profiles']), default='full')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--mobile-artifact', '--native-artifact', type=Path, action='append', default=[],
                        help='Stage a checked ARM64 artifact. Repeat once per mobile platform.')
    parser.add_argument('--refresh-default', action='store_true', help='Regenerate the checked-in full descriptor/manifest only.')
    parser.add_argument('--check-default', action='store_true')
    args = parser.parse_args()
    if sum([args.output is not None, args.refresh_default, args.check_default]) != 1:
        parser.error('Choose exactly one of --output, --refresh-default, --check-default.')
    if (args.refresh_default or args.check_default) and args.profile != 'full':
        parser.error('The source checkout always uses the full profile; stage other profiles with --output.')
    if args.mobile_artifact and args.output is None:
        parser.error('--mobile-artifact requires --output.')
    if args.refresh_default:
        write_documents(PLUGIN, 'full')
    elif args.check_default:
        expected = documents('full')
        actual = tuple(json.loads((PLUGIN/name).read_text()) for name in ['SimurghRTC.uplugin','SimurghRTC.profile.json'])
        if expected != actual:
            parser.error('Default profile drift: run --refresh-default.')
    else:
        try:
            stage(args.profile, args.output, args.mobile_artifact)
        except (ValueError, OSError) as error:
            parser.error(str(error))

if __name__ == '__main__':
    main()
