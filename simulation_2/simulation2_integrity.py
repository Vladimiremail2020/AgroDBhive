#!/usr/bin/env python3
"""
AgroDBhive - Simulation 2: Strong Integrity and Tamper Detection
Evaluates the SHA-256 hash-chain verification mechanism against 6 distinct attack classes:
1. Payload modification
2. Record reordering
3. Record deletion
4. Record injection
5. Node spoofing
6. Timestamp tampering
"""

import os
import json
import hashlib
import time
from datetime import datetime, timedelta
from pathlib import Path
import numpy as np
import pandas as pd
import argparse

# Default setup for consistency
N_NODES = 6
ROWS_PER_NODE = 720  # 12 hours
START_DATE = datetime(2026, 1, 1, 0, 0, 0)

def compute_hash(node_id, timestamp, packed_data, prev_hash):
    """Compute SHA-256 hash matching the core engine logic."""
    timestamp_str = str(timestamp)
    data_str = f"{node_id}:{timestamp_str}:{packed_data}:{prev_hash}"
    return hashlib.sha256(data_str.encode()).hexdigest()

def generate_base_chain(seed):
    """Generates a strictly valid original hash chain for testing."""
    rng = np.random.RandomState(seed)
    hash_chain = []
    
    for node_id in range(1, N_NODES + 1):
        prev_hash = "0" * 64  # Genesis
        for i in range(ROWS_PER_NODE):
            timestamp = START_DATE + timedelta(minutes=i)
            # POPRAVLJENO: Dodat dtype=np.int64 kako ne bi pucalo na 32-bitnim granicama
            packed_data = int(rng.randint(0, 2**63 - 1, dtype=np.int64))
            current_hash = compute_hash(node_id, timestamp, packed_data, prev_hash)
            
            hash_chain.append({
                "node_id": node_id,
                "timestamp": timestamp,
                "current_hash": current_hash,
                "prev_hash": prev_hash,
                "packed_data": packed_data
            })
            prev_hash = current_hash
            
    return pd.DataFrame(hash_chain)

def verify_hash_chain(df):
    """
    Verifies hash chain integrity.
    Returns True if valid, False if tampered or broken.
    """
    for node_id in sorted(df["node_id"].unique()):
        node_chain = df[df["node_id"] == node_id].reset_index(drop=True)
        
        for i, row in node_chain.iterrows():
            if i == 0:
                expected_prev_hash = "0" * 64
            else:
                expected_prev_hash = node_chain.loc[i - 1, "current_hash"]
                
            # Test 1: Check cryptographic linkage continuity
            if row["prev_hash"] != expected_prev_hash:
                return False
                
            # Test 2: Recalculate hash on current row payload to verify data immutability
            recalculated_hash = compute_hash(
                row["node_id"],
                row["timestamp"],
                row["packed_data"],
                row["prev_hash"]
            )
            if row["current_hash"] != recalculated_hash:
                return False
                
    return True

# --- SIMULATION OF ATTACK CLASSES ---

def attack_payload_modification(df):
    """Attack 1: Modifying sensor readings inside a bit-packed payload slot."""
    tampered = df.copy()
    # Change the sensor payload of a record inside the chain
    tampered.loc[150, "packed_data"] = tampered.loc[150, "packed_data"] ^ 0b10101
    return not verify_hash_chain(tampered)

def attack_record_reordering(df):
    """Attack 2: Reordering two adjacent or distinct historical records."""
    tampered = df.copy()
    # Swap position of row 200 and 201
    idx1, idx2 = 200, 201
    row1, row2 = tampered.loc[idx1].copy(), tampered.loc[idx2].copy()
    tampered.loc[idx1], tampered.loc[idx2] = row2, row1
    return not verify_hash_chain(tampered)

def attack_record_deletion(df):
    """Attack 3: Dropping a transaction/record from the middle of the ledger stream."""
    tampered = df.copy().reset_index(drop=True)
    # Delete row at index 300
    tampered = tampered.drop(300).reset_index(drop=True)
    return not verify_hash_chain(tampered)

def attack_record_injection(df):
    """Attack 4: Inserting an unauthorized rogue row inside a valid node sequence."""
    tampered = df.copy().reset_index(drop=True)
    # POPRAVLJENO: Ispravljen poziv kopiranja pojedinačnog reda sa iloc
    rogue_row = tampered.iloc[400].copy()
    rogue_row["timestamp"] = rogue_row["timestamp"] + timedelta(seconds=30)
    
    # Slice and inject
    df_top = tampered.iloc[:400]
    df_bottom = tampered.iloc[400:]
    tampered = pd.concat([df_top, pd.DataFrame([rogue_row]), df_bottom]).reset_index(drop=True)
    return not verify_hash_chain(tampered)

def attack_node_spoofing(df):
    """Attack 5: Forging messages using a non-existent or unauthenticated node identity."""
    tampered = df.copy()
    # Spoof a malicious node ID onto a valid packet
    tampered.loc[500, "node_id"] = 99 
    return not verify_hash_chain(tampered)

def attack_timestamp_tampering(df):
    """Attack 6: Altering historical timestamps (Timestomping) to mask malicious delays."""
    tampered = df.copy()
    # Tamper with the timestamp of an existing block
    tampered.loc[120, "timestamp"] = tampered.loc[120, "timestamp"] - timedelta(hours=2)
    return not verify_hash_chain(tampered)

# --- MAIN AUTOMATION RUNNER ---

def parse_args():
    parser = argparse.ArgumentParser(description="Simulation 2 Runner - Tamper and Attack Detection")
    parser.add_argument("--seed", type=int, default=20260811, help="Seed for chain generation")
    parser.add_argument("--output_dir", type=str, default="results", help="JSON output directory")
    return parser.parse_args()

def main():
    args = parse_args()
    output_path = Path(args.output_dir)
    output_path.mkdir(parents=True, exist_ok=True)
    
    print("=" * 60)
    print("AgroDBhive: Executing Simulation 2 (Tamper Detection Suite)")
    print("=" * 60)
    
    # 1. Generate clean validation chain
    df_clean = generate_base_chain(args.seed)
    
    # Measure baseline latency
    start_time = time.perf_counter()
    originally_valid = verify_hash_chain(df_clean)
    end_time = time.perf_counter()
    latency_ms = (end_time - start_time) * 1000
    
    print(f"Baseline Verification Status: {originally_valid} (Expected: True)")
    print(f"Cryptographic Verification Latency: {latency_ms:.2f} ms")
    
    # 2. Fire the attack vectors and collect outcomes
    print("\nDeploying Attack Matrix...")
    results = {
        "payload_modification_detected": attack_payload_modification(df_clean),
        "reorder_attack_detected": attack_record_reordering(df_clean),
        "deletion_attack_detected": attack_record_deletion(df_clean),
        "injection_attack_detected": attack_record_injection(df_clean),
        "node_spoofing_detected": attack_node_spoofing(df_clean),
        "timestamp_tampering_detected": attack_timestamp_tampering(df_clean)
    }
    
    detected_count = sum(1 for status in results.values() if status is True)
    print(f"Attack Matrix Results: Successfully detected {detected_count} / 6 threat vectors.")
    
    # 3. Shape the JSON object for the reviewers
    json_output = {
        "simulation": "Simulation 2",
        "platform": "Raspberry Pi 4 / Reference Hardware",
        "dataset_seed": args.seed,
        "total_records_evaluated": len(df_clean),
        "chain_originally_valid": originally_valid,
        "verification_latency_ms": round(latency_ms, 2),
        "attack_detection_results": results,
        "detected_attack_classes": detected_count,
        "total_attack_classes": 6
    }
    
    # Save file
    file_target = output_path / "integrity_results.json"
    with open(file_target, "w") as f:
        json.dump(json_output, f, indent=2)
        
    print(f"\n   Integrity verification matrix exported to: {file_target.resolve()}")
    print("=" * 60 + "\n")

if __name__ == "__main__":
    main()
