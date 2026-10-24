"""Audit release assets and checksums before publishing. No SDK or user data."""
import hashlib
from pathlib import Path, PurePosixPath
import sys
import xml.etree.ElementTree as ET
import zipfile

root = Path(__file__).resolve().parents[1]
folder = Path(sys.argv[1])
project = ET.parse(root / 'src/SDRSharp.T98/SDRSharp.T98.csproj')
version = project.findtext('./PropertyGroup/Version')
assert project.findtext('./PropertyGroup/PackageLicenseExpression') == 'GPL-3.0-only'
runtime = {'SDRSharp.Plugin.T98.dll', 't98_native.dll', 'ambe2.dll', 'libfftw3f-3.dll'}
prefix = 'Plugins/SDRSharp.T98/'
expected = {prefix + name for name in runtime}
with zipfile.ZipFile(folder / 'STD-T98-Receiver.zip') as archive:
    files = {n.replace('\\', '/') for n in archive.namelist() if not n.endswith('/')}
    assert {n for n in files if n.startswith('Plugins/')} == expected, 'Unexpected plugin file'
    assert files == expected | {'README.md', 'LICENSE', 'FORMAT.md', 'THIRD_PARTY_NOTICES.md', 'VERSION.txt', 'thumbnail.png', 'licenses/ambe2-c.txt', 'licenses/fftw.txt'}, files
    assert archive.read('thumbnail.png') == (root / 'thumbnail.png').read_bytes()
    assert archive.read('VERSION.txt').decode().strip() == version
    assert b'Version 3, 29 June 2007' in archive.read('LICENSE')
    for name in runtime:
        assert archive.read(prefix + name) == (folder / prefix / name).read_bytes()
with zipfile.ZipFile(folder / 'std-t98-receiver-sharp-source.zip') as archive:
    files = archive.namelist()
    for name in files:
        path = PurePosixPath(name)
        assert path.parts[0] == 'std-t98-receiver-sharp' and '..' not in path.parts
        assert not set(path.parts) & {'references', '.git', 'build', 'artifacts', 'bin', 'obj', '__pycache__'}
        assert path.suffix.lower() not in {'.dll', '.exe', '.pyc', '.pdb', '.cf32', '.burst', '.wav'}
        if path.suffix.lower() == '.llr':
            assert path.parent.as_posix() == 'std-t98-receiver-sharp/third_party/ambe2/tests/vectors'
            assert path.name in {'soft_harsh.llr', 'soft_noisy.llr', 'soft_random.llr', 'soft_ties.llr'}
        assert not path.name.startswith('secret-cache.json')
    assert 'std-t98-receiver-sharp/.github/workflows/release.yml' in files
    assert 'std-t98-receiver-sharp/.gitattributes' in files
    assert archive.read('std-t98-receiver-sharp/thumbnail.png') == (root / 'thumbnail.png').read_bytes()
    source_project = ET.fromstring(archive.read('std-t98-receiver-sharp/src/SDRSharp.T98/SDRSharp.T98.csproj'))
    assert source_project.findtext('./PropertyGroup/Version') == version
verified = set()
for line in (folder / 'SHA256SUMS.txt').read_text(encoding='ascii').splitlines():
    digest, name = line.split('  ', 1)
    assert name in expected | {'STD-T98-Receiver.zip', 'std-t98-receiver-sharp-source.zip'}
    assert hashlib.sha256((folder / name).read_bytes()).hexdigest() == digest, name
    verified.add(name)
assert verified == expected | {'STD-T98-Receiver.zip', 'std-t98-receiver-sharp-source.zip'}
print(f'PASS: v{version}, GPL-3.0, four runtime DLLs, corresponding source/workflow, no SDK/cache/captures, SHA256')
