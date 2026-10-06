"""Extract AP-local browser reports from an existing timestamped UART capture.

The browser posts to the ESP32 over its AP; no route from WSL to the client
subnet or extra connection to the board is needed. Reading this file is passive.
"""
import argparse
import json
import math
import statistics
from datetime import datetime
from pathlib import Path


def collect(log):
    records = []
    decoder = json.JSONDecoder()
    for line in log.read_text(errors='replace').splitlines():
        marker = 'link_bench: BENCH_RESULT '
        if marker not in line:
            continue
        try:
            stamp = line.split(' ', 1)[0]
            datetime.fromisoformat(stamp)
            report, _ = decoder.raw_decode(line.split(marker, 1)[1])
            if report.get('firmware_label') not in ('diag35-ap-local', 'diag36-sta-socks'):
                continue
            if report.get('phase') not in ('start', 'round', 'complete', 'error'):
                continue
            if report.get('phase') == 'complete':
                rounds = report['rounds']
                if len(rounds) != 3 or report['streams'] != 1:
                    continue
                for key in ('down', 'up'):
                    values = [r[key] for r in rounds]
                    if any(type(v) not in (int, float) or not math.isfinite(v) or v <= 0 for v in values):
                        raise ValueError('invalid measurements')
                    report['median_' + key] = round(statistics.median(values), 2)
            records.append({'received_at': stamp, 'source': 'UART', 'report': report})
        except (ValueError, TypeError, KeyError, AttributeError):
            continue  # Partial UART line is not a completed report.
    return records


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('log', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    records = collect(args.log)
    args.output.write_text(''.join(json.dumps(r, ensure_ascii=False, allow_nan=False) + '\n' for r in records))
    for record in records:
        report = record['report']
        if report['phase'] == 'complete':
            print(f"{report['firmware_label']} run={report['run_id']} down={report['median_down']} up={report['median_up']} Mbps")
    print(f'Saved {len(records)} reports to {args.output}')
