"""Hash the versioned fixture build/config/probe inputs, excluding generated evidence."""
import hashlib


def source_hashes(root):
    files = [root / 'Dockerfile', root / '.dockerignore']
    files += list(root.glob('*.py')) + list(root.glob('*.json'))
    files += [path for path in (root / 'config').rglob('*') if path.is_file()]
    return {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(files)}
