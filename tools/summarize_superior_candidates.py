"""Summarize paired quartet deltas; CSV timestamps are authoritative raw samples."""
import csv
import json
from collections import defaultdict
from pathlib import Path
from statistics import median, pstdev
import sys

for filename in sys.argv[1:]:
    rows = list(csv.DictReader(open(filename, newline='')))
    grouped = defaultdict(list)
    for row in rows:
        grouped[row['stage'], int(row['scale'])].append(row)
    results = []
    for (stage, scale), group in sorted(grouped.items()):
        samples = {v: [float(r['gpu_ms']) for r in group if r['variant'] == v] for v in ('base', 'candidate')}
        quartets = defaultdict(lambda: defaultdict(list))
        for r in group:
            quartets[int(r['block']), int(r['quartet'])][r['variant']].append(float(r['gpu_ms']))
        paired = [median(q['candidate']) - median(q['base']) for q in quartets.values() if len(q)==2]
        blocks = defaultdict(list)
        for key, q in quartets.items():
            if len(q)==2: blocks[key[0]].append(median(q['candidate']) - median(q['base']))
        result = {'stage': stage, 'scale': scale, 'samples_per_variant': len(samples['base']),
                  'base_ms': median(samples['base']), 'candidate_ms': median(samples['candidate']),
                  'paired_delta_ms': median(paired), 'paired_stddev_ms': pstdev(paired),
                  'base_stddev_ms': pstdev(samples['base']), 'candidate_stddev_ms': pstdev(samples['candidate']),
                  'block_median_deltas_ms': [median(b) for b in blocks.values()]}
        results.append(result)
        print(f"{Path(filename).stem:18} {stage:16} P{scale:3} {result['base_ms']:.6f} -> {result['candidate_ms']:.6f} "
              f"paired {median(paired):+.6f} std {pstdev(paired):.6f} n={len(samples['base'])}")
    Path(filename).with_suffix('.json').write_text(json.dumps(results, indent=2)+'\n')
