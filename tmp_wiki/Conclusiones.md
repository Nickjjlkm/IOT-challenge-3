# Conclusiones

## Conclusiones

- Construimos un sistema IoT completo que va del sensor a la nube.
- El aporte central es la fusión de señales. Pedir nivel bajo y además evaporación alta o clima anómalo evita falsas alarmas.
- Penman (1948) en flujo de masa sirve para agua libre y se puede calcular con sensores baratos.
- FreeRTOS reparte el trabajo en hilos. Ningún módulo bloquea a los otros.
- Hay redundancia. Si se cae la red la alarma del sitio sigue sonando. Si se cae internet el gateway guarda los datos.
- TinyML permite recomendar sin internet. Para usarlo en campo hay que reentrenarlo con datos reales.

## Retos

| Reto | Solución |
|---|---|
| Pasar de Arduino Uno a ESP32 | Reescribimos el firmware y el cableado a 3.3 V con divisor para el ultrasónico |
| Bus I2C trabado y BMP180 intermitente | Lo diagnosticamos con pruebas de nivel lógico y escáner I2C. Eran cables en la fila equivocada y soldaduras malas |
| Zona ciega del ultrasónico | Subimos el sensor y descartamos lecturas fuera de rango |
| Faltaban cables hembra-hembra | Unimos dos macho-hembra en el protoboard |
| ESP32 sin energía con cable C a C | Usamos un cable A a C |
| Sin datos reales para la IA | Generamos datos simulados y declaramos la limitación |

## Trabajo futuro

1. Probarlo semanas en un embalse y comparar con el IDEAM.
2. Reentrenar la IA con datos reales.
3. Enviar la predicción de la IA a Ubidots y silenciar desde la nube.
4. Avisos por correo o SMS con eventos de Ubidots.
5. Bajo consumo con deep sleep y panel solar.
6. MQTT con TLS y HTTPS en el tablero.
7. Agregar anemómetro y piranómetro.

## Modelo de negocio

| Bloque | Contenido |
|---|---|
| Clientes | Alcaldías, acueductos veredales, distritos de riego y la CAR |
| Propuesta de valor | Alerta temprana con menos falsas alarmas, predicción a 12 h y costo muy bajo frente a una estación oficial |
| Canales | Convenios con alcaldías y presentaciones ante la CAR |
| Relación | Instalación, capacitación y mantenimiento mensual |
| Ingresos | Venta del nodo y suscripción al tablero y al mantenimiento |
| Recursos | Hardware, firmware, modelo de IA y equipo técnico |
| Actividades | Fabricar, instalar, mantener y reentrenar |
| Socios | Proveedores de electrónica, Ubidots, IDEAM y la universidad |
| Costos | Unos 195 000 COP por nodo, gateway, nube e instalación |

Un gateway atiende varios nodos y su costo se reparte entre todos.

## Referencias

[1] Minambiente, "Gobierno confirma inicio del fenómeno de El Niño y alerta sobre su alcance," 2026. https://www.minambiente.gov.co/gobierno-confirma-inicio-del-fenomeno-de-el-nino-y-alerta-sobre-su-alcance/

[2] Caracol Radio, "Fenómeno del Niño: los municipios en Cundinamarca con mayor riesgo de desabastecimiento de agua," jun. 2026. https://caracol.com.co/2026/06/16/fenomeno-del-nino-los-municipios-en-cundinamarca-con-mayor-riesgo-de-desabastecimiento-de-agua/

[3] H. L. Penman, "Natural evaporation from open water, bare soil and grass," *Proc. R. Soc. Lond. A*, vol. 193, pp. 120–145, 1948. doi: 10.1098/rspa.1948.0037

[4] R. G. Allen et al., *Crop evapotranspiration*, FAO Irrigation and Drainage Paper 56, 1998. https://www.fao.org/4/x0490e/x0490e00.htm

[5] OASIS, *MQTT Version 3.1.1*, 2014. https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/mqtt-v3.1.1.html

[6] Espressif, *ESP32 Datasheet*. https://www.espressif.com/sites/default/files/documentation/esp32_datasheet_en.pdf

[7] TensorFlow Lite for Microcontrollers. https://github.com/tensorflow/tflite-micro

[8] Eclipse Mosquitto. https://mosquitto.org/

[9] Ubidots Docs. https://docs.ubidots.com/
