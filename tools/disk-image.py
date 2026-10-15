import binascii
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
output = pathlib.Path(sys.argv[1])
root = pathlib.Path(__file__).resolve().parent.parent
local = root / 'tools/host/usr/sbin/mke2fs'
formatter = [str(root / 'tools/host-tool'), 'mke2fs'] if local.exists() else [shutil.which('mke2fs') or '/sbin/mke2fs']
sectors = 262144
start = 2048
length = 131072
block_size = int(sys.argv[2]) if len(sys.argv)>2 else 1024
if block_size not in (1024, 2048, 4096):
    raise ValueError("ext2 block size must be 1024, 2048 or 4096")
with tempfile.TemporaryDirectory() as directory:
    directory = pathlib.Path(directory)
    partition = directory / 'partition.img'
    seed = directory / 'seed'
    seed.mkdir()
    (seed / 'large.bin').write_bytes(bytes(range(256)) * 2048)
    (seed / 'indexed').mkdir()
    for index in range(300):
        (seed / 'indexed' / f'file-{index:04d}').write_text(str(index)+'\n')
    with partition.open('wb') as stream:
        stream.truncate(length * 512)
    subprocess.run(formatter + ['-q', '-F', '-t', 'ext2', '-b', str(block_size), '-I', '128', '-O', 'none,filetype,sparse_super,large_file,dir_index', '-d', str(seed), str(partition)], check=True)
    checker=[str(root / 'tools/host-tool'), 'e2fsck'] if local.exists() else [shutil.which('e2fsck') or '/sbin/e2fsck']
    checked=subprocess.run(checker+['-fyD',str(partition)])
    if checked.returncode not in (0,1):
        raise RuntimeError('e2fsck could not build indexed directories')
    with output.open('wb') as disk:
        disk.truncate(sectors * 512)
        mbr = bytearray(512)
        mbr[510:] = b'\x55\xaa'
        if output.name.startswith('sata'):
            struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\xfe\xff\xff', 0x83, b'\xfe\xff\xff', start, length)
            disk.write(mbr)
        else:
            struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\x00\x02\x00', 0xee, b'\xfe\xff\xff', 1, sectors-1)
            disk.write(mbr)
            entries = bytearray(128 * 128)
            struct.pack_into('<16s16sQQQ72s', entries, 0, uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4').bytes_le, uuid.uuid4().bytes_le, start, start+length-1, 0, 'KarioBSD ext2'.encode('utf-16le').ljust(72, b'\x00'))
            disk_guid = uuid.uuid4().bytes_le
            def header(current, backup, entry_lba):
                data = bytearray(512)
                struct.pack_into('<8sIIIIQQQQ16sQIII', data, 0, b'EFI PART', 0x10000, 92, 0, 0, current, backup, 34, sectors-34, disk_guid, entry_lba, 128, 128, binascii.crc32(entries))
                struct.pack_into('<I', data, 16, binascii.crc32(data[:92]))
                return data
            disk.seek(512)
            disk.write(header(1, sectors-1, 2))
            disk.write(entries)
            disk.seek((sectors-33)*512)
            disk.write(entries)
            disk.write(header(sectors-1, 1, sectors-33))
        disk.seek(start * 512)
        with partition.open('rb') as source:
            shutil.copyfileobj(source, disk)
print(f'{output}: {"MBR" if output.name.startswith("sata") else "GPT"}, ext2 partition at sector {start}')
