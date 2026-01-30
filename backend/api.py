import json
import time
import threading

from fastapi import FastAPI, HTTPException
from pydantic import BaseModel
from typing import Optional

import paho.mqtt.client as mqtt

from db import init_db, insert_message, upsert_locker_state
from pathlib import Path

import sqlite3
from pathlib import Path


# -------- Config --------
BROKER_HOST = "192.168.1.11"
BROKER_PORT = 1883
SUB_TOPIC = "locker/#"

DB_PATH = Path(__file__).with_name("locker.db")

def get_conn():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


LOCKER_CMD_TOPIC_FMT = "locker/{locker_id}/cmd"

app = FastAPI(title="Smart Locker Lab (Unified)")

# Publisher (para POST /unlock)
pub = mqtt.Client()
pub_connected = False

# Subscriber (ingestor)
sub = mqtt.Client()

def classify_topic(topic: str) -> tuple[str, str | None]:
    parts = topic.split("/")
    locker_id = parts[1] if len(parts) >= 2 and parts[0] == "locker" else None
    kind = parts[2] if len(parts) >= 3 and parts[0] == "locker" else "unknown"
    return kind, locker_id

# -------- MQTT publisher callbacks --------
def _pub_on_connect(client, userdata, flags, rc, properties=None):
    global pub_connected
    pub_connected = (rc == 0)

def _pub_on_disconnect(client, userdata, rc, properties=None):
    global pub_connected
    pub_connected = False

# -------- MQTT subscriber callbacks --------
def _sub_on_connect(client, userdata, flags, rc, properties=None):
    print("SUB connected rc=", rc)
    client.subscribe(SUB_TOPIC)
    print("SUB subscribed to", SUB_TOPIC)

def _sub_on_message(client, userdata, msg):
    payload = msg.payload.decode(errors="replace")
    ts_ingest = int(time.time())

    kind, locker_id = classify_topic(msg.topic)

    # 1) Guardar mensaje crudo
    insert_message(ts_ingest, msg.topic, payload, kind, locker_id)

    # 2) Si es telemetry, upsert estado actual
    if kind == "telemetry" and locker_id:
        try:
            obj = json.loads(payload)
            door = obj.get("door")
            relay = obj.get("relay")
            upsert_locker_state(locker_id, ts_ingest, door, relay, payload)
        except Exception:
            pass

    print(f"[{time.strftime('%H:%M:%S')}] {msg.topic} {payload}")

def _start_mqtt_clients():
    # Publisher
    pub.on_connect = _pub_on_connect
    pub.on_disconnect = _pub_on_disconnect
    pub.connect(BROKER_HOST, BROKER_PORT, keepalive=60)
    pub.loop_start()

    # Subscriber
    sub.on_connect = _sub_on_connect
    sub.on_message = _sub_on_message
    sub.connect(BROKER_HOST, BROKER_PORT, keepalive=60)
    sub.loop_forever()

@app.on_event("startup")
def startup():
    init_db()

    t = threading.Thread(target=_start_mqtt_clients, daemon=True)
    t.start()

# -------- API --------
class UnlockRequest(BaseModel):
    duration_ms: int = 1500
    cmd_id: Optional[str] = None

@app.get("/health")
def health():
    return {"ok": True, "mqtt_pub_connected": pub_connected}

@app.post("/lockers/{locker_id}/unlock")
def unlock(locker_id: str, req: UnlockRequest):
    dur = req.duration_ms
    if dur < 50 or dur > 10000:
        raise HTTPException(status_code=400, detail="duration_ms must be between 50 and 10000")

    cmd_id = req.cmd_id or str(int(time.time()))
    topic = LOCKER_CMD_TOPIC_FMT.format(locker_id=locker_id)
    payload = {"cmd_id": cmd_id, "action": "unlock", "duration_ms": dur}

    if not pub_connected:
        raise HTTPException(status_code=503, detail="MQTT not connected")

    pub.publish(topic, json.dumps(payload, separators=(",", ":")), qos=1, retain=False)
    return {"sent": True, "topic": topic, "payload": payload}

@app.get("/lockers")
def list_lockers():
    conn = get_conn()
    rows = conn.execute(
        "SELECT DISTINCT locker_id FROM messages WHERE locker_id IS NOT NULL ORDER BY locker_id"
    ).fetchall()
    conn.close()
    return {"lockers": [r["locker_id"] for r in rows]}

@app.get("/lockers/{locker_id}/state")
def locker_state(locker_id: str):
    conn = get_conn()
    row = conn.execute(
        "SELECT locker_id, ts_update, door, relay, raw_payload FROM locker_state WHERE locker_id=?",
        (locker_id,),
    ).fetchone()
    conn.close()

    if not row:
        raise HTTPException(status_code=404, detail="locker not found")

    return dict(row)

@app.get("/messages")
def get_messages(limit: int = 50):
    limit = max(1, min(limit, 500))
    conn = get_conn()
    rows = conn.execute(
        "SELECT id, ts_ingest, topic, kind, locker_id, payload FROM messages ORDER BY id DESC LIMIT ?",
        (limit,),
    ).fetchall()
    conn.close()
    return {"messages": [dict(r) for r in rows]}

@app.get("/lockers/{locker_id}/events")
def get_locker_events(locker_id: str, limit: int = 50):
    limit = max(1, min(limit, 500))
    conn = get_conn()
    rows = conn.execute(
        "SELECT id, ts_ingest, topic, payload FROM messages "
        "WHERE locker_id=? AND kind='event' ORDER BY id DESC LIMIT ?",
        (locker_id, limit),
    ).fetchall()
    conn.close()
    return {"events": [dict(r) for r in rows]}
