# Simulation 2 — Strong Integrity and Tamper Detection

This simulation evaluates the strong SHA-256 hash-chain verification
mechanism implemented in AgroDBhive.

## Experimental setup

- Platform: Raspberry Pi 4
- Number of nodes: 6
- Records per node: 720
- Total records: 4,320
- Random seed: 20260811
- Integrity mechanism: SHA-256 hash chain
- Verification input: node ID, timestamp, packed payload, and previous hash

## Verification procedure

A clean hash chain is first generated and verified.

The experiment then applies six independent attack classes:

1. Payload modification
2. Record reordering
3. Record deletion
4. Record injection
5. Node spoofing
6. Timestamp tampering

An attack is considered detected when the modified chain fails
cryptographic verification.

## Results

| Metric | Result |
|---|---:|
| Total records evaluated | 4,320 |
| Original chain valid | Yes |
| Verification latency | 279.6 ms |
| Attack classes evaluated | 6 |
| Attack classes detected | 6 |
| Detection rate | 100% |

All six evaluated attack classes were detected:

| Attack class | Detected |
|---|---|
| Payload modification | Yes |
| Record reordering | Yes |
| Record deletion | Yes |
| Record injection | Yes |
| Node spoofing | Yes |
| Timestamp tampering | Yes |

## Reproducibility

Run:

```bash
python simulation2_integrity.py --seed 20260811 --output_dir results








The experiment generates:

results/integrity_results.json






The experiment demonstrates that the implemented hash-chain
verification detects modifications affecting the payload, record
ordering, record presence, node identity, and timestamp information.

The result demonstrates integrity-detection capability for the
evaluated attack classes. It does not imply protection against all
possible attacks or universal security guarantees.




Reproducibility note

The random seed is explicitly reported to permit deterministic
reproduction of the evaluated test chain.