import os
import pathlib
import sys
import tarfile
root = pathlib.Path(sys.argv[1])
for name in ('sbin', 'bin', 'etc', 'tmp', 'dev', 'mnt'):
    (root / name).mkdir(parents=True, exist_ok=True)
(root / 'etc' / 'motd').write_text('KarioBSD v0.1.0\nIndependent x86_64 UEFI operating system\n')
with tarfile.open(sys.argv[2], 'w', format=tarfile.USTAR_FORMAT) as archive:
    for path in sorted(root.rglob('*'), key=lambda p: (len(p.parts), str(p))):
        entry = archive.gettarinfo(str(path), str(path.relative_to(root)))
        entry.uid = entry.gid = entry.mtime = 0
        entry.uname = entry.gname = 'root'
        entry.mode = 0o755 if path.is_dir() or path.parent.name in ('bin', 'sbin') else 0o644
        if path.is_file():
            with path.open('rb') as source:
                archive.addfile(entry, source)
        else:
            archive.addfile(entry)
