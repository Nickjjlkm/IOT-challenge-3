# Equipo y uso de IA

## Equipo

| Integrante | Qué hizo | % |
|---|---|---|
| Lautaro Facundo Biondo de Marco | Raspberry Pi, Mosquitto, gateway, Ubidots y correcciones del código | 33.3 |
| Tiffany Cardona | Maqueta, banco de pruebas, correcciones del circuito y wiki | 33.3 |
| Nicolás Salazar | Maqueta, banco de pruebas, correcciones del circuito y wiki | 33.3 |

## Uso de IA

Usamos Claude (Anthropic) para:

- Cableado y diagnóstico del bus I2C.
- Firmware del ESP32: servidor web, hilos, MQTT y silenciado.
- Revisar la ecuación de Penman y sus fuentes.
- El gateway y la configuración de Mosquitto.
- Preparar la sustentación y el borrador de esta wiki.

Conversación: https://claude.ai/code/session_01Y4y3Jvqpfp1otAkWf9JFkB

Algunas de las preguntas que le hicimos:

- "Vamos a hacer el cableado, dime todos y cada uno de los componentes hacia donde van sus patas"
- "Sale bh1750 error recieved nack on trabsmit of data"
- "Necesito que pienses bien y me expliques como defender que usamos bien la formula para el contexto del problema"
- "Quiero hacer lo demás servidor web mientras tiffany hace cableado, por donde empezamos"
- "Como probamos la radiación, necesito saber que el valor que da es real"
- "necesitamos hacer la documentacion para la wiki"

**Cómo lo validamos.** Nada se tomó como cierto sin probarlo. El cableado y el firmware se probaron en la placa. La ecuación se comparó con el artículo de Penman. El gateway se probó con `mosquitto_sub` y con datos llegando a Ubidots. Las métricas de la IA se recalcularon con el script. Cuando la IA y la prueba no coincidían nos quedamos con la prueba.

| Parte | Hecho con IA | Hecho por el equipo |
|---|---|---|
| Texto de la wiki | 80 % | 20 % |
| Código del firmware | 75 % | 25 % |

## Anexos

| Archivo | Dónde corre |
|---|---|
| [`HappyMoist.ino`](https://github.com/Nickjjlkm/IOT-challenge-3/blob/main/codigo/HappyMoist.ino) | ESP32 |
| [`modelo_tinyml.h`](https://github.com/Nickjjlkm/IOT-challenge-3/blob/main/codigo/modelo_tinyml.h) | ESP32 |
| [`entrenar_modelo.py`](https://github.com/Nickjjlkm/IOT-challenge-3/blob/main/codigo/entrenar_modelo.py) | PC |
| [`gateway.py`](https://github.com/Nickjjlkm/IOT-challenge-3/blob/main/codigo/gateway.py) | Raspberry Pi |
| [`env.ejemplo`](https://github.com/Nickjjlkm/IOT-challenge-3/blob/main/codigo/env.ejemplo) | Raspberry Pi |
| [`mosquitto_config.md`](https://github.com/Nickjjlkm/IOT-challenge-3/blob/main/codigo/mosquitto_config.md) | Raspberry Pi |

Las claves de Wi-Fi y MQTT, la IP de la Raspberry y el token de Ubidots se cambiaron por textos de ejemplo.

Librerías del ESP32: DHT sensor library, Adafruit BMP085, BH1750, PubSubClient y Chirale_TensorFlowLite.
