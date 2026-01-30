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
