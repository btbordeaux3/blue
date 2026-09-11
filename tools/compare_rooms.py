#!/usr/bin/env python3
"""Compare RF signatures from room CSVs to judge room separability.

Usage:
    python tools/compare_rooms.py data/office_*.csv data/bedroom_*.csv

Prints per-room AP statistics, pairwise BSSID overlap, effect sizes for
shared BSSIDs, and leave-one-out nearest-centroid accuracy (a simple
pre-ML sanity check, not the real classifier).
"""

import csv
import math
import sys
from collections import defaultdict
from pathlib import Path

MISSING_RSSI = -100.0


def load(paths):
    rooms = defaultdict(dict)
    total_rows = 0
    for p in paths:
        with open(p, newline="") as fh:
            reader = csv.DictReader(fh)
            if not reader.fieldnames or "room" not in reader.fieldnames:
                sys.exit(f"{p}: missing expected CSV header")
            for row in reader:
                try:
                    rssi = float(row["rssi"])
                except (TypeError, ValueError):
                    continue
                room = row["room"].strip()
                key = (Path(p).name, row["timestamp"].strip())
                rooms[room].setdefault(key, {})[row["bssid"]] = rssi
                total_rows += 1
    return {r: [snap for _, snap in sorted(snaps.items())] for r, snaps in rooms.items()}, total_rows


def mean(xs):
    return sum(xs) / len(xs)


def stdev(xs):
    if len(xs) < 2:
        return 0.0
    m = mean(xs)
    return math.sqrt(sum((x - m) ** 2 for x in xs) / (len(xs) - 1))


def bssid_stats(snaps):
    values = defaultdict(list)
    for snap in snaps:
        for bssid, rssi in snap.items():
            values[bssid].append(rssi)
    stats = {}
    for bssid, vals in values.items():
        stats[bssid] = {
            "mean": mean(vals),
            "std": stdev(vals),
            "presence": len(vals) / len(snaps),
        }
    return stats


def vectors(snaps, bssids):
    return [[snap.get(b, MISSING_RSSI) for b in bssids] for snap in snaps]


def centroid(vecs):
    n = len(vecs)
    dim = len(vecs[0])
    return [sum(v[d] for v in vecs) / n for d in range(dim)]


def sq_dist(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b))


def loo_accuracy(data, bssids):
    all_vecs = {room: vectors(snaps, bssids) for room, snaps in data.items()}
    centroids = {room: centroid(vecs) for room, vecs in all_vecs.items()}
    correct = 0
    confusion = defaultdict(int)
    total = 0
    for true_room, vecs in all_vecs.items():
        for i, v in enumerate(vecs):
            others = [x for j, x in enumerate(vecs) if j != i]
            own_centroid = centroid(others) if others else None
            best_room, best_d = None, None
            for room in all_vecs:
                ref = own_centroid if room == true_room else centroids[room]
                d = sq_dist(v, ref)
                if best_d is None or d < best_d:
                    best_room, best_d = room, d
            total += 1
            if best_room == true_room:
                correct += 1
            else:
                confusion[(true_room, best_room)] += 1
    acc = correct / total if total else 0.0
    return acc, dict(confusion), centroids, all_vecs


def main():
    paths = sys.argv[1:]
    if len(paths) < 2:
        sys.exit("Pass at least two CSV files: compare_rooms.py fileA.csv fileB.csv ...")

    data, total_rows = load(paths)
    print(f"Loaded {total_rows} rows from {len(paths)} file(s)\n")

    all_stats = {}
    for room, snaps in sorted(data.items()):
        stats = bssid_stats(snaps)
        all_stats[room] = stats
        visible = mean([len(s) for s in snaps])
        print(f"[{room}] snapshots={len(snaps)} unique_bssids={len(stats)} avg_visible_aps={visible:.1f}")

    print("\n=== Pairwise comparison ===")
    room_names = sorted(data)
    shared_all = set.intersection(*(set(all_stats[r]) for r in room_names)) if len(room_names) > 1 else set()
    discriminative = []

    for i in range(len(room_names)):
        for j in range(i + 1, len(room_names)):
            ra, rb = room_names[i], room_names[j]
            sa, sb = set(all_stats[ra]), set(all_stats[rb])
            only_a, only_b, shared = sa - sb, sb - sa, sa & sb
            print(f"\n{ra} vs {rb}: shared={len(shared)} only_in_{ra}={len(only_a)} only_in_{rb}={len(only_b)}")

            for bssid in shared:
                va, vb = all_stats[ra][bssid], all_stats[rb][bssid]
                pooled = math.sqrt((va["std"] ** 2 + vb["std"] ** 2) / 2.0)
                cohens_d = abs(va["mean"] - vb["mean"]) / pooled if pooled > 0 else float("inf")
                delta = abs(va["mean"] - vb["mean"])
                if cohens_d >= 1.0 and min(va["presence"], vb["presence"]) >= 0.5:
                    discriminative.append((cohens_d, delta, bssid, ra, rb))

    if discriminative:
        print("\n=== Most discriminative shared BSSIDs (|Cohen's d| >= 1, present >=50%) ===")
        for d, delta, bssid, ra, rb in sorted(discriminative, reverse=True)[:15]:
            ma = all_stats[ra][bssid]["mean"]
            mb = all_stats[rb][bssid]["mean"]
            print(f"  {bssid}  d={d:5.2f}  {ra}:{ma:7.1f} dBm  {rb}:{mb:7.1f} dBm  (delta {delta:.1f})")
    else:
        print("\nNo shared BSSID shows a strong RSSI difference between rooms.")

    print("\n=== Leave-one-out nearest-centroid check ===")
    bssids = sorted(set().union(*(set(all_stats[r]) for r in room_names)))
    acc, confusion, _, _ = loo_accuracy(data, bssids)
    print(f"Accuracy: {acc * 100:.1f}% over {sum(len(s) for s in data.values())} snapshots")
    if confusion:
        print("Misclassifications (true -> predicted):")
        for (t, p), n in sorted(confusion.items()):
            print(f"  {t} -> {p}: {n}")

    print("\n=== Verdict ===")
    if acc >= 0.9:
        verdict = "STRONG separation. Proceed to feature engineering and a real classifier."
    elif acc >= 0.7:
        verdict = "PROMISING but noisy. Collect longer sessions and/or more rooms before ML."
    else:
        verdict = "WEAK separation. Check that sessions were stationary and try longer collections."
    print(verdict)


if __name__ == "__main__":
    main()
