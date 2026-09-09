# AgroDBhive: Edge-Oriented Validation & Experimental Deployment

This repository contains the comprehensive simulation frameworks, experimental deployment artifacts, and validation data for **AgroDBhive**: an edge-oriented, bit-packed storage and data-integrity framework tailored for IoT sensor streams in smart agriculture.

The project evaluates structural robustness, performance benchmarking, data compaction ratios, and forensic chain-of-custody verification for high-frequency agricultural telemetry.

---

## 📂 Repository Structure

The repository is organized into four core pillars, each representing a distinct phase of the framework's validation:

### 1. `simulation_1` — Robustness & Statistical Validation
* **Purpose**: Evaluates fixed-width floor quantization and absolute reconstruction fidelity across multiple randomized iterations.
* **Key Components**:
  * `results/`: Contains repeated multi-node simulation checkpoints stored in local DuckDB databases (`.duckdb`) and sequential telemetry logs (`.csv`).
  * `figures/`: Graphical charts depicting Mean Absolute Error (MAE) metrics for soil moisture, air temperature, and humidity, alongside automated irrigation alarm triggers.
  * `paper_snippets/`: Generated LaTeX (`.tex`) structural tables and mathematical data blocks formatted directly for academic publication.

### 2. `simulation_2` — Data Compaction & Storage Efficiency
* **Purpose**: Measures the bit-packing framework's exact compression performance compared to standard storage architectures.
* **Key Components**:
  * Focuses on the 64-bit layout distribution schema (`soil_moisture: 10b`, `air_temp: 11b`, `air_humidity: 10b`, `pressure: 14b`, `light_lux: 19b`) and storage footprint evaluation.

### 3. `simulation_3` — Performance & Runtime Benchmarking
* **Purpose**: Contains isolated stress-testing and processing performance counters.
* **Key Components**:
  * `simulation3_v3_1.py`: Automated orchestration script designed to measure processing latency, ingestion throughput, and hash-chain link compilation times.
  * `results/benchmark_runs.csv`: Empirical execution speed results across scaled record pools.

### 4. `experimental_deployment` — Hardware & Forensic Evidence
* **Purpose**: Physical edge-gateway integration blueprints and validation from real-world IoT sensor networks.
* **Key Components**:
  * `firmware/`: Production-ready Arduino/ESP32 sketch files (`.ino`) implementing intermittent edge caching, UTC journal logging, and forensic-grade read-only access layers.
  * `forensic/`: Chain-of-custody audit summaries and cryptographic breakdown of data-tamper detection.
  * `logs/`: Cryptographic network ledger logs (`.log`) verifying SHA-256 reconstruction and verification bounds.

---

## 📊 Quantization & Error Bounds

To map high-precision physical telemetry into packed 64-bit packages, values are normalized and encoded via fixed-width floor quantization:

$$q = \left\lfloor z \cdot (2^b - 1) \right\rfloor$$

Where $z$ is the min-max normalized physical input, $b$ is the designated bit width, and $q$ represents the packed integer payload. Inverse normalization yields the reconstruction code $\hat{x}$, maintaining strict bounded mathematical error:

$$0 \le |x_i - \hat{x}_i| < \Delta$$

---

## 🛠️ Getting Started

### Prerequisites
Install the required environment dependencies before initiating data extraction or graph replication:
```bash
pip install -r simulation_1/requirements.txt
```

### Reproducing Benchmark Analysis
To run the performance metrics infrastructure within the engine suite, execute:
```bash
python simulation_3/simulation3_v3_1.py
```
