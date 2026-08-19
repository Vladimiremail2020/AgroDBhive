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

| Variable | Bits | Quantization interval (Δ) | Mean MAE |
| :--- | :---: | :---: | :---: |
| Soil moisture | 10 | 0.09775 % | 0.0487 % |
| Air temperature | 11 | 0.06106 °C | 0.0305 °C |
| Humidity | 10 | 0.09775 %RH | 0.0492 %RH |
| Pressure | 14 | 0.04883 mbar | 0.0245 mbar |
| Light | 19 | 0.38147 lux | 0.1918 lux |


The reported Simulation 1 MAE values are aggregated over 10 independent runs using seeds 0–200. Seed 42 is provided as a deterministic single-run reproduction example.




### Quantization and Reconstruction

Each simulated sensor variable is represented using a predefined physical range and a fixed bit width. The value is first normalized to the interval $[0, 1]$ and then quantized using floor-based fixed-width quantization.

For a sensor value $x$ with minimum and maximum values $x_{\min}$ and $x_{\max}$, and a bit width $b$, the normalized value is:

$$z = \frac{x - x_{\min}}{x_{\max} - x_{\min}}$$

The quantized integer representation is calculated as:

$$q = \lfloor z (2^b - 1) \rfloor$$

where $q$ is the resulting integer code and $b$ is the number of allocated bits.

The corresponding reconstructed value is obtained by inverse normalization:

$$\hat{x} = x_{\min} + \frac{q}{2^b - 1} (x_{\max} - x_{\min})$$

The nominal quantization interval is:

$$\Delta = \frac{x_{\max} - x_{\min}}{2^b - 1}$$

Reconstruction fidelity is evaluated using the Mean Absolute Error ($\text{MAE}$):

$$\text{MAE} = \frac{1}{N} \sum_{i=1}^{N} |x_i - \hat{x}_i|$$

where $x_i$ is the original simulated value and $\hat{x}_i$ is the reconstructed value after quantization and unpacking.

The reported $\text{MAE}$ therefore measures the numerical reconstruction error introduced by the fixed-width representation. It does not represent the absolute measurement accuracy of a physical sensor.

For the implemented floor-based quantizer, the reconstruction error of an individual value is bounded by the corresponding quantization interval:

$$0 \le |x_i - \hat{x}_i| < \Delta$$

The reported Simulation 1 results evaluate whether this bound is maintained across all simulated sensor variables and independent simulation runs.

