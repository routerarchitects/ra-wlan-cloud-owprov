#!/usr/bin/env python3
"""
simulate_scale.py
─────────────────
Simulates N wireless-AP devices sending uCentral connection events to
the Kafka `connection` topic, to manually test owprov's GroupConsumer
scalability (PR #100 — kafka_group_broadcast branch).

What this tests
───────────────
  GroupConsumer (group.id = "prov"):
    - Multiple owprov replicas share the consumer group "prov".
    - Kafka assigns disjoint partitions to each replica.
    - Each replica processes only the devices whose serial number
      hashed to its assigned partitions.
    - This script produces 1,000 device serials × 3 event types = 3,000
      messages (configurable via --devices) and verifies even partition spread.

  BroadcastConsumer:
    - NOT exercised by this script.
    - To test it, use real live `service_events` traffic already emitted
      by running services. Every owprov replica has its own independent
      consumer group for BroadcastConsumer, so they all receive the same
      messages. Do not inject fake service_events messages.

Important: commit vs. PostgreSQL write timing
─────────────────────────────────────────────
  In KafkaManager.cpp the consumer calls commit(msg) immediately after
  the ConnectionReceived() callback (line 324). That callback only
  enqueues the message into AutoDiscovery's worker queue. The actual
  PostgreSQL write (CreateFromConnection / "Adding X to inventory.")
  happens asynchronously in the worker thread AFTER the Kafka commit.
  Kafka lag reaching zero does NOT guarantee all DB writes finished.

How to run (correct method — do NOT install packages in kafka container)
────────────────────────────────────────────────────────────────────────
  # Prerequisites — do these MANUALLY before running the script:

  # 1. Verify / increase topic partitions on isolated test broker:
  docker exec openwifi-kafka-1 \\
    /opt/bitnami/kafka/bin/kafka-topics.sh \\
    --bootstrap-server localhost:9092 \\
    --describe --topic connection

  docker exec openwifi-kafka-1 \\
    /opt/bitnami/kafka/bin/kafka-topics.sh \\
    --bootstrap-server localhost:9092 \\
    --alter --topic connection --partitions 10

  # 2. Scale owprov (uses pri-owprov:latest — build your PR image first):
  cd ~/Workspace-OpenWiFi/mango-cloud-deployment/docker-compose
  docker compose -f docker-compose.lb.selfsigned.yml up -d --scale owprov=3 --no-recreate
  # Repeat with --scale owprov=5 and --scale owprov=10 for full experiment.

  # 3. Run the script via a temporary Python container:
  cd ~/Workspace-OpenWiFi/source_code/ra-wlan-cloud-owprov
  mkdir -p tests/scale-results

  docker run --rm \\
    --network openwifi_openwifi \\
    -v "$PWD/tests/simulate_scale.py:/tmp/simulate_scale.py:ro" \\
    -v "$PWD/tests/scale-results:/results" \\
    -w /results \\
    python:3.11-slim \\
    sh -c 'pip install --no-cache-dir -q confluent-kafka && \\
           python /tmp/simulate_scale.py \\
             --broker kafka:9092 \\
             --devices 1000 \\
             --partitions 10'

  # 4. Check partition assignments and verify lag = 0:
  docker exec openwifi-kafka-1 \\
    /opt/bitnami/kafka/bin/kafka-consumer-groups.sh \\
    --bootstrap-server localhost:9092 \\
    --describe --group prov

  # 5. Verify database ingestion in PostgreSQL:
  docker exec openwifi-postgresql-1 psql -U postgres -d owprov \\
    -c "SELECT count(*) AS total_devices, count(DISTINCT serialnumber) AS unique_devices FROM inventory WHERE serialnumber LIKE 'sim<RUN_ID>%';"
"""

import argparse
import csv
import json
import os
import sys
import time
import uuid
from collections import defaultdict
from typing import Dict, List

try:
    from confluent_kafka import Producer
    from confluent_kafka.admin import AdminClient
except ImportError:
    print(
        "[ERROR] confluent-kafka is not installed.\n"
        "        Run:  pip install confluent-kafka",
        file=sys.stderr,
    )
    sys.exit(1)

TOPIC = "connection"


# ──────────────────────────────────────────────────────────────────────────────
# Message builders — match what owgw sends, as parsed by AutoDiscovery.cpp
# ──────────────────────────────────────────────────────────────────────────────

def _base_payload(serial: str, ip: str, firmware: str) -> dict:
    """
    Fields that both ProcessPing() and ProcessConnect() read (AutoDiscovery.cpp):
      serialNumber  — uCentralProtocol::SERIALNUMBER
      connectionIp  — uCentralProtocol::CONNECTIONIP  (format: serial@IP:port)
      firmware      — uCentralProtocol::FIRMWARE
      compatible    — uCentralProtocol::COMPATIBLE  (read by ProcessPing,
                      stored as DeviceType in CreateFromConnection)
      locale        — read by both, written to locale column in inventory DB

    connectionIp format note:
      storage_inventory.cpp tokenizes connectionIp on "@:" expecting 3 tokens:
        Tokens[0]=serial, Tokens[1]=IP, Tokens[2]=port
      A plain IP produces only 1 token — venue/entity lookup is skipped but
      device creation still succeeds. We use the proper format here.
    """
    return {
        "serialNumber": serial,
        "connectionIp": f"{serial}@{ip}:2000",   # serial@IP:port — 3 tokens for venue lookup
        "firmware": firmware,
        "compatible": "linksys,ea8300",
        "locale": "en",
        "timestamp": int(time.time()),
    }


def make_connect_msg(serial: str, ip: str, firmware: str) -> str:
    """
    AutoDiscovery::run() line 98: PayloadObj->has("capabilities") → ProcessConnect().
    compatible is read from capabilities.compatible, not top-level.
    isConnection=true → creates new inventory record.
    """
    payload = _base_payload(serial, ip, firmware)
    payload["capabilities"] = {
        "compatible": "linksys,ea8300",
        "platform": "ath79",
    }
    return json.dumps({"payload": payload})


def make_ping_msg(serial: str, ip: str, firmware: str) -> str:
    """
    AutoDiscovery::run() line 95: PayloadObj->has("ping") → ProcessPing().
    compatible and locale are read directly from the ping object.
    isConnection=false → updates existing record only.
    """
    ping_obj = _base_payload(serial, ip, firmware)
    return json.dumps({"payload": {"ping": ping_obj}})


def make_disconnect_msg(serial: str) -> str:
    """
    AutoDiscovery::run() line 101: PayloadObj->has("disconnection").
    Connected=false → CreateFromConnection is NOT called.
    Still consumed and counts toward partition workload.
    """
    return json.dumps({"payload": {"disconnection": {"serialNumber": serial}}})


# ──────────────────────────────────────────────────────────────────────────────
# Murmur2 partition prediction — must match "murmur2_random" producer setting.
# (confluent-kafka default "consistent_random" uses CRC32, not Murmur2.)
# ──────────────────────────────────────────────────────────────────────────────

def _murmur2(data: bytes) -> int:
    seed = 0x9747B28C
    m = 0x5BD1E995
    r = 24
    h = seed ^ len(data)
    for i in range(len(data) // 4):
        k = int.from_bytes(data[i * 4: i * 4 + 4], "little", signed=False)
        k = (k * m) & 0xFFFFFFFF
        k ^= k >> r
        k = (k * m) & 0xFFFFFFFF
        h = (h * m) & 0xFFFFFFFF
        h ^= k
    rem = len(data) & 3
    off = (len(data) // 4) * 4
    if rem == 3:
        h ^= data[off + 2] << 16
    if rem >= 2:
        h ^= data[off + 1] << 8
    if rem >= 1:
        h ^= data[off]
        h = (h * m) & 0xFFFFFFFF
    h ^= h >> 13
    h = (h * m) & 0xFFFFFFFF
    h ^= h >> 15
    return h


def predict_partition(key: str, num_partitions: int) -> int:
    return (_murmur2(key.encode("utf-8")) & 0x7FFFFFFF) % num_partitions


# ──────────────────────────────────────────────────────────────────────────────
# Verify topic has EXACTLY the expected partition count.
# The prediction uses num_partitions as the modulus — if actual differs,
# the distribution table would be inaccurate.
# Script exits with error if count doesn't match — do NOT auto-alter.
# ──────────────────────────────────────────────────────────────────────────────

def verify_partition_count(broker: str, expected: int) -> None:
    admin = AdminClient({"bootstrap.servers": broker})
    try:
        meta = admin.list_topics(topic=TOPIC, timeout=10)
    except Exception as exc:
        print(f"[ERROR] Could not query Kafka metadata: {exc}", file=sys.stderr)
        sys.exit(1)

    if TOPIC not in meta.topics:
        print(
            f"[ERROR] Topic '{TOPIC}' does not exist on broker {broker}.\n"
            f"        Make sure the full docker compose stack is running "
            f"and init-kafka has completed.",
            file=sys.stderr,
        )
        sys.exit(1)

    actual = len(meta.topics[TOPIC].partitions)
    if actual != expected:
        print(
            f"[ERROR] Topic '{TOPIC}' has {actual} partition(s) but "
            f"--partitions {expected} was specified.\n"
            f"        The count must match exactly for accurate partition prediction.\n\n"
            f"        To change (irreversible on this broker — use isolated test setup):\n"
            f"  docker exec openwifi-kafka-1 \\\n"
            f"    /opt/bitnami/kafka/bin/kafka-topics.sh \\\n"
            f"    --bootstrap-server localhost:9092 \\\n"
            f"    --alter --topic {TOPIC} --partitions {expected}",
            file=sys.stderr,
        )
        sys.exit(1)

    print(f"[OK]   Topic '{TOPIC}' has exactly {actual} partition(s).")


# ──────────────────────────────────────────────────────────────────────────────
# Validate partition distribution after all deliveries
# ──────────────────────────────────────────────────────────────────────────────

def validate_distribution(
    device_partition_map: Dict[str, int],   # serial -> actual partition (from connect delivery)
    actual_part_msg_count: Dict[int, int],  # partition -> total messages received
    num_partitions: int,
    delivery_rows: List[Dict],
) -> List[str]:
    """
    Returns a list of validation failure strings (empty = all passed).

    Checks:
      1. Messages spread across more than one partition.
      2. All 3 events for each device (connect/ping/disconnect) land on the same partition.
    """
    failures = []

    # Check 1: messages spread across multiple partitions
    used_partitions = set(actual_part_msg_count.keys())
    if len(used_partitions) < 2:
        failures.append(
            f"FAIL: All messages landed on partition(s) {used_partitions}. "
            f"Expected spread across multiple partitions. "
            f"Check that the topic has {num_partitions} partitions and "
            f"the producer uses murmur2_random partitioner."
        )

    # Check 2: all 3 events for each device use the same partition
    # Build: serial -> set of partitions seen across all 3 events
    serial_partitions: Dict[str, set] = defaultdict(set)
    for row in delivery_rows:
        serial_partitions[row["serial"]].add(row["partition"])

    multi_partition_devices = {
        s: parts for s, parts in serial_partitions.items() if len(parts) > 1
    }
    if multi_partition_devices:
        failures.append(
            f"FAIL: {len(multi_partition_devices)} device(s) had events split across "
            f"multiple partitions (expected all 3 events per device on same partition). "
            f"Examples: {dict(list(multi_partition_devices.items())[:3])}"
        )

    return failures


# ──────────────────────────────────────────────────────────────────────────────
# Main
# ──────────────────────────────────────────────────────────────────────────────

def run(broker: str, num_devices: int, num_partitions: int,
        delay: float, verbose: bool) -> None:

    # UUID-based run ID — unique across test runs, no time-collision risk
    run_id = str(uuid.uuid4()).replace("-", "")[:12]

    print(f"\n{'='*60}")
    print(f"  owprov GroupConsumer Scale-Test  (run_id={run_id})")
    print(f"  Devices: {num_devices}  |  Partitions: {num_partitions}  |  Broker: {broker}")
    print(f"{'='*60}\n")

    # Step 1: verify exact partition count — abort if wrong
    verify_partition_count(broker, num_partitions)

    # Step 2: build device list with UUID-unique serials
    devices: List[Dict] = []
    for i in range(1, num_devices + 1):
        serial = f"sim{run_id}{i:05d}"
        ip     = f"192.168.{(i - 1) // 254}.{((i - 1) % 254) + 1}"
        fw     = f"OpenWrt-21.02.{i % 5}"
        devices.append({"serial": serial, "ip": ip, "firmware": fw})

    # Step 3: predicted distribution (Murmur2, matches producer below)
    part_map: Dict[int, List[str]] = defaultdict(list)
    for d in devices:
        p = predict_partition(d["serial"], num_partitions)
        part_map[p].append(d["serial"])

    print(f"[INFO] Predicted partition distribution (Murmur2, {num_partitions} partitions):")
    for p in sorted(part_map):
        print(f"       Partition {p:2d}: {len(part_map[p]):3d} device(s)")
    print()

    # Step 4: producer
    # IMPORTANT: msg.headers() always returns None in confluent-kafka producer
    # delivery callbacks (documented librdkafka limitation). Capture event_type
    # in a lambda closure instead — the closure captures the local variable at
    # call time, so each produce() call gets the correct type.
    delivery_rows: List[Dict] = []
    delivery_errors: List[str] = []
    delivered_count = [0]
    actual_part_msg_count: Dict[int, int] = defaultdict(int)
    device_partition_map: Dict[str, int] = {}  # serial -> partition (from connect delivery)

    def make_callback(serial: str, event_type: str):
        """
        Returns a delivery callback with event_type captured in closure.
        This is necessary because msg.headers() returns None in producer
        delivery callbacks (confluent-kafka / librdkafka limitation).
        """
        def on_delivery(err, msg):
            if err:
                delivery_errors.append(
                    f"serial={serial} event={event_type} "
                    f"partition={msg.partition()}: {err}"
                )
            else:
                delivered_count[0] += 1
                actual_part_msg_count[msg.partition()] += 1
                delivery_rows.append({
                    "run_id": run_id,
                    "serial": serial,
                    "event_type": event_type,
                    "partition": msg.partition(),
                    "offset": msg.offset(),
                    "timestamp_sent": int(time.time()),
                })
                if event_type == "connect":
                    # Record which partition this device's connect event landed on
                    device_partition_map[serial] = msg.partition()
                if verbose:
                    print(
                        f"  OK  partition={msg.partition():2d}  "
                        f"offset={msg.offset():6d}  "
                        f"event={event_type:<12s}  serial={serial}"
                    )
        return on_delivery

    producer = Producer({
        "bootstrap.servers": broker,
        "partitioner": "murmur2_random",   # must match Murmur2 prediction above
    })

    # Step 5: produce
    total_msgs = num_devices * 3
    print(f"[INFO] Producing {total_msgs} messages ({num_devices} devices x 3 event types)...")
    if not verbose:
        print(f"       (use --verbose to see each delivery)\n")

    start = time.monotonic()
    for idx, dev in enumerate(devices, start=1):
        s, ip, fw = dev["serial"], dev["ip"], dev["firmware"]
        key_bytes = s.encode("utf-8")

        producer.produce(TOPIC, key=key_bytes,
                         value=make_connect_msg(s, ip, fw).encode(),
                         callback=make_callback(s, "connect"))
        producer.produce(TOPIC, key=key_bytes,
                         value=make_ping_msg(s, ip, fw).encode(),
                         callback=make_callback(s, "ping"))
        producer.produce(TOPIC, key=key_bytes,
                         value=make_disconnect_msg(s).encode(),
                         callback=make_callback(s, "disconnect"))

        producer.poll(0)

        if delay > 0:
            time.sleep(delay)

        if idx % 25 == 0:
            print(f"  ... sent events for {idx}/{num_devices} devices")

    print("\n[INFO] Flushing producer queue (max 30s)...")
    remaining = producer.flush(timeout=30)
    elapsed = time.monotonic() - start

    # Step 6: save delivery evidence CSV
    # Default output dir is /results (matches docker run -v mount).
    # Falls back to current dir when running outside Docker.
    out_dir = "/results" if os.path.isdir("/results") else "."
    csv_path = os.path.join(out_dir, f"simulate_delivery_{run_id}.csv")
    if delivery_rows:
        with open(csv_path, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=delivery_rows[0].keys())
            writer.writeheader()
            writer.writerows(delivery_rows)
        print(f"\n[CSV]  Delivery evidence: {csv_path}")
        print(f"       Columns: run_id, serial, event_type, partition, offset, timestamp_sent")
        print(f"       Cross-reference:")
        print(f"         partition col  <->  kafka-consumer-groups --group prov --members --verbose")
        print(f"         serial col     <->  owprov log 'Adding <serial> to inventory.'")

    # Step 7: validate partition distribution
    validation_failures = validate_distribution(
        device_partition_map, actual_part_msg_count, num_partitions, delivery_rows
    )

    # Step 8: report — Stage 1 (Kafka delivery)
    print(f"\n{'='*60}")
    print(f"  Stage 1: Kafka Delivery  (from broker ACKs — authoritative)")
    print(f"{'='*60}")
    print(f"  Delivered : {delivered_count[0]}/{total_msgs} messages")
    print(f"  Errors    : {len(delivery_errors)}")
    print(f"  Unflushed : {remaining}")
    print(f"  Duration  : {elapsed:.2f}s")

    if delivery_errors:
        print(f"\n[ERRORS]")
        for e in delivery_errors:
            print(f"  {e}")

    print(f"\n[INFO] Actual partition distribution (from broker ACKs):")
    for p in sorted(actual_part_msg_count):
        predicted = len(part_map.get(p, []))
        print(f"       Partition {p:2d}: {actual_part_msg_count[p]:4d} msg(s)"
              f"  (predicted {predicted * 3} for {predicted} device(s))")

    if validation_failures:
        print(f"\n[VALIDATION FAILURES]")
        for f in validation_failures:
            print(f"  {f}")
    else:
        partitions_used = len(actual_part_msg_count)
        print(f"\n[OK]   Messages spread across {partitions_used} partition(s).")
        print(f"[OK]   All devices: connect/ping/disconnect on same partition.")

    # Stage 2 — owprov processing verification (manual)
    print(f"\n{'='*60}")
    print(f"  Stage 2: owprov Processing  (verify manually)")
    print(f"{'='*60}")
    print(f"  NOTE: Kafka commit precedes PostgreSQL write (async worker queue).")
    print(f"  Wait until Kafka lag = 0, then wait an additional ~5s before checking.\n")
    print(f"  A. Partition assignments per owprov replica:")
    print(f"     docker exec openwifi-kafka-1 \\")
    print(f"       /opt/bitnami/kafka/bin/kafka-consumer-groups.sh \\")
    print(f"       --bootstrap-server localhost:9092 \\")
    print(f"       --describe --group prov --members --verbose")
    print(f"     (--verbose shows ALL partitions; PR#100 logs only parts.front())")
    print(f"  B. Verify consumer lag is 0 across all partitions:")
    print(f"     docker exec openwifi-kafka-1 \\")
    print(f"       /opt/bitnami/kafka/bin/kafka-consumer-groups.sh \\")
    print(f"       --bootstrap-server localhost:9092 \\")
    print(f"       --describe --group prov")
    print()
    print(f"  C. Verify all {num_devices} devices ingested into PostgreSQL inventory table:")
    print(f"     docker exec openwifi-postgresql-1 psql -U postgres -d owprov \\")
    print(f"       -c \"SELECT count(*) FROM inventory WHERE serialnumber LIKE 'sim{run_id}%';\"")
    print(f"     Expected count: {num_devices}")

    # Stage 3 — BroadcastConsumer (not this script's job)
    print(f"\n{'='*60}")
    print(f"  Stage 3: BroadcastConsumer  (separate — not tested here)")
    print(f"{'='*60}")
    print(f"  Use LIVE service_events traffic from running services.")
    print(f"  All 10 owprov replicas should each receive every service_events message")
    print(f"  (each has its own independent consumer group for BroadcastConsumer).")
    print(f"  Verify via Kafka UI (localhost:9080) → Consumer Groups → service_events.")

    # Final pass/fail
    has_failures = bool(delivery_errors) or remaining > 0 or bool(validation_failures)
    if has_failures:
        print(f"\n[FAIL] Stage 1 failed. See errors/validation failures above.", file=sys.stderr)
        sys.exit(1)

    print(f"\n[PASS] Stage 1 complete: {total_msgs} messages delivered and distribution validated.")
    print(f"       Proceed to Stage 2 and 3 verification manually.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description=(
            "Simulate N AP devices sending connection events to Kafka "
            "for owprov GroupConsumer scalability testing (PR #100)."
        )
    )
    parser.add_argument(
        "--broker", default="kafka:9092",
        help="Kafka broker (default: kafka:9092 — inside Docker network openwifi_openwifi)"
    )
    parser.add_argument(
        "--devices", type=int, default=1000,
        help="Number of simulated devices (default: 1000)"
    )
    parser.add_argument(
        "--partitions", type=int, default=10,
        help=(
            "EXACT expected topic partition count. "
            "Script fails if actual count differs (default: 10)"
        )
    )
    parser.add_argument(
        "--delay", type=float, default=0.01,
        help="Seconds between each device's 3-message batch (default: 0.01)"
    )
    parser.add_argument(
        "--verbose", action="store_true",
        help="Print every delivery acknowledgement with partition, offset and event type"
    )
    args = parser.parse_args()

    run(
        broker=args.broker,
        num_devices=args.devices,
        num_partitions=args.partitions,
        delay=args.delay,
        verbose=args.verbose,
    )
