#!/usr/bin/env python3
"""Log irrigation status messages from MQTT to a CSV file and print them.

Usage:
  pip install paho-mqtt
  python3 tools/mqtt_logger.py --broker 192.168.1.10 --topic garden/irrigation --out irrigation_log.csv

Send a command from another terminal:
  mosquitto_pub -h 192.168.1.10 -t garden/irrigation/cmd -m '{"cmd":"water","seconds":20}'
"""
import argparse
import csv
import json
import os
import sys
import time

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("paho-mqtt is required: pip install paho-mqtt")

FIELDS = ["received_at", "state", "moisture_pct", "sensor_mv", "pump", "pump_run_s",
          "pump_total_s", "tank_ok", "auto", "in_window", "cycles", "fault_cycles",
          "on_pct", "off_pct", "uptime_s", "time"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--broker", required=True)
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--topic", default="garden/irrigation", help="base topic")
    ap.add_argument("--out", default="irrigation_log.csv")
    args = ap.parse_args()

    new_file = not os.path.exists(args.out)
    out = open(args.out, "a", newline="")
    writer = csv.DictWriter(out, fieldnames=FIELDS, extrasaction="ignore")
    if new_file:
        writer.writeheader()

    def on_connect(client, userdata, flags, reason_code, properties=None):
        print(f"connected to {args.broker}, subscribing to {args.topic}/#")
        client.subscribe(f"{args.topic}/#")

    def on_message(client, userdata, msg):
        stamp = time.strftime("%Y-%m-%d %H:%M:%S")
        if msg.topic.endswith("/status"):
            try:
                d = json.loads(msg.payload)
            except json.JSONDecodeError:
                print(f"{stamp} bad JSON on {msg.topic}")
                return
            d["received_at"] = stamp
            writer.writerow(d)
            out.flush()
            print(f"{stamp} {d.get('state'):13} moisture={d.get('moisture_pct')}% "
                  f"pump={'on' if d.get('pump') else 'off'} tank={'ok' if d.get('tank_ok') else 'EMPTY'}")
        else:
            print(f"{stamp} {msg.topic}: {msg.payload.decode(errors='replace')}")

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2) if hasattr(mqtt, "CallbackAPIVersion") else mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(args.broker, args.port, 60)
    try:
        client.loop_forever()
    except KeyboardInterrupt:
        pass
    finally:
        out.close()


if __name__ == "__main__":
    main()
