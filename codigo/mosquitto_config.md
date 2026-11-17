# Configuración de Mosquitto — HappyMoist (Raspberry Pi)

Broker MQTT local del gateway. Recibe los mensajes del ESP32 y los pone a disposición del `gateway.py`.

| Dato | Valor |
|---|---|
| Puerto | `1883` |
| Usuario del gateway (Raspberry) | `gateway` |
| Usuario del ESP32 | `esp32` |
| Contraseña | definida al crear cada usuario (no se publica) |
| Topics | `happymoist/01/datos`, `happymoist/01/estado` |

---

## 1. Instalar

```bash
sudo apt update
sudo apt install -y mosquitto mosquitto-clients
```

## 2. Crear los usuarios

Solo el primer comando lleva `-c`, que **crea** el archivo. Si lo usas en el segundo, borras al primer usuario.

```bash
sudo mosquitto_passwd -b -c /etc/mosquitto/passwd gateway CLAVE
sudo mosquitto_passwd -b /etc/mosquitto/passwd esp32 CLAVE
sudo chown mosquitto:mosquitto /etc/mosquitto/passwd
sudo chmod 600 /etc/mosquitto/passwd
```

Ver qué usuarios existen (muestra los nombres, no las contraseñas):

```bash
sudo cut -d: -f1 /etc/mosquitto/passwd
```

## 3. Archivo de configuración

Crear `/etc/mosquitto/conf.d/happymoist.conf`:

```bash
sudo nano /etc/mosquitto/conf.d/happymoist.conf
```

```
listener 1883
allow_anonymous false
password_file /etc/mosquitto/passwd
```

| Línea | Para qué sirve |
|---|---|
| `listener 1883` | Acepta conexiones desde la red. Sin ella, Mosquitto 2.x solo escucha en `localhost` y el ESP32 no puede entrar |
| `allow_anonymous false` | Exige usuario y contraseña |
| `password_file` | Archivo de usuarios creado en el paso 2 |

> No dupliques estas líneas. Si ya tienes otros archivos en `/etc/mosquitto/conf.d/` con `listener` o `password_file`, revísalos con `cat /etc/mosquitto/conf.d/*.conf`; Mosquitto puede negarse a arrancar con opciones repetidas.

## 4. Reiniciar y comprobar

```bash
sudo systemctl enable mosquitto
sudo systemctl restart mosquitto
sudo systemctl status mosquitto --no-pager | head -5
sudo ss -tlnp | grep 1883
```

El último comando debe mostrar `0.0.0.0:1883`. Si muestra solo `127.0.0.1:1883`, el ESP32 no podrá conectarse.

## 5. Probar

Con dos terminales (o dos sesiones SSH):

```bash
# Terminal A: escuchar todo
mosquitto_sub -h localhost -u gateway -P CLAVE -t 'happymoist/#' -v

# Terminal B: publicar como si fuera el ESP32
mosquitto_pub -h localhost -u esp32 -P CLAVE -t happymoist/01/estado -m online
mosquitto_pub -h localhost -u esp32 -P CLAVE -t happymoist/01/datos -m '{"nivel":120.5,"temp":15.0,"hum":70.0,"presion":745.0,"radiacion":200.0,"evap":4.2,"alerta":0}'
```

La terminal A debe mostrar los dos mensajes. Con el ESP32 encendido, también llegan sus datos cada 5 s.

Probar desde otro equipo de la red (usa la IP de la Raspberry):

```bash
mosquitto_sub -h IP_DE_LA_RASPBERRY -u gateway -P CLAVE -t 'happymoist/#' -v
```

## 6. IP fija de la Raspberry

El ESP32 tiene la IP del broker escrita en su código (`MQTT_HOST = "IP_DE_LA_RASPBERRY"`). Si la IP de la Raspberry cambia, el ESP32 deja de conectarse.

```bash
hostname -I
```

Reserva esa IP en el router (reserva DHCP) si tu red lo permite. En un hotspot de celular normalmente no se puede, y habrá que actualizar el código del ESP32 si cambia.

## 7. Cambiar una contraseña

```bash
sudo mosquitto_passwd -b /etc/mosquitto/passwd gateway NUEVA_CLAVE
sudo systemctl restart mosquitto
```

Después actualiza la clave donde se use:
- `gateway`: variable `LOCAL_PASS` en `/opt/happymoist/.env`, y luego `sudo systemctl restart happymoist-gateway`.
- `esp32`: constante `MQTT_PASS` en el código del ESP32 (y volver a cargarlo).

Agregar otro usuario (por ejemplo, un segundo ESP32), sin `-c`:

```bash
sudo mosquitto_passwd -b /etc/mosquitto/passwd esp32b OTRA_CLAVE
```

Borrar un usuario:

```bash
sudo mosquitto_passwd -D /etc/mosquitto/passwd esp32b
```

## 8. (Opcional) Permisos por usuario con ACL

Limita qué puede hacer cada usuario: el ESP32 solo publica en `happymoist/01/#` y el gateway solo lee.

Crear `/etc/mosquitto/acl`:

```bash
sudo nano /etc/mosquitto/acl
```

```
user esp32
topic write happymoist/01/#

user gateway
topic read happymoist/#
```

Agregar esta línea a `/etc/mosquitto/conf.d/happymoist.conf`:

```
acl_file /etc/mosquitto/acl
```

Dar permisos y reiniciar:

```bash
sudo chown mosquitto:mosquitto /etc/mosquitto/acl
sudo chmod 600 /etc/mosquitto/acl
sudo systemctl restart mosquitto
```

> Con un `acl_file`, un usuario que no esté listado no tiene ningún permiso. Si agregas otro ESP32 (por ejemplo `happymoist/02/#`), agrégalo también al ACL. Este bloque es opcional.

## 9. Last Will del ESP32 (estado `offline`)

El ESP32 ya se conecta así (no hay que cambiarlo):

- Al conectarse publica `online` en `happymoist/01/estado` (retenido).
- Registra un *Last Will*: si el ESP32 se cae sin avisar, el broker publica `offline` en ese mismo topic (retenido, QoS 1).
- Con `keepAlive = 15 s`, el broker detecta la caída en unos 20 a 30 segundos.

Ver el último estado retenido:

```bash
mosquitto_sub -h localhost -u gateway -P CLAVE -t happymoist/01/estado -v -C 1
```

Borrar un mensaje retenido (por ejemplo, un `offline` viejo que quedó):

```bash
mosquitto_pub -h localhost -u esp32 -P CLAVE -t happymoist/01/estado -n -r
```

## 10. Diagnóstico

Ver en vivo quién se conecta y por qué falla:

```bash
sudo journalctl -u mosquitto -f
```

Códigos que imprime el ESP32 en el Monitor Serie (`[mqtt] conectando ... fallo (rc=X)`):

| rc | Significado | Qué revisar |
|---|---|---|
| `-2` | No alcanza el broker | IP errónea, `listener 1883` ausente, mismo WiFi, aislamiento de clientes del router |
| `-4` | Timeout | Red lenta o broker caído |
| `4` | Usuario o contraseña incorrectos | Contraseña del ESP32 vs. `/etc/mosquitto/passwd` |
| `5` | No autorizado | Usuario inexistente, o ACL sin permiso |

Problemas frecuentes:

| Síntoma | Causa probable | Solución |
|---|---|---|
| Mosquitto no arranca | Opción duplicada en `conf.d` | `sudo journalctl -u mosquitto -n 30 --no-pager` y quitar la repetida |
| El ESP32 no conecta, pero `localhost` funciona | Falta `listener 1883` | Paso 3 |
| `Connection error: Connection Refused: not authorised` | Usuario o clave incorrectos | Paso 2 / Paso 7 |
| Aviso de permisos del archivo de contraseñas | Dueño o modo incorrectos | `chown mosquitto:mosquitto` y `chmod 600` (paso 2) |

## 11. Comandos útiles

```bash
sudo systemctl restart mosquitto      # reiniciar tras cambiar la configuración
sudo systemctl status mosquitto       # estado
sudo journalctl -u mosquitto -f       # log en vivo
cat /etc/mosquitto/conf.d/*.conf      # ver la configuración activa
```
