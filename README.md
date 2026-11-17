# Happy & Moist

Sistema IoT de alerta hídrica para Sabana Centro durante El Niño 2026. Challenge #3 del curso Internet de las Cosas de la Universidad de La Sabana.

## Qué presentamos

Un nodo con ESP32 mide el nivel del agua, la temperatura, la humedad, la presión y la luz. Con esos datos calcula la evaporación con la ecuación de Penman (1948). La alerta se activa solo cuando el nivel está bajo y además la evaporación es alta o el clima es anómalo.

El sistema avisa por tres vías:

- Pantalla LCD y buzzer en el sitio.
- Tablero web que sirve el ESP32 en la red local, con usuario y contraseña.
- Tablero en Ubidots para consultar desde cualquier lugar.

Los datos viajan por MQTT a una Raspberry Pi. Ella los guarda en SQLite y cada 5 minutos envía un promedio a Ubidots. Un modelo TinyML dentro del ESP32 predice el nivel a 12 horas y recomienda una acción a las autoridades.

## Documentación

Todo el detalle está en la [wiki](../../wiki): arquitectura, diseño, hardware, módulo de IA, pruebas, resultados y conclusiones.

## Enlaces

- Video del banco de pruebas: https://www.youtube.com/watch?v=zvBTsywOzQM
- Tablero en Ubidots: https://stem.ubidots.com/app/dashboards/public/dashboard/_SsdHF4HwAajMxC3ZZSDHGtdJXMxIqnh?navbar=true&contextbar=false&layersBar=false

## Equipo

- Lautaro Facundo Biondo de Marco
- Tiffany Cardona
- Nicolás Salazar
