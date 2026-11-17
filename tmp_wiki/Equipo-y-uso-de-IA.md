# Equipo y uso de IA

## Equipo

**Lautaro Facundo Biondo de Marco** se encargó de la Raspberry Pi, Mosquitto, el gateway y Ubidots, y corrigió el código. Aporte: 33.3 %.

**Tiffany Cardona** construyó la maqueta y el banco de pruebas, corrigió el circuito y trabajó en la wiki. Aporte: 33.3 %.

**Nicolás Salazar** construyó la maqueta y el banco de pruebas, corrigió el circuito y trabajó en la wiki. Aporte: 33.3 %.

## Uso de IA

Usamos Claude (Anthropic) con los modelos Claude Opus 5.5 y Claude Fable 5.2. Nos ayudó con el cableado y el diagnóstico del bus I2C, con el firmware del ESP32, con la revisión de la ecuación de Penman, con el gateway y la configuración de Mosquitto, y con la preparación de la sustentación y el borrador de esta wiki.

Conversación: https://claude.ai/code/session_01Y4y3Jvqpfp1otAkWf9JFkB

Algunas de las preguntas que le hicimos:

- "Vamos a hacer el cableado, dime todos y cada uno de los componentes hacia donde van sus patas"
- "Sale bh1750 error recieved nack on trabsmit of data"
- "Necesito que pienses bien y me expliques como defender que usamos bien la formula para el contexto del problema"
- "Quiero hacer lo demás servidor web mientras tiffany hace cableado, por donde empezamos"
- "Como probamos la radiación, necesito saber que el valor que da es real"
- "necesitamos hacer la documentacion para la wiki"

Nada lo tomamos como cierto sin probarlo. El cableado y el firmware los probamos en la placa. La ecuación la comparamos con el artículo de Penman. El gateway lo probamos viendo llegar los datos a Ubidots. Las métricas de la IA las recalculamos con el script. Cuando la IA y la prueba no coincidían nos quedamos con la prueba.

Calculamos que cerca del 75 % del código del firmware salió de la IA y el 25 % restante lo escribimos o corregimos nosotros.

## Anexos

Código del ESP32: [HappyMoist.ino](https://github.com/Nickjjlkm/IOT-challenge-3/blob/main/codigo/HappyMoist.ino) y [modelo_tinyml.h](https://github.com/Nickjjlkm/IOT-challenge-3/blob/main/codigo/modelo_tinyml.h).

Código de la Raspberry Pi: [gateway.py](https://github.com/Nickjjlkm/IOT-challenge-3/blob/main/codigo/gateway.py), [env.ejemplo](https://github.com/Nickjjlkm/IOT-challenge-3/blob/main/codigo/env.ejemplo) y [mosquitto_config.md](https://github.com/Nickjjlkm/IOT-challenge-3/blob/main/codigo/mosquitto_config.md).

Entrenamiento de la IA: [entrenar_modelo.py](https://github.com/Nickjjlkm/IOT-challenge-3/blob/main/codigo/entrenar_modelo.py).

En el código cambiamos las claves de Wi-Fi y MQTT, la IP de la Raspberry y el token de Ubidots por textos de ejemplo.
