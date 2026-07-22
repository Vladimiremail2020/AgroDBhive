#!/usr/bin/env python3
"""
Multi-Node Irrigation Simulation with DuckDB and Blockchain Hash Chain
Simulates 6 sensor nodes monitoring soil moisture, temperature, humidity, pressure, and light
Generates irrigation alarms when soil moisture drops below threshold
Stores data in DuckDB database with blockchain-style hash chain for integrity
"""

import os
import json
import hashlib
import struct
from datetime import datetime, timedelta
from pathlib import Path
import numpy as np
import pandas as pd
import duckdb
import matplotlib.pyplot as plt
import argparse


OUTPUT_DIR = None

# Simulation parameters
N_NODES = 6
ROWS_PER_NODE = 720
SEED = 42
START_DATE = datetime(2026, 1, 1, 0, 0, 0)

# Sensor bit layout for packing into 64-bit integer
BIT_LAYOUT = [
    {"name": "soil_moisture", "bits": 10, "scale": 10.0, "min_val": 0.0, "max_val": 100.0},
    {"name": "air_temp", "bits": 11, "scale": 10.0, "min_val": -40.0, "max_val": 85.0},
    {"name": "air_humidity", "bits": 10, "scale": 10.0, "min_val": 0.0, "max_val": 100.0},
    {"name": "pressure", "bits": 14, "scale": 10.0, "min_val": 300.0, "max_val": 1100.0},
    {"name": "light_lux", "bits": 19, "scale": 1.0, "min_val": 0.0, "max_val": 200000.0}
]

# Irrigation alarm threshold
SOIL_MOISTURE_THRESHOLD = 30.0

class SensorNode:
    """Simulates a single sensor node with realistic environmental readings"""

    def __init__(self, node_id, seed):
        self.node_id = node_id
        self.rng = np.random.RandomState(seed + node_id)

        # Base values for each node (simulate different field locations)
        self.base_soil_moisture = 40.0 + self.rng.uniform(-10, 20)
        self.base_temp = 20.0 + self.rng.uniform(-3, 5)
        self.base_humidity = 70.0 + self.rng.uniform(-10, 10)
        self.base_pressure = 1008.0 + self.rng.uniform(-5, 5)

    def generate_reading(self, timestamp_index):
        """Generate a single sensor reading with temporal variation"""
        # Time of day effects (simulates day/night cycles)
        hour = (timestamp_index / 60.0) % 24

        # Temperature varies with time of day
        temp_variation = 3 * np.sin(2 * np.pi * hour / 24)
        air_temp = self.base_temp + temp_variation + self.rng.normal(0, 1)

        # Soil moisture slowly decreases during day (evaporation)
        moisture_trend = -0.02 * timestamp_index / 60.0
        soil_moisture = self.base_soil_moisture + moisture_trend + self.rng.normal(0, 3)
        soil_moisture = np.clip(soil_moisture, 0, 100)

        # Humidity inversely related to temperature
        air_humidity = self.base_humidity - temp_variation * 0.5 + self.rng.normal(0, 3)
        air_humidity = np.clip(air_humidity, 0, 100)

        # Pressure slowly varies
        pressure = self.base_pressure + self.rng.normal(0, 1)

        # Light varies strongly with time of day
        if 6 <= hour <= 18:
            light_lux = 50000 + 30000 * np.sin(np.pi * (hour - 6) / 12) + self.rng.uniform(-5000, 5000)
        else:
            light_lux = self.rng.uniform(0, 1000)

        # Irrigation alarm trigger when soil moisture is low
        irrigation_alarm = 1 if soil_moisture < SOIL_MOISTURE_THRESHOLD else 0

        return {
            "soil_moisture": soil_moisture,
            "air_temp": air_temp,
            "air_humidity": air_humidity,
            "pressure": pressure,
            "light_lux": light_lux,
            "irrigation_alarm": irrigation_alarm
        }


def pack_sensor_data(reading):
    """Pack sensor readings into a 64-bit integer"""
    packed = 0
    bit_offset = 0

    for field in BIT_LAYOUT:
        name = field["name"]
        bits = field["bits"]
        scale = field["scale"]
        min_val = field["min_val"]
        max_val = field["max_val"]

        value = reading[name]
        # Normalize to [0, 1] range
        normalized = (value - min_val) / (max_val - min_val)
        normalized = np.clip(normalized, 0, 1)

        # Quantize to n bits
        max_int = (1 << bits) - 1
        quantized = int(normalized * max_int)

        # Pack into 64-bit integer
        packed |= (quantized << bit_offset)
        bit_offset += bits

    return packed


def unpack_sensor_data(packed):
    """Unpack 64-bit integer back to sensor readings"""
    unpacked = {}
    bit_offset = 0

    for field in BIT_LAYOUT:
        name = field["name"]
        bits = field["bits"]
        scale = field["scale"]
        min_val = field["min_val"]
        max_val = field["max_val"]

        
        mask = (1 << bits) - 1
        quantized = (packed >> bit_offset) & mask

       
        max_int = (1 << bits) - 1
        normalized = quantized / max_int
        value = normalized * (max_val - min_val) + min_val

        unpacked[name] = value
        bit_offset += bits

    return unpacked


def compute_hash(node_id, timestamp, packed_data, prev_hash):
    """Compute SHA-256 hash for blockchain chain"""
    timestamp_str = str(timestamp)
    data_str = f"{node_id}:{timestamp_str}:{packed_data}:{prev_hash}"
    return hashlib.sha256(data_str.encode()).hexdigest()


def generate_simulation_data(seed):
    """Generate multi-node sensor simulation data"""
   
    print("Multi-Node Irrigation Simulation")
   

    nodes = [SensorNode(i + 1, seed) for i in range(N_NODES)]
    all_data = []
    hash_chain = []

    # Generate data for each node
    for node_idx, node in enumerate(nodes):
        print(f"\nGenerating data for Node {node.node_id}...")
        prev_hash = "0" * 64

        for i in range(ROWS_PER_NODE):
            timestamp = START_DATE + timedelta(minutes=i)
            reading = node.generate_reading(i)
            packed = pack_sensor_data(reading)

            # Compute hash for blockchain chain
            current_hash = compute_hash(node.node_id, timestamp, packed, prev_hash)

            
            row = {
                "ts": timestamp,
                "node_id": node.node_id,
                "soil_moisture": reading["soil_moisture"],
                "air_temp": reading["air_temp"],
                "air_humidity": reading["air_humidity"],
                "pressure": reading["pressure"],
                "light_lux": reading["light_lux"],
                "irrigation_alarm": reading["irrigation_alarm"],
                "packed": packed
            }
            all_data.append(row)

            hash_chain.append({
                "node_id": node.node_id,
                "timestamp": timestamp,
                "current_hash": current_hash,
                "prev_hash": prev_hash,
                "packed_data": packed
            })

            prev_hash = current_hash

    print(f"\n✓ Generated {len(all_data)} sensor readings across {N_NODES} nodes")
    return pd.DataFrame(all_data), pd.DataFrame(hash_chain)


def verify_hash_chain(hash_chain_df):
    """Verify integrity of blockchain hash chain using both link continuity
    and payload-based hash reconstruction."""
    print("\nVerifying blockchain hash chain...")

    for node_id in sorted(hash_chain_df["node_id"].unique()):
        node_chain = hash_chain_df[hash_chain_df["node_id"] == node_id].reset_index(drop=True)

        for i, row in node_chain.iterrows():
            if i == 0:
                expected_prev_hash = "0" * 64
            else:
                expected_prev_hash = node_chain.loc[i - 1, "current_hash"]

            if row["prev_hash"] != expected_prev_hash:
                print(f"  ✗ Hash chain link broken at Node {node_id}, position {i}")
                return False

            expected_current_hash = compute_hash(
                row["node_id"],
                row["timestamp"],
                row["packed_data"],
                row["prev_hash"]
            )

            if row["current_hash"] != expected_current_hash:
                print(f"  ✗ Payload/hash mismatch at Node {node_id}, position {i}")
                return False

    print("  ✓ Hash chain verified successfully")
    return True


def create_duckdb_database(df):
    """Store simulation data in DuckDB database"""
    db_path = OUTPUT_DIR / "agrodbhive_multi_node.duckdb"

    print(f"\nCreating DuckDB database: {db_path}")

    # Connect to DuckDB
    conn = duckdb.connect(str(db_path))

    # Create table
    conn.execute("DROP TABLE IF EXISTS sensor_stream")
    conn.execute("""
        CREATE TABLE sensor_stream (
            ts TIMESTAMP,
            node_id INTEGER,
            soil_moisture DOUBLE,
            air_temp DOUBLE,
            air_humidity DOUBLE,
            pressure DOUBLE,
            light_lux DOUBLE,
            irrigation_alarm INTEGER,
            packed BIGINT
        )
    """)

    # Insert data
    conn.execute("INSERT INTO sensor_stream SELECT * FROM df")

    row_count = conn.execute("SELECT COUNT(*) FROM sensor_stream").fetchone()[0]
    print(f"  ✓ Inserted {row_count} rows into sensor_stream table")

    conn.close()
    return db_path


def generate_visualizations(df):
   
    print("\nGenerating visualizations...")

    df_subset = df.head(300 * N_NODES)
    avg_moisture = df_subset.groupby(df_subset.index // N_NODES)["soil_moisture"].mean()

    plt.figure(figsize=(10, 6))
    plt.plot(avg_moisture.values, linewidth=2, color='blue')
    plt.axhline(y=SOIL_MOISTURE_THRESHOLD, color='red', linestyle='--',
                label=f'Irrigation Threshold ({SOIL_MOISTURE_THRESHOLD}%)')
    plt.xlabel("Timestamp Index")
    plt.ylabel("Soil Moisture (%)")
    plt.title("Average Soil Moisture Across Simulated Nodes")
    plt.legend()
    plt.grid(True, alpha=0.3)
    plt.tight_layout()

    chart1_path = OUTPUT_DIR / "chart_avg_soil_moisture.png"
    plt.savefig(chart1_path, dpi=150)
    plt.close()
    print(f"  ✓ Saved: {chart1_path}")

    
    with open(chart1_path.with_suffix(".png.meta.json"), "w") as f:
        json.dump({
            "caption": "Average soil moisture across simulated nodes",
            "description": "Line chart of average soil moisture over the first 300 timestamps in the synthetic multi-node simulation."
        }, f, indent=2)



    alarm_counts = df.groupby("node_id")["irrigation_alarm"].sum()

    plt.figure(figsize=(8, 6))
    plt.bar(alarm_counts.index, alarm_counts.values, color='orange', edgecolor='black')
    plt.xlabel("Node ID")
    plt.ylabel("Irrigation Alarm Count")
    plt.title("Irrigation Alarm Counts by Node")
    plt.xticks(alarm_counts.index)
    plt.grid(True, alpha=0.3, axis='y')
    plt.tight_layout()

    chart2_path = OUTPUT_DIR / "chart_irrigation_alarms.png"
    plt.savefig(chart2_path, dpi=150)
    plt.close()
    print(f"  ✓ Saved: {chart2_path}")

    
    with open(chart2_path.with_suffix(".png.meta.json"), "w") as f:
        json.dump({
            "caption": "Irrigation alarm counts by node",
            "description": "Bar chart of irrigation alarm events per simulated sensor node."
        }, f, indent=2)


def save_simulation_summary(df, hash_chain_df, seed):
    """Save simulation summary as JSON"""
    alarm_counts = df.groupby("node_id")["irrigation_alarm"].sum().to_dict()

    # Verify original chain
    original_chain_valid = verify_hash_chain(hash_chain_df)

    # Create tampered chain for testing
    tampered_chain = hash_chain_df.copy()
    if len(tampered_chain) > 100:
        tampered_chain.loc[100, "packed_data"] = 999999999  # Tamper with data
    tampered_chain_valid = verify_hash_chain(tampered_chain)

    # Compute reconstruction MAE
    reconstruction_mae = {}
    for field in BIT_LAYOUT:
        name = field["name"]
        original_values = df[name].values

        # Reconstruct from packed data
        reconstructed = []
        for packed in df["packed"].values:
            unpacked = unpack_sensor_data(packed)
            reconstructed.append(unpacked[name])

        mae = np.mean(np.abs(original_values - np.array(reconstructed)))
        reconstruction_mae[name] = mae

    summary = {
        "n_nodes": N_NODES,
        "rows_per_node": ROWS_PER_NODE,
        "total_rows": len(df),
        "seed": seed,
        "bit_layout": BIT_LAYOUT,
        "total_bits": sum(f["bits"] for f in BIT_LAYOUT),
        "original_chain_valid": original_chain_valid,
        "tampered_chain_valid": tampered_chain_valid,
        "alarm_counts": [{"node_id": k, "alarm_count": v} for k, v in alarm_counts.items()],
        "reconstruction_mae": [{"field": k, "mae": v} for k, v in reconstruction_mae.items()]
    }

    summary_path = OUTPUT_DIR / "multi_node_simulation_summary.json"
    with open(summary_path, "w") as f:
        json.dump(summary, f, indent=2)

    print(f"\n✓ Saved simulation summary: {summary_path}")


def save_csv_exports(df, hash_chain_df):
    
    print("\nExporting CSV files...")

    
    csv_path = OUTPUT_DIR / "multi_node_sensor_stream.csv"
    df.to_csv(csv_path, index=False)
    print(f"  ✓ Saved: {csv_path}")

    # Export original hash chain
    chain_path = OUTPUT_DIR / "multi_node_hash_chain.csv"
    hash_chain_df.to_csv(chain_path, index=False)
    print(f"  ✓ Saved: {chain_path}")

    # Export tampered chain for comparison
    tampered_chain = hash_chain_df.copy()
    if len(tampered_chain) > 100:
        tampered_chain.loc[100, "packed_data"] = 999999999

    tampered_path = OUTPUT_DIR / "multi_node_hash_chain_tampered.csv"
    tampered_chain.to_csv(tampered_path, index=False)
    print(f"  ✓ Saved: {tampered_path}")







def parse_args():
    parser = argparse.ArgumentParser(
        description="Multi-node irrigation simulation with DuckDB and strong hash-chain verification"
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=42,
        help="Random seed for simulation reproducibility (default: 42)"
    )
    parser.add_argument(
        "--output_dir",
        type=str,
        default="output",
        help="Directory where simulation results will be saved"
    )
    return parser.parse_args()






def generate_latex_snippet(df):
    
    print("\nGenerating LaTeX snippet for paper...")

    
    sample_df = df.head(10)[["ts", "node_id", "soil_moisture", "air_temp",
                             "irrigation_alarm", "packed"]]

    latex_snippet = r"""
\begin{table}[h]
\centering
\caption{Sample Multi-Node Sensor Data with Irrigation Alarms}
\label{tab:simulation_sample}
\begin{tabular}{|c|c|c|c|c|c|}
\hline
\textbf{Timestamp} & \textbf{Node} & \textbf{Soil (\%)} & \textbf{Temp (°C)} & \textbf{Alarm} & \textbf{Packed} \\
\hline
"""

    for _, row in sample_df.iterrows():
        ts_str = row["ts"].strftime("%H:%M")
        latex_snippet += f"{ts_str} & {row['node_id']} & {row['soil_moisture']:.1f} & "
        latex_snippet += f"{row['air_temp']:.1f} & {row['irrigation_alarm']} & {row['packed']} \\\\\n"

    latex_snippet += r"""\hline
\end{tabular}
\end{table}
"""

    latex_path = OUTPUT_DIR / "simulation_experiment_snippet.tex"
    with open(latex_path, "w") as f:
        f.write(latex_snippet)

    print(f"  ✓ Saved: {latex_path}")


def main():
    global OUTPUT_DIR

   
    args = parse_args()

   
    OUTPUT_DIR = Path(args.output_dir)
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    print("\n" + "=" * 60)
    print("STARTING MULTI-NODE IRRIGATION SIMULATION")
    print("=" * 60)
    print(f"Seed prosleđen iz CLI: {args.seed}")
    print(f"Output directory:      {OUTPUT_DIR.resolve()}")

    
    df, hash_chain_df = generate_simulation_data(args.seed)

    
    db_path = create_duckdb_database(df)
    generate_visualizations(df)
    save_simulation_summary(df, hash_chain_df, args.seed)
    save_csv_exports(df, hash_chain_df)
    generate_latex_snippet(df)

    print("\n" + "=" * 60)
    print("SIMULATION COMPLETE")
    print("=" * 60)
    print(f"\nAll outputs successfully saved to: {OUTPUT_DIR.absolute()}")




if __name__ == "__main__":
    main()