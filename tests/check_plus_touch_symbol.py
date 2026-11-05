"""Fail if the linked image dropped the Plus touch backend.

The Metalio board's touch poller is reached through a registration table, so a
silently dropped component still links. Checking the ELF keeps that failure loud.
Usage: python tests/check_plus_touch_symbol.py <path to .elf>
"""
import pathlib
import sys

NEEDLE = b"pollMetalioPlusTouch"

if len(sys.argv) != 2:
    raise SystemExit(__doc__.strip())

image = pathlib.Path(sys.argv[1])
if not image.is_file():
    raise SystemExit(f'No ELF at {image}')

if NEEDLE not in image.read_bytes():
    raise SystemExit('Linked application is missing the Plus touch backend')
print(f'{image.name}: Plus touch backend present')
