import sqlite3
from pathlib import Path

DB_PATH = Path(__file__).with_name("locker.db")

SCHEMA_SQL = """
CREATE TABLE IF NOT EXISTS messages (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  ts_ingest  INTEGER NOT NULL,
  topic      TEXT    NOT NULL,
  payload    TEXT    NOT NULL,
  kind       TEXT    NOT NULL,
  locker_id  TEXT
);

CREATE INDEX IF NOT EXISTS idx_messages_ts ON messages(ts_ingest);
CREATE INDEX IF NOT EXISTS idx_messages_topic ON messages(topic);
CREATE INDEX IF NOT EXISTS idx_messages_locker ON messages(locker_id);

CREATE TABLE IF NOT EXISTS locker_state (
  locker_id    TEXT PRIMARY KEY,
  ts_update    INTEGER NOT NULL,
  door         TEXT,
  relay        TEXT,
  raw_payload  TEXT
);

CREATE INDEX IF NOT EXISTS idx_state_ts ON locker_state(ts_update);

"""

def get_conn():
    conn = sqlite3.connect(DB_PATH)
    conn.execute("PRAGMA journal_mode=WAL;")
    return conn

def init_db():
    conn = get_conn()
    with conn:
        conn.executescript(SCHEMA_SQL)
    conn.close()

def insert_message(ts_ingest: int, topic: str, payload: str, kind: str, locker_id: str | None):
    conn = get_conn()
    with conn:
        conn.execute(
            "INSERT INTO messages(ts_ingest, topic, payload, kind, locker_id) VALUES(?,?,?,?,?)",
            (ts_ingest, topic, payload, kind, locker_id),
        )
    conn.close()


def upsert_locker_state(locker_id: str, ts_update: int, door: str | None, relay: str | None, raw_payload: str):
    conn = get_conn()
    with conn:
        conn.execute(
            """
            INSERT INTO locker_state(locker_id, ts_update, door, relay, raw_payload)
            VALUES(?,?,?,?,?)
            ON CONFLICT(locker_id) DO UPDATE SET
              ts_update=excluded.ts_update,
              door=excluded.door,
              relay=excluded.relay,
              raw_payload=excluded.raw_payload
            """,
            (locker_id, ts_update, door, relay, raw_payload),
        )
    conn.close()
