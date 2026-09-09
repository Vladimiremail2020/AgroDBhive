# AgroDBhive — Simulation 3

## Purpose

Simulation 3 benchmarks AgroDBhive against DuckDB and InfluxDB on a Raspberry Pi 4 edge platform using the same 100,000-record synthetic agricultural IoT dataset.

## Experimental configuration

- Platform: Raspberry Pi 4, Linux aarch64
- Python: 3.13.5
- DuckDB: 1.5.2
- Dataset: 100,000 records, 6 nodes
- Sampling interval: 300 s (5 min)
- Seed: 20260811
- Measured repetitions: 5
- Warm-up: excluded from reported statistics
- Statistics: mean ± sample SD
- Cache clearing: requested during the benchmark
- AgroDBhive payload: 64 bits, 59 used bits + 5 reserved bits
- InfluxDB timestamps: explicitly written with `WritePrecision.S`
- InfluxDB query validation: each run required 6 node-level output rows

## Results

| Metric | DuckDB | AgroDBhive | InfluxDB |
|---|---:|---:|---:|
| Ingestion (points/s) | 209,650.34 ± 58,357.35 | 383,109.17 ± 174,904.06 | 10,102.17 ± 344.72 |
| Query latency (ms) | 8.597 ± 0.132 | 10.145 ± 0.094 | 7,721.301 ± 17.236 |
| Storage (MB) | 3.5117 ± 0 | 1.2617 ± 0 | Not measured |
| RAM (MB) | 148.38 ± 5.33 | 146.53 ± 1.40 | 205.61 ± 3.33 |
| CPU (% of one core) | 54.41 ± 25.45 | 56.16 ± 24.68 | 32.81 ± 1.28 |

Additional AgroDBhive measurements:

- Temperature MAE: 0.0024993
- SHA-256 overhead: 5.1748 ± 0.0420 µs/word
- Storage reduction relative to DuckDB: 64.07%
- Ingestion throughput relative to DuckDB: 1.83×
- Query latency increase relative to DuckDB: 18.01%

## Interpretation

The current run is substantially stronger than the previous attempt because the InfluxDB query is now **valid**: all five repetitions returned 6 node-level rows. The previous `query_rows=0` failure has therefore been resolved.

The results support the following bounded claim:

> On the tested Raspberry Pi 4 workload, AgroDBhive achieved approximately 1.83× the measured ingestion throughput of DuckDB and reduced the packed database footprint by 64.07%, while analytical query latency was approximately 18.01% higher.

InfluxDB provides a valid ingestion and query measurement in this run. Its measured query latency is much higher because the benchmark uses a Flux `pivot` + `group` + `reduce` pipeline to reproduce the same node-level analytical outputs. This should be described as an implementation-level cross-engine benchmark, not as a universal database-performance ranking.

InfluxDB storage is intentionally **not reported**. The benchmark does not claim that an immediate data-directory size delta is a reliable representation of InfluxDB's persisted storage because persistence and compaction are asynchronous.

RAM and CPU values are process-level measurements and should not be interpreted as perfectly equivalent resource accounting across an embedded DuckDB/Python workload and the separate `influxd` server process.

## Reproducibility

Generate the dataset:

```bash
python simulation3_v3_1.py generate \
  --rows 100000 \
  --nodes 6 \
  --seed 20260811 \
  --interval 300 \
  --out artifacts/dataset_100k.csv
```

Run DuckDB + AgroDBhive + InfluxDB:

```bash
python simulation3_v3_1.py benchmark \
  --dataset artifacts/dataset_100k.csv \
  --out results/final_3engine \
  --seed 20260811 \
  --interval 300 \
  --repetitions 5 \
  --warmup \
  --clear-cache \
  --influx-url http://127.0.0.1:8086 \
  --influx-token "$INFLUX_TOKEN" \
  --influx-org "$INFLUX_ORG" \
  --influx-batch-size 1000
```

The benchmark writes:

- `simulation3_results.json` — complete summarized and raw results
- `benchmark_runs.csv` — one row per measured repetition
- `benchmark_raw_runs.jsonl` — raw JSON records

## Important methodological note

The storage comparison is specifically between the resulting DuckDB and AgroDBhive database files used by this benchmark. It should not be described as total end-to-end firmware/storage savings including journals, hashes, operating-system overhead, or other deployment files.

