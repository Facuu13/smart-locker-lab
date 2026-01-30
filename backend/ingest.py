import json
import time
import paho.mqtt.client as mqtt

BROKER_HOST = "192.168.1.11"   # IP de tu PC donde corre Mosquitto
BROKER_PORT = 1883
TOPIC = "locker/#"

def on_connect(client, userdata, flags, rc, properties=None):
    print("MQTT connected rc=", rc)
    client.subscribe(TOPIC)
    print("Subscribed to", TOPIC)

def on_message(client, userdata, msg):
    payload = msg.payload.decode(errors="replace")
    print(f"[{time.strftime('%H:%M:%S')}] {msg.topic} {payload}")

    # opcional: validar que sea JSON
    try:
        _ = json.loads(payload)
    except Exception:
        pass

def main():
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(BROKER_HOST, BROKER_PORT, keepalive=60)
    client.loop_forever()

if __name__ == "__main__":
    main()
