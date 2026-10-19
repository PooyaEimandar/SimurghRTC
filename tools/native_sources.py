"""Fetch and verify the pinned source graph for the isolated native backend."""
from pathlib import Path
import subprocess
import mobile_artifacts as artifacts
import mobile_build
ROOT = Path(__file__).resolve().parents[1]

def source_inputs(work, lock):
    """Fetch exact revisions without resetting or adopting another checkout."""
    patch_list = lock['patches'] + lock.get('backend_patches', [])
    allowed = {}
    for patch in patch_list:
        allowed.setdefault(patch['repository'], set()).update(patch['files'])
    for name, spec in sorted(lock['repositories'].items(), key=lambda item: (item[0].count('/'), item[0])):
        artifacts.require(len(spec['revision']) == 40
                          and all(c in '0123456789abcdef' for c in spec['revision']), 'Unpinned source: ' + name)
        artifacts.require(spec['url'].startswith('https://'), 'Source URLs must use HTTPS.')
        path = mobile_build.owned_path(work, name)
        if not (path / '.git').exists():
            artifacts.require(not path.exists() or not any(path.iterdir()), 'Nonempty source directory: ' + name)
            path.mkdir(parents=True, exist_ok=True)
            mobile_build.run(['git', 'init', '-q', path])
            mobile_build.run(['git', 'remote', 'add', 'origin', spec['url']], cwd=path)
        artifacts.require(mobile_build.run(['git', 'remote', 'get-url', 'origin'], cwd=path) == spec['url'],
                          'Source remote drift: ' + name)
        head = subprocess.run(['git', 'rev-parse', '--verify', 'HEAD'], cwd=path, capture_output=True, text=True)
        if head.returncode:
            print('Fetching ' + name, flush=True)
            mobile_build.run(['git', 'fetch', '--depth=1', 'origin', spec['revision']], cwd=path)
            mobile_build.run(['git', 'checkout', '--detach', 'FETCH_HEAD'], cwd=path)
        artifacts.require(mobile_build.run(['git', 'rev-parse', 'HEAD'], cwd=path) == spec['revision'],
                          'Source revision drift: ' + name)
        changed = set(mobile_build.run(['git', 'diff', '--name-only', 'HEAD'], cwd=path).splitlines())
        artifacts.require(changed <= allowed.get(name, set()), 'Edited source: ' + name)
        generated = {'src': {'out/', 'simurgh_probe/'},
                     'src/build': {'config/gclient_args.gni'},
                     'src/third_party': {'llvm-build/'}}.get(name, set())
        nested = {child[len(name) + 1:] + '/' for child in lock['repositories'] if child.startswith(name + '/')}
        untracked = set(filter(None, mobile_build.run(
            ['git', 'ls-files', '--others', '--directory', '-z'], cwd=path).split('\0')))
        artifacts.require(untracked <= generated | nested,
                          'Unexpected source files in ' + name + ': ' + str(sorted(untracked - generated - nested)[:12]))
    final_hashes = {}
    known_hashes = {}
    for patch in patch_list:
        for name, hashes in patch['files'].items():
            final_hashes[(patch['repository'], name)] = hashes['after']
            known_hashes.setdefault((patch['repository'], name), set()).update(hashes.values())
    for patch in patch_list:
        artifacts.require(artifacts.digest(ROOT / patch['path']) == patch['sha256'], 'Native patch drift.')
        repo = work / patch['repository']
        actual = {name: artifacts.digest(repo / name) for name in patch['files']}
        if all(actual[name] == hashes['before'] for name, hashes in patch['files'].items()):
            mobile_build.run(['git','apply','--check',ROOT / patch['path']], cwd=repo)
            mobile_build.run(['git','apply',ROOT / patch['path']], cwd=repo)
        else:
            artifacts.require(all(actual[name] in known_hashes[(patch['repository'], name)]
                                  for name, hashes in patch['files'].items()), 'Edited native patch input: ' + patch['path'])
    for (repo, name), digest in final_hashes.items():
        artifacts.require(artifacts.digest(work / repo / name) == digest, 'Native patch result mismatch: ' + name)
    artifacts.require(artifacts.digest(work / 'src/DEPS') == lock['upstream_deps_sha256'], 'DEPS drift.')
