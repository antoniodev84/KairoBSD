import pathlib
import shutil
import subprocess
root = pathlib.Path('build/root')
include = root / 'usr/include'
library = root / 'usr/lib'
include.mkdir(parents=True, exist_ok=True)
library.mkdir(parents=True, exist_ok=True)
shutil.copytree('libc/include', include, dirs_exist_ok=True)
shutil.copytree('shared/include', include, dirs_exist_ok=True)
shutil.copytree('third_party/chibicc/include', library / 'chibicc/include', dirs_exist_ok=True)
shutil.copyfile('libc/user.ld', library / 'user.ld')
shutil.copyfile('build/libc/src/start.o', library / 'crt0.o')
objects = sorted(str(p) for p in pathlib.Path('build/libc/src').glob('*.o') if p.name != 'start.o')
subprocess.run(['ar', 'rcs', str(library / 'libkario.a'), *objects], check=True)

licenses = root / 'usr/share/licenses'
for source, name in [('third_party/chibicc/LICENSE', 'chibicc'), ('third_party/tinycc/COPYING', 'tinycc'), ('boot/limine/LICENSE', 'limine')]:
    target = licenses / name
    target.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target / pathlib.Path(source).name)
