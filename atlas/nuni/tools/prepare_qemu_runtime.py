"""Fetch verified Ubuntu ARM64 libraries for isolated SDK QEMU tests ONLY.
These libraries are not linked, packaged or deployed with NUNI.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import urllib.request

PACKAGES = [
    ('libcap2.deb', 'https://ports.ubuntu.com/ubuntu-ports/pool/main/libc/libcap2/libcap2_2.66-5ubuntu2.4_arm64.deb',
     '3c4459762b9ee61c773209bc45db1ca7cf34e1254cc047260d309a8db11b3490'),
    ('libzstd1.deb', 'https://ports.ubuntu.com/ubuntu-ports/pool/main/libz/libzstd/libzstd1_1.5.5+dfsg2-2build1.1_arm64.deb',
     'ea7d6e9fde577ea14dbf07fdfb74d6287eb09c6cba4a9d63abdcb87b0ec54bcc'),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', default='build/qemu-runtime')
    args = parser.parse_args()
    root = Path(args.output).resolve()
    root.mkdir(parents=True, exist_ok=True)
    for filename, url, checksum in PACKAGES:
        archive = root/filename
        data = archive.read_bytes() if archive.exists() else urllib.request.urlopen(url, timeout=30).read()
        if hashlib.sha256(data).hexdigest() != checksum:
            raise RuntimeError(f'{filename}: SHA256 mismatch; refusing extraction')
        archive.write_bytes(data)
        subprocess.run(['dpkg-deb', '-x', str(archive), str(root)], check=True)
    (root/'SOURCE.json').write_text(json.dumps(PACKAGES, indent=2))
    print(root/'usr/lib/aarch64-linux-gnu')

if __name__ == '__main__':
    main()
