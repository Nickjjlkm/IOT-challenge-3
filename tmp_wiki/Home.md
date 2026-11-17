# Happy & Moist

Sistema IoT de alerta hídrica para Sabana Centro durante El Niño 2026.

Challenge #3 · Internet de las Cosas · Universidad de La Sabana · 2026-2

Equipo: Lautaro Facundo Biondo de Marco, Tiffany Cardona y Nicolás Salazar

## Abstract

Happy & Moist is a low-cost IoT system that monitors water storage points in Sabana Centro (Colombia) during the 2026 El Niño event. An ESP32 node measures water level, temperature, humidity, pressure and light, and estimates evaporation with the Penman (1948) equation. An alert is raised only when a low level comes with high evaporation or unusual weather. Alerts show up on the device, on a local web dashboard and on Ubidots. Data travel over MQTT to a Raspberry Pi gateway that stores them in SQLite. A TinyML model on the ESP32 forecasts the level 12 hours ahead and recommends an action.

## Resumen

Un ESP32 mide el nivel del agua, la temperatura, la humedad, la presión y la luz. Con eso calcula la evaporación usando Penman (1948).

La alerta no depende de una sola variable. Se activa cuando el nivel está bajo y además la evaporación es alta o el clima es anómalo. Así un consumo puntual o un día caluroso no generan falsas alarmas.

El aviso llega por tres lados. En el sitio con una pantalla LCD y un buzzer. En un tablero web que sirve el propio ESP32 con usuario y contraseña. Y en Ubidots para consultar desde cualquier lugar.

Los datos viajan por MQTT a una Raspberry Pi. Ella los guarda en SQLite y cada 5 minutos manda un promedio a Ubidots. Un modelo TinyML dentro del ESP32 predice el nivel a 12 horas y recomienda qué hacer.

## Motivación

En 2026 el IDEAM confirmó la llegada de El Niño. La CAR señaló 20 municipios de Cundinamarca en riesgo extremo de quedarse sin agua. Las redes oficiales no tienen datos en tiempo real de cada reservorio local. Hacía falta algo barato que cruzara el clima con el nivel del agua.

## Estructura

1. [Diseño](Diseno): restricciones, arquitectura, hardware y módulos.
2. [Módulo de IA](Modulo-de-IA).
3. [Pruebas y resultados](Pruebas-y-resultados).
4. [Conclusiones](Conclusiones): conclusiones, retos, trabajo futuro, modelo de negocio y referencias.
5. [Equipo y uso de IA](Equipo-y-uso-de-IA): roles, uso de IA y anexos.

## Enlaces

- Video: https://www.youtube.com/watch?v=zvBTsywOzQM
- Tablero en Ubidots: https://stem.ubidots.com/app/dashboards/public/dashboard/_SsdHF4HwAajMxC3ZZSDHGtdJXMxIqnh?navbar=true&contextbar=false&layersBar=false
