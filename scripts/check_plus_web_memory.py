"""Reject a stale configured build that silently ignores the network defaults."""
from pathlib import Path
import sys

expected = {
    'CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL': '2048',
    'CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP': '1',
    'CONFIG_ESP_WIFI_STATIC_TX_BUFFER': '1',
    'CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM': '16',
    'CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM': '32',
}


def check(path):
    configured = {}
    for line in path.read_text().splitlines():
        fields = line.split()
        if len(fields) >= 3 and fields[0] == '#define':
            configured[fields[1]] = fields[2]
    missing = [key for key, value in expected.items() if configured.get(key) != value]
    if missing:
        raise SystemExit('Stale Plus network config: ' + ', '.join(missing) +
                         '. Reconfigure from sdkconfig.defaults using RICKYOS_SDKCONFIG and '
                         'RICKYOS_SDKCONFIG_DEFAULTS; do not flash this build.')
    print('Configured Plus ordinary/internal allocation preference and network buffer defaults verified')


if __name__ == '__main__':
    check(Path(sys.argv[1]))
