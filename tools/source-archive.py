import sys
from pathlib import Path
import zipfile

root=Path(__file__).resolve().parents[1]
allowed=['src','native','tools','tests','models','third_party','licenses','.github','README.md','LICENSE','FORMAT.md','THIRD_PARTY_NOTICES.md','.gitignore','.gitattributes']
with zipfile.ZipFile(sys.argv[1],'x',compression=zipfile.ZIP_DEFLATED) as archive:
    for name in allowed:
        path=root/name
        for file in path.rglob('*') if path.is_dir() else [path]:
            relative=file.relative_to(root)
            if not file.is_file() or any(p in ('bin','obj','__pycache__','.git') for p in relative.parts): continue
            if file.suffix.lower() in ('.dll','.exe','.pyc','.pdb'): continue
            if file.name.startswith('secret-cache.json'): continue
            archive.write(file,'std-t98-receiver-sharp/'+relative.as_posix())
