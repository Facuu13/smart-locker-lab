
# 🧠 Smart Locker Lab — ESP32 + MQTT + FastAPI

Proyecto práctico de **IoT end-to-end** que implementa un sistema de **lockers inteligentes** utilizando ESP32, MQTT, backend en Python, base de datos SQLite y API REST.

El objetivo del proyecto es **practicar y demostrar** una arquitectura IoT completa, desde el firmware embebido hasta el backend y la API de control.

---

## 🏗️ Arquitectura General

```
┌────────────┐
│   ESP32    │
│            │
│  - GPIO    │
│  - ISR     │
│  - MQTT    │
└─────┬──────┘
      │ MQTT
      ▼
┌───────────────┐
│ Mosquitto     │
│ (Docker)      │
└─────┬─────────┘
      │
      ▼
┌──────────────────────────┐
│ Backend (FastAPI + MQTT) │
│                          │
│ - Ingest MQTT            │
│ - SQLite (messages)      │
│ - locker_state           │
│ - REST API               │
└──────────────────────────┘
```

---

## ⚙️ Firmware ESP32

### Funcionalidades

* Conexión Wi-Fi (STA)
* Cliente MQTT
* **GPIO con interrupciones**
* Debounce por ISR + confirmación en task
* Simulación de reed switch (puerta)
* Control de relay por comando MQTT
* Publicación de eventos, telemetry y ACKs

### GPIO usados (configurable)

* `GPIO17` → Botón (simula puerta abierta/cerrada)
* `GPIO2` → Relay (unlock)

### Topics MQTT

| Topic                   | Descripción                          |
| ----------------------- | ------------------------------------ |
| `locker/<id>/event`     | Eventos (`door_open`, `door_closed`) |
| `locker/<id>/telemetry` | Estado actual (door / relay)         |
| `locker/<id>/cmd`       | Comandos (`unlock`)                  |
| `locker/<id>/ack`       | Confirmación de comandos             |

### Ejemplo de evento

```json
{
  "type": "door_open",
  "ts": 526,
  "source_gpio": 17
}
```

### Ejemplo de comando

```json
{
  "cmd_id": "1",
  "action": "unlock",
  "duration_ms": 1500
}
```

---

## 🧠 Backend

### Tecnologías

* Python 3.12
* FastAPI
* paho-mqtt
* SQLite
* Docker

### Funciones principales

* **Subscriber MQTT** (background)
* Persistencia de todos los mensajes (`messages`)
* Estado actual del locker (`locker_state`)
* Publicación de comandos por API REST
* Reintentos y reconexión MQTT

### Base de datos

**messages**

* topic
* payload
* tipo (`event`, `telemetry`, `cmd`, `ack`)
* locker_id
* timestamp de ingestión

**locker_state**

* estado actual de cada locker
* puerta (`open / closed`)
* relay (`on / off`)
* último update

---

## 🌐 API REST

### Health

```http
GET /health
```

### Listar lockers

```http
GET /lockers
```

### Estado actual

```http
GET /lockers/{locker_id}/state
```

### Eventos recientes

```http
GET /lockers/{locker_id}/events
```

### Enviar unlock

```http
POST /lockers/{locker_id}/unlock
```

Body:

```json
{
  "duration_ms": 1500
}
```

---

## 🐳 Docker

El proyecto corre completamente en Docker:

* Mosquitto (broker MQTT)
* Backend FastAPI + subscriber MQTT
* Base de datos persistente (SQLite)

### Levantar todo

```bash
docker compose up -d --build
```

### Ver logs

```bash
docker logs -f locker-backend
```

---

## ▶️ Cómo probar el sistema

1. Flashear ESP32 con el firmware
2. Levantar Docker
3. Ver estado:

```bash
curl http://localhost:8000/lockers/locker-01/state
```

4. Enviar unlock:

```bash
curl -X POST http://localhost:8000/lockers/locker-01/unlock \
  -H "Content-Type: application/json" \
  -d '{"duration_ms":1500}'
```

5. Apretar el botón físico y ver eventos

---

## 📌 Decisiones de diseño

* ISR mínima → lógica en tasks
* MQTT desacoplado (eventos vs estado)
* Telemetry solo cuando cambia el estado
* Backend unificado (API + ingest)
* SQLite por simplicidad y portabilidad

---

## 🚀 Futuras mejoras

* LWT online/offline
* Autenticación en API
* Dashboard web
* Múltiples lockers simultáneos
* TLS en MQTT
* OTA para ESP32

