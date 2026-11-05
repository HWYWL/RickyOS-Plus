"""Reject configured builds that silently retain the previous SAE-off workaround."""
from pathlib import Path
import sys


def check(path):
    configured = {}
    for line in path.read_text().splitlines():
        fields = line.split()
        if len(fields) >= 3 and fields[0] == '#define':
            configured[fields[1]] = fields[2]
    required = ('CONFIG_ESP_WIFI_ENABLE_WPA3_SAE', 'CONFIG_ESP_WIFI_ENABLE_SAE_H2E',
                'CONFIG_ESP_WIFI_WPA3_COMPATIBLE_SUPPORT')
    missing = [key for key in required if configured.get(key) != '1']
    if missing:
        raise SystemExit('Stale Plus WPA3 config: ' + ', '.join(missing) +
                         '. Use a fresh RICKYOS_SDKCONFIG with the reviewed '
                         'RICKYOS_SDKCONFIG_DEFAULTS; do not flash this build.')
    print('Configured Plus WPA3-Personal, SAE H2E and compatibility support verified')


if __name__ == '__main__':
    check(Path(sys.argv[1]))
