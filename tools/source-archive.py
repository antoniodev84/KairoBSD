import os
import pathlib
import stat
import subprocess
import sys
import zipfile

root = pathlib.Path(__file__).resolve().parent.parent
output = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root.parent / (root.name + '-source.zip')
result = subprocess.run(
    ['git', '-C', str(root), 'ls-files', '--cached', '--others', '--exclude-standard', '-z'],
    check=True,
    stdout=subprocess.PIPE,
)
paths = {root / os.fsdecode(name) for name in result.stdout.split(b'\0') if name}
paths.update(path for path in (root / '.git').rglob('*') if path.is_file() or path.is_symlink())
excluded = ('build/', 'tools/host/', 'tools/apt/', 'tools/packages/', 'boot/limine/.git/')
output.parent.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    for path in sorted(paths):
        relative = path.relative_to(root).as_posix()
        if relative.startswith(excluded) or '__pycache__' in path.relative_to(root).parts:
            continue
        if path.resolve() == output or not (path.is_file() or path.is_symlink()):
            continue
        name = root.name + '/' + relative
        if path.is_symlink():
            info = zipfile.ZipInfo(name)
            info.create_system = 3
            info.external_attr = (stat.S_IFLNK | 0o777) << 16
            archive.writestr(info, os.readlink(path))
        else:
            archive.write(path, name)
with zipfile.ZipFile(output) as archive:
    names = archive.namelist()
    if root.name + '/.git/HEAD' not in names:
        raise RuntimeError('Archive does not contain .git/HEAD')
    if archive.testzip() is not None:
        raise RuntimeError('Archive integrity verification failed')
print(f'{output}: {output.stat().st_size} bytes, {len(names)} files')
