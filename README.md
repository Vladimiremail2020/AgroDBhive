# AgroDBhive Edge Validation

This repository contains the simulation code and validation artifacts for the AgroDBhive concept: an edge-oriented, bit-packed storage framework for IoT sensor streams in smart agriculture.

## Scope

The implementation validates:
- 64-bit bit-packed storage of multi-sensor readings,
- local DuckDB-based persistence,
- irrigation alarm behavior across simulated nodes,
- lightweight hash-chain integrity verification.

## Bit Layout

The packed 64-bit layout uses:
- soil_moisture: 10 bits
- air_temp: 11 bits
- air_humidity: 10 bits
- pressure: 14 bits
- light_lux: 19 bits

Total: 64 bits.

## Integrity Verification

Two verification modes are supported:
- `verify_hash_chain_links()`: validates hash-link continuity only,
- `verify_hash_chain_strong()`: validates both link continuity and SHA-256 reconstruction from `(prev_hash, node_id, timestamp, packed_data)`.

## How to Reproduce

1. Install dependencies:
   `pip install -r requirements.txt`

2. Run a simulation:
   `python simulation.py --seed 42 --output_dir results/run_seed_42`

3. Inspect the generated outputs in the selected results folder.

## Output Artifacts

Each run produces:
- `multi_node_simulation_summary.json`
- `multi_node_sensor_stream.csv`
- `multi_node_hash_chain.csv`
- `multi_node_hash_chain_tampered.csv`
- chart images
- DuckDB database file

## Multi-Run Validation

The repository includes repeated simulation runs across multiple independent random seeds to assess robustness of reconstruction accuracy, irrigation-alert behavior, and tamper detection reliability.

**Quantization:** Each sensor value is min–max normalized to its predefined range and mapped to an integer using fixed-width floor quantization:

$$q = \lfloor z (2^b - 1) \rfloor$$

**Reconstruction** is performed by inverse normalization, and numerical fidelity is evaluated using $\text{MAE}$.



### Reported Simulation 1 Results

| Variable | Bits | Quantization interval ($\Delta$) | Mean MAE |
| :--- | :---: | :---: | :---: |
| Soil moisture | 10 | 0.09775 % | 0.0487 % |
| Air temperature | 11 | 0.06106 $^{\circ}$C | 0.0305 $^{\circ}$C |
| Humidity | 10 | 0.09775 %RH | 0.0492 %RH |
| Pressure | 14 | 0.04883 mbar | 0.0245 mbar |
| Light | 19 | 0.38147 lux | 0.1918 lux |


