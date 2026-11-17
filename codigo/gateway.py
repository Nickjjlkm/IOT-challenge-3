#!/usr/bin/env python3
"""
HappyMoist gateway (v2, con diagnostico)
  ESP32 --MQTT--> Mosquitto (local) --> SQLite --(promedio)--> Ubidots (MQTT)

Topicos que publica el ESP32 (DEVICE_ID = "01"):
  happymoist/01/datos   {"nivel":..,"temp":..,"hum":..,"presion":..,
                         "radiacion":..,"evap":..,"alerta":0|1}   cada 5 s
  happymoist/01/estado  "online" | "offline"  (retenido, Last Will)

Variables que llegan a Ubidots (device "happymoist-01"):
  nivel, temp, hum, presion, radiacion, evap -> promedio del periodo
  alerta                                     -> 1 si hubo alguna alerta en el periodo
  estado                                     -> 1 = online, 0 = offline

Uso:
  python gateway.py            # servicio normal
  python gateway.py --probar   # solo prueba la conexion a Ubidots y termina

La base de datos es compatible con la version anterior (mismo esquema).
"""
import argparse
import json
import logging
import math
import os
import signal
import socket
import sqlite3
import ssl
import sys
import threading
import time
from contextlib import contextmanager

import paho.mqtt.client as mqtt

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("gateway")

# ---------- Configuracion (viene del .env) ----------
DB_PATH        = os.getenv("DB_PATH", "/opt/happymoist/happymoist.db")
L_HOST         = os.getenv("LOCAL_HOST", "localhost")
L_PORT         = int(os.getenv("LOCAL_PORT", "1883"))
L_USER         = os.environ["LOCAL_USER"]
L_PASS         = os.environ["LOCAL_PASS"]
DEVICE_ID      = os.getenv("DEVICE_ID", "01")

UB_HOST        = os.getenv("UBIDOTS_HOST", "industrial.api.ubidots.com")
UB_TLS         = os.getenv("UBIDOTS_TLS", "0").strip().lower() in ("1", "true", "si", "yes")
UB_PORT        = int(os.getenv("UBIDOTS_PORT", "8883" if UB_TLS else "1883"))
UB_TOKEN       = os.environ["UBIDOTS_TOKEN"].strip()
UB_DEVICE      = os.getenv("UBIDOTS_DEVICE", f"happymoist-{DEVICE_ID}")

INTERVAL       = int(os.getenv("PUBLISH_INTERVAL", "300"))      # segundos entre envios
FIRST_DELAY    = int(os.getenv("FIRST_PUBLISH_DELAY", "60"))    # primer envio (para comprobar rapido)
RETENTION_DAYS = int(os.getenv("RETENTION_DAYS", "30"))

TOPIC_DATOS  = f"happymoist/{DEVICE_ID}/datos"
TOPIC_ESTADO = f"happymoist/{DEVICE_ID}/estado"
UB_TOPIC     = f"/v1.6/devices/{UB_DEVICE}"

# Campos que manda el ESP32 y como se resumen en cada periodo
CAMPOS_PROMEDIO = ["nivel", "temp", "hum", "presion", "radiacion", "evap"]
CAMPOS_MAXIMO   = ["alerta"]          # 0/1: si hubo alguna alerta, llega 1
CAMPOS = CAMPOS_PROMEDIO + CAMPOS_MAXIMO

stop = threading.Event()
stats = {"datos": 0, "estado": None, "ultimo_dato": None}


# ---------- Base de datos ----------
@contextmanager
def db():
    con = sqlite3.connect(DB_PATH, timeout=10)
    try:
        con.execute("PRAGMA journal_mode=WAL")
        yield con
        con.commit()
    except Exception:
        con.rollback()
        raise
    finally:
        con.close()


def init_db():
    with db() as con:
        con.executescript("""
        CREATE TABLE IF NOT EXISTS lecturas(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            ts INTEGER NOT NULL,
            campo TEXT NOT NULL,
            valor REAL NOT NULL,
            enviado INTEGER NOT NULL DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_env ON lecturas(enviado, id);
        CREATE TABLE IF NOT EXISTS estados(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            ts INTEGER NOT NULL,
            estado TEXT NOT NULL
        );
        """)


def estado_a_num(texto):
    return 1 if texto.strip().lower() == "online" else 0


# ---------- MQTT Ubidots (gateway -> nube) ----------
ub_ready = threading.Event()     # se activa solo cuando Ubidots acepto la conexion


def on_ub_connect(c, u, flags, rc, props):
    if rc.is_failure:
        ub_ready.clear()
        log.error("Ubidots RECHAZO la conexion: %s. Revisa UBIDOTS_TOKEN y que "
                  "UBIDOTS_HOST (%s) corresponda al tipo de tu cuenta.", rc, UB_HOST)
        return
    ub_ready.set()
    log.info("Ubidots conectado (%s:%d%s)", UB_HOST, UB_PORT, " TLS" if UB_TLS else "")


def on_ub_disconnect(c, u, flags, rc, props):
    ub_ready.clear()
    log.warning("Ubidots desconectado: %s (se reconecta solo)", rc)


def on_ub_connect_fail(c, u):
    ub_ready.clear()
    log.warning("No se pudo abrir la conexion con %s:%d (sin internet, DNS, o puerto "
                "bloqueado por la red). Reintentando...", UB_HOST, UB_PORT)


ub = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                 client_id=f"happymoist-gw-{DEVICE_ID}")
ub.username_pw_set(UB_TOKEN, "")          # Ubidots: usuario = token, clave vacia
ub.on_connect = on_ub_connect
ub.on_disconnect = on_ub_disconnect
ub.on_connect_fail = on_ub_connect_fail
ub.reconnect_delay_set(min_delay=2, max_delay=60)
if UB_TLS:
    ub.tls_set(cert_reqs=ssl.CERT_REQUIRED)


def publicar_ubidots(payload, timeout=15):
    """Publica y espera confirmacion (QoS 1). True si Ubidots lo recibio."""
    info = ub.publish(UB_TOPIC, json.dumps(payload), qos=1)
    if info.rc != mqtt.MQTT_ERR_SUCCESS:
        log.warning("publish rechazado localmente (rc=%s)", info.rc)
        return False
    try:
        info.wait_for_publish(timeout=timeout)
    except Exception as e:
        log.warning("Fallo esperando confirmacion de Ubidots: %s", e)
        return False
    return info.is_published()


# ---------- MQTT local (ESP32 -> gateway) ----------
def on_local_connect(c, u, flags, rc, props):
    if rc.is_failure:
        log.error("Mosquitto local RECHAZO la conexion: %s "
                  "(revisa LOCAL_USER / LOCAL_PASS)", rc)
        return
    log.info("Mosquitto local conectado, suscrito a %s y %s", TOPIC_DATOS, TOPIC_ESTADO)
    c.subscribe([(TOPIC_DATOS, 0), (TOPIC_ESTADO, 1)])


def on_local_message(c, u, msg):
    payload = msg.payload.decode(errors="ignore").strip()
    now = int(time.time())
    try:
        if msg.topic == TOPIC_DATOS:
            data = json.loads(payload)
            # Si un sensor falla, el ESP32 omite el campo: solo se guarda lo que llega
            rows = [(now, k, float(data[k])) for k in CAMPOS
                    if k in data
                    and isinstance(data[k], (int, float))
                    and math.isfinite(data[k])]
            if not rows:
                log.warning("Datos sin campos validos: %r", payload)
                return
            with db() as con:
                con.executemany(
                    "INSERT INTO lecturas(ts,campo,valor) VALUES(?,?,?)", rows)
            stats["datos"] += 1
            stats["ultimo_dato"] = now
            if stats["datos"] % 60 == 1:      # una linea cada ~5 min, sin llenar el log
                log.info("Recibiendo datos del ESP32 (%d mensajes desde el arranque)",
                         stats["datos"])

        elif msg.topic == TOPIC_ESTADO:
            valor = estado_a_num(payload)
            stats["estado"] = payload
            with db() as con:
                con.execute("INSERT INTO estados(ts,estado) VALUES(?,?)",
                            (now, payload))
            log.info("Estado del ESP32: %s", payload)
            # Un cambio de estado se avisa de inmediato, sin esperar al promedio
            # (si el ESP32 se apaga ya no llegan datos y no habria promedio que enviar).
            # Sin esperar confirmacion: este hilo no debe bloquearse.
            if ub_ready.is_set():
                ub.publish(UB_TOPIC, json.dumps({"estado": valor}), qos=1)

    except Exception as e:
        log.warning("Mensaje invalido en %s: %r (%s)", msg.topic, payload, e)


local = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="gateway-sub")
local.username_pw_set(L_USER, L_PASS)
local.on_connect = on_local_connect
local.on_message = on_local_message
local.reconnect_delay_set(min_delay=1, max_delay=30)


# ---------- Promedio y envio ----------
def enviar_promedio():
    with db() as con:
        max_id = con.execute(
            "SELECT MAX(id) FROM lecturas WHERE enviado=0").fetchone()[0]
        if max_id is None:
            log.warning("No hay lecturas nuevas para enviar (mensajes de datos recibidos "
                        "desde el arranque: %d). Esta encendido el ESP32?", stats["datos"])
            return
        rows = con.execute(
            "SELECT campo, AVG(valor), MAX(valor), COUNT(*) FROM lecturas "
            "WHERE enviado=0 AND id<=? GROUP BY campo", (max_id,)).fetchall()
        est = con.execute(
            "SELECT estado FROM estados ORDER BY id DESC LIMIT 1").fetchone()

    payload = {}
    muestras = 0
    for campo, avg, mx, n in rows:
        payload[campo] = int(mx) if campo in CAMPOS_MAXIMO else round(avg, 2)
        muestras = max(muestras, n)
    if est:
        payload["estado"] = estado_a_num(est[0])

    if not ub_ready.is_set():
        log.warning("Sin conexion con Ubidots: %d muestras quedan guardadas y se "
                    "enviaran en el proximo ciclo", muestras)
        return

    if not publicar_ubidots(payload):
        log.warning("Ubidots no confirmo el envio; se reintenta en el proximo ciclo")
        return

    with db() as con:
        con.execute("UPDATE lecturas SET enviado=1 WHERE enviado=0 AND id<=?", (max_id,))
        limite = int(time.time()) - RETENTION_DAYS * 86400
        con.execute("DELETE FROM lecturas WHERE enviado=1 AND ts<?", (limite,))
        con.execute("DELETE FROM estados WHERE ts<?", (limite,))
    log.info("ENVIADO a Ubidots (%d muestras): %s", muestras, payload)


# ---------- Prueba de conexion a Ubidots ----------
def probar_ubidots():
    print(f"Probando Ubidots: {UB_HOST}:{UB_PORT} (TLS={'si' if UB_TLS else 'no'}), "
          f"dispositivo '{UB_DEVICE}'")
    if "xxxx" in UB_TOKEN.lower():
        print("AVISO: UBIDOTS_TOKEN parece el valor de ejemplo; pon tu token real.")

    try:
        ip = socket.getaddrinfo(UB_HOST, UB_PORT, proto=socket.IPPROTO_TCP)[0][4][0]
        print(f"[1/3] DNS OK -> {ip}")
    except OSError as e:
        print(f"[1/3] DNS FALLO: {e}\n      La Raspberry no tiene internet o no resuelve nombres.")
        return 1

    try:
        socket.create_connection((UB_HOST, UB_PORT), timeout=8).close()
        print("[2/3] TCP OK")
    except OSError as e:
        print(f"[2/3] TCP FALLO: {e}\n      Probable puerto bloqueado por la red/operador. "
              "Prueba otra red (por ejemplo WiFi del hogar) o UBIDOTS_TLS=1.")
        return 1

    ub.connect_async(UB_HOST, UB_PORT, 60)
    ub.loop_start()
    if not ub_ready.wait(15):
        print("[3/3] MQTT FALLO: Ubidots no acepto la conexion en 15 s. Mira el mensaje "
              "de error de arriba: token incorrecto o host que no corresponde a tu cuenta.")
        ub.loop_stop()
        return 1
    print("[3/3] MQTT OK: token aceptado")

    ok = publicar_ubidots({"gateway_test": 1})
    ub.loop_stop()
    ub.disconnect()
    if ok:
        print("PRUEBA OK: se envio la variable 'gateway_test' a Ubidots. "
              "Revisa el dispositivo en la plataforma (puedes borrar esa variable despues).")
        return 0
    print("La conexion funciona pero Ubidots no confirmo la publicacion.")
    return 1


# ---------- Main ----------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probar", action="store_true",
                    help="prueba la conexion a Ubidots y termina")
    args = ap.parse_args()

    if args.probar:
        sys.exit(probar_ubidots())

    if "xxxx" in UB_TOKEN.lower():
        log.error("UBIDOTS_TOKEN parece el valor de ejemplo: pon tu token real en el .env")

    init_db()
    signal.signal(signal.SIGTERM, lambda *a: stop.set())
    signal.signal(signal.SIGINT, lambda *a: stop.set())

    log.info("Config: Ubidots %s:%d%s dispositivo=%s | envio cada %d s (primero a los %d s)",
             UB_HOST, UB_PORT, " TLS" if UB_TLS else "", UB_DEVICE, INTERVAL,
             min(FIRST_DELAY, INTERVAL))

    local.connect_async(L_HOST, L_PORT, 60)
    local.loop_start()
    ub.connect_async(UB_HOST, UB_PORT, 60)
    ub.loop_start()

    espera = min(FIRST_DELAY, INTERVAL)
    while not stop.is_set():
        log.info("Proximo envio a Ubidots en %d s", espera)
        if stop.wait(espera):
            break
        try:
            enviar_promedio()
        except Exception:
            log.exception("Error en el ciclo de envio")
        espera = INTERVAL

    log.info("Cerrando...")
    local.loop_stop()
    ub.loop_stop()
    local.disconnect()
    ub.disconnect()


if __name__ == "__main__":
    main()
