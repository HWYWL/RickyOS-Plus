#!/usr/bin/env python3
"""Test only a new temporary file on the verified Plus USB SD volume, then remove it."""
from pathlib import Path
import fcntl
import hashlib
import json
import os
import plistlib
import subprocess
import sys
import time
import uuid

volume=Path(sys.argv[1]).resolve()
report=Path(sys.argv[2])
usb=subprocess.check_output(['ioreg','-p','IOUSB','-l','-w0'],text=True)
assert 'RickyOS Metalio Plus' in usb and '"USB Serial Number" = "1c2904d02a50"' in usb, 'Expected Plus USB identity absent'
info=plistlib.loads(subprocess.check_output(['diskutil','info','-plist',str(volume)]))
assert info['BusProtocol']=='USB' and info['MountPoint']==str(volume) and info.get('Writable')
assert not info.get('Internal',True), 'Refuse internal media'
assert 15_000_000_000 < info['TotalSize'] < 17_000_000_000, 'Unexpected SD volume size'
books=volume/'books'
assert books.is_dir() and books.resolve().parent==volume
payload=hashlib.shake_256(b'RickyOS Plus S31 USB sector validation').digest(1024*1024)
file=books/('.rickyos-usb-check-'+uuid.uuid4().hex+'.tmp')
record={'device':info['DeviceIdentifier'],'whole_disk':info['ParentWholeDisk'],'bytes':len(payload),'expected_sha256':hashlib.sha256(payload).hexdigest(),'cache_bypass':True}
created=False
try:
 start=time.monotonic()
 fd=os.open(file,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600);created=True
 with os.fdopen(fd,'wb') as stream:
  # Darwin F_NOCACHE=48: direct test avoids a successful read from the OS page cache.
  fcntl.fcntl(stream.fileno(),48,1)
  stream.write(payload);stream.flush();os.fsync(stream.fileno())
 record['write_seconds']=time.monotonic()-start
 start=time.monotonic()
 with file.open('rb') as stream:
  fcntl.fcntl(stream.fileno(),48,1)
  received=stream.read(len(payload)+1)
 record['read_seconds']=time.monotonic()-start
 record['read_sha256']=hashlib.sha256(received).hexdigest()
 record['matched']=received==payload
 assert record['matched'], 'USB readback differs'
finally:
 if created:
  file.unlink();record['temporary_file_removed']=True
 report.write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record,indent=2))
