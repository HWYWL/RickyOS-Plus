"""A pure-WPA3 claim needs bootstrap, actual security logs and all associations."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile

script = Path(__file__).resolve().parents[1] / 'scripts/summarize_plus_network_auto.py'
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    for label, auth, security_log, missing_round, expected in (
        ('pure', 6, True, 0, True),
        ('legacy', 4, True, 0, False),
        ('no-security-log', 6, False, 0, False),
        ('failed-join', 6, True, 5, False),
    ):
        lines = ['[WPA3TEST] bootstrap connected=1 pure_wpa3_ap=1 auth=6 pwe=3 threshold=6']
        if security_log:
            lines.append('wifi:security: WPA3-SAE, pmf:1')
        for round in range(1, 13):
            phase = (round - 1) // 4
            joined = int(round != missing_round)
            lines.append(f'[FONTNETAUTO] round={round} phase={phase} copy_tx=0 joined={joined} join_ms=2000')
            if joined:
                lines.append(f'[FONTNETAUTO] link round={round} phase={phase} protocols=7 phy_result=0 phy=4 he20=0 ap_result=0 ap_ax=0 auth={auth}')
                lines.append(f'[FONTNETAUTO] round={round} phase={phase} copy_tx=0 ok=1 cleanup=1 request_ms=700 families=7 files=77 clock_started=1')
        count = 11 if missing_round else 12
        lines.append(f'[FONTNETAUTO] done attempted={count} passed={count} expected=12 elapsed_ms=40000 auto_reconnect_restored=1')
        log, output = root / (label + '.log'), root / (label + '.json')
        log.write_text('\n'.join(lines))
        subprocess.run([sys.executable, str(script), str(log), '--security-check', '--wpa3-only-ap',
                        '--output', str(output)], check=True, stdout=subprocess.DEVNULL)
        result = json.loads(output.read_text())
        assert result['wpa3_only_real_ap_tested']
        assert result['wpa3_only_ap_regression_passed'] is expected
print('WPA3 report rejects legacy auth, missing actual security evidence and interrupted associations')
