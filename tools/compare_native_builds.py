#!/usr/bin/env python3
"""Compare two fresh isolated builds with each other and an accepted artifact."""
import argparse
import json
from pathlib import Path

import mobile_artifacts as a
import native_artifacts


def compare(left, right, baseline):
    roots = {'a': left, 'b': right, 'baseline': baseline}
    identities = {key: native_artifacts.validate(path) for key, path in roots.items()}
    manifests = {key: a.read_json(path / 'artifact.json') for key, path in roots.items()}
    builds = {key: a.read_json(path / 'Build/build.json') for key, path in roots.items()}
    a.require(manifests['a']['metadata'] == manifests['b']['metadata'] == manifests['baseline']['metadata'],
              'Artifact target/source metadata differs.')
    for key in ('a', 'b'):
        a.require(builds[key].get('fresh_output') is True, 'Repeat builds must start from empty output directories.')
        a.require(isinstance(builds[key].get('build_directory'), str) and builds[key]['build_directory'],
                  'Missing build directory provenance.')
    a.require(builds['a']['build_directory'] != builds['b']['build_directory'], 'Repeat builds reused one output directory.')
    # Debug objects retain compiler paths. Compare the shipped, stripped archive
    # exactly; do not normalize or rewrite either artifact to obtain a match.
    build_local = {'build_directory', 'archive_before_isolation_sha256', 'isolated_sha256'}
    a.require({k: v for k, v in builds['a'].items() if k not in build_local}
              == {k: v for k, v in builds['b'].items() if k not in build_local},
              'Build inputs or toolchain records differ.')
    inventory = manifests['a']['files']
    a.require(all(set(m['files']) == set(inventory) for m in manifests.values()), 'Payload inventories differ.')
    run_records = {'Build/build.json', 'Build/link-probe.json'}
    stable = sorted(set(inventory) - run_records)
    for name in stable:
        a.require(len({m['files'][name] for m in manifests.values()}) == 1,
                  'Repeat or accepted-baseline payload mismatch: ' + name)
    if 'Build/link-probe.json' in inventory:
        links = [a.read_json(roots[key] / 'Build/link-probe.json') for key in ('a', 'b')]
        a.require({k: v for k, v in links[0].items() if k != 'executable_sha256'}
                  == {k: v for k, v in links[1].items() if k != 'executable_sha256'},
                  'Android consumer proof inputs differ.')
    differences = {key: sorted(name for name in inventory if manifests[key]['files'][name] != inventory[name])
                   for key in ('b', 'baseline')}
    return {'schema': 1, 'passed': True, 'platform': identities['a']['platform'],
            'scope': 'two fresh outputs on the same host; shared verified sources and toolchain',
            'independent_host_validated': False, 'artifacts': identities,
            'build_directories': {key: builds[key]['build_directory'] for key in ('a', 'b')},
            'archive_sha256': inventory['Lib/libwebrtc.a'], 'matches_accepted_archive': True,
            'byte_identical_payloads': stable, 'record_differences_from_a': differences,
            'recipe_sha256': builds['a']['recipe_sha256'],
            'baseline_recipe_sha256': builds['baseline']['recipe_sha256'],
            'raw_intermediate_hashes': {key: {name: builds[key].get(name) for name in
                ('archive_before_isolation_sha256', 'isolated_sha256')} for key in roots}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('left', 'right', 'baseline', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    a.require(not args.output.exists(), 'Use a new comparison report path.')
    record = {'schema': 1, 'passed': False}
    try:
        record = compare(args.left.resolve(), args.right.resolve(), args.baseline.resolve())
    except (ValueError, OSError, KeyError) as error:
        record['error'] = str(error)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({'passed': record['passed'], 'report': str(args.output), 'error': record.get('error')}))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
