import json
import time
import paho.mqtt.client as mqtt

from db import init_db, insert_message

BROKER_HOST = "192.168.1.11"   # IP de tu PC donde corre Mosquitto
BROKER_PORT = 1883
TOPIC = "locker/#"

def classify_topic(topic: str) -> tuple[str, str | None]:
    # locker/<id>/<kind>
    parts = topic.split("/")
    locker_id = parts[1] if len(parts) >= 2 and parts[0] == "locker" else None
    kind = parts[2] if len(parts) >= 3 and parts[0] == "locker" else "unknown"
    return kind, locker_id

def on_connect(client, userdata, flags, rc, properties=None):
    print("MQTT connected rc=", rc)
    client.subscribe(TOPIC)
    print("Subscribed to", TOPIC)

def on_message(client, userdata, msg):
    payload = msg.payload.decode(errors="replace")
    ts_ingest = int(time.time())

    kind, locker_id = classify_topic(msg.topic)

    # Guardar SIEMPRE como texto (aunque no sea JSON válido)
    insert_message(ts_ingest, msg.topic, payload, kind, locker_id)

    # Print para debug
    try:
        json.loads(payload)
        ok = "json"
    except Exception:
        ok = "raw"
    print(f"[{time.strftime('%H:%M:%S')}] ({ok}) {msg.topic} {payload}")

def main():
    init_db()
    client = mqtt.Client()  # warning lo arreglamos después
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(BROKER_HOST, BROKER_PORT, keepalive=60)
    client.loop_forever()

if __name__ == "__main__":
    main()
