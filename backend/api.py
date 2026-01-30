import json
import time
import sqlite3
from pathlib import Path
from typing import Optional

from fastapi import FastAPI, HTTPException
from pydantic import BaseModel

import paho.mqtt.client as mqtt


# -------- Config --------
DB_PATH = Path(__file__).with_name("locker.db")

BROKER_HOST = "192.168.1.11"
BROKER_PORT = 1883

def get_conn():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


# -------- MQTT Publisher (simple) --------
class MqttPublisher:
    def __init__(self, host: str, port: int):
        self.host = host
        self.port = port
        self.client = mqtt.Client()
        self.connected = False

        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect

    def _on_connect(self, client, userdata, flags, rc, properties=None):
        self.connected = (rc == 0)

    def _on_disconnect(self, client, userdata, rc, properties=None):
        self.connected = False

    def start(self):
        self.client.connect(self.host, self.port, keepalive=60)
        self.client.loop_start()

        # esperar un toque a que conecte
        t0 = time.time()
        while not self.connected and (time.time() - t0) < 2.0:
            time.sleep(0.05)

    def publish(self, topic: str, payload: dict, qos: int = 1):
        data = json.dumps(payload, separators=(",", ":"))
        self.client.publish(topic, data, qos=qos, retain=False)


app = FastAPI(title="Smart Locker Lab API")
pub = MqttPublisher(BROKER_HOST, BROKER_PORT)


@app.on_event("startup")
def _startup():
    pub.start()


# -------- Models --------
class UnlockRequest(BaseModel):
    duration_ms: int = 1500
    cmd_id: Optional[str] = None


# -------- Routes --------
@app.get("/health")
def health():
    return {"ok": True, "mqtt_connected": pub.connected}


@app.get("/lockers")
def list_lockers():
    conn = get_conn()
    rows = conn.execute(
        "SELECT DISTINCT locker_id FROM messages WHERE locker_id IS NOT NULL ORDER BY locker_id"
    ).fetchall()
    conn.close()
    return {"lockers": [r["locker_id"] for r in rows]}


@app.get("/messages")
def get_messages(limit: int = 50):
    limit = max(1, min(limit, 500))
    conn = get_conn()
    rows = conn.execute(
        "SELECT id, ts_ingest, topic, kind, locker_id, payload FROM messages ORDER BY id DESC LIMIT ?",
        (limit,),
    ).fetchall()
    conn.close()
    return {
        "messages": [
            dict(r) for r in rows
        ]
    }


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


@app.post("/lockers/{locker_id}/unlock")
def unlock(locker_id: str, req: UnlockRequest):
    dur = req.duration_ms
    if dur < 50 or dur > 10000:
        raise HTTPException(status_code=400, detail="duration_ms must be between 50 and 10000")

    cmd_id = req.cmd_id or str(int(time.time()))

    topic = f"locker/{locker_id}/cmd"
    payload = {"cmd_id": cmd_id, "action": "unlock", "duration_ms": dur}

    if not pub.connected:
        raise HTTPException(status_code=503, detail="MQTT not connected")

    pub.publish(topic, payload)
    return {"sent": True, "topic": topic, "payload": payload}
