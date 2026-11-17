# Conclusiones

## Conclusiones

Construimos un sistema IoT completo que va del sensor a la nube.

Lo más importante es la regla de alerta. Pedir nivel bajo y además evaporación alta o clima anómalo evita falsas alarmas.

Penman (1948) sirve para agua libre y se puede calcular con sensores baratos.

Repartir el trabajo en hilos hace que ningún módulo bloquee a los otros. Si se cae la red la alarma del sitio sigue sonando, y si se cae internet el gateway guarda los datos.

TinyML permite recomendar sin internet. Para usarlo en campo hay que reentrenarlo con datos reales.

## Retos

Pasar de Arduino Uno a ESP32 nos obligó a reescribir el firmware y rehacer el cableado a 3.3 V.

El bus I2C se trababa y el BMP180 fallaba a ratos. Con pruebas de nivel lógico y un escáner encontramos cables en la fila equivocada y soldaduras malas.

El ultrasónico no mide a menos de 25 cm, así que lo subimos y descartamos las lecturas fuera de rango.

Nos faltaron cables hembra-hembra y los reemplazamos uniendo dos macho-hembra en el protoboard. El ESP32 no prendía con un cable USB-C a USB-C y tuvimos que usar uno USB-A a USB-C.

No había datos reales para entrenar la IA, así que los simulamos y lo declaramos como limitación.

## Trabajo futuro

Dejar el nodo varias semanas en un embalse y comparar con el IDEAM. Con esos datos reentrenar la IA.

Enviar la predicción de la IA a Ubidots, poder silenciar desde la nube y mandar avisos por correo o SMS.

Bajar el consumo con modo de sueño y panel solar. Cifrar MQTT y el tablero. Agregar anemómetro y piranómetro.

## Modelo de negocio

Los clientes serían alcaldías, acueductos veredales, distritos de riego y la CAR. Les ofrecemos una alerta temprana con menos falsas alarmas y una predicción a 12 horas, por mucho menos que una estación oficial.

Llegaríamos con convenios con alcaldías y presentaciones ante la CAR. El ingreso viene de vender e instalar el nodo y de una suscripción mensual al tablero y al mantenimiento. Los socios serían proveedores de electrónica, Ubidots, el IDEAM y la universidad.

El nodo cuesta unos 195 000 COP. A eso se suman el gateway, la nube y la instalación. Un gateway atiende varios nodos y su costo se reparte entre todos.

## Referencias

[1] Minambiente, "Gobierno confirma inicio del fenómeno de El Niño y alerta sobre su alcance," 2026. https://www.minambiente.gov.co/gobierno-confirma-inicio-del-fenomeno-de-el-nino-y-alerta-sobre-su-alcance/

[2] Caracol Radio, "Fenómeno del Niño: los municipios en Cundinamarca con mayor riesgo de desabastecimiento de agua," jun. 2026. https://caracol.com.co/2026/06/16/fenomeno-del-nino-los-municipios-en-cundinamarca-con-mayor-riesgo-de-desabastecimiento-de-agua/

[3] H. L. Penman, "Natural evaporation from open water, bare soil and grass," Proc. R. Soc. Lond. A, vol. 193, pp. 120–145, 1948. doi: 10.1098/rspa.1948.0037

[4] R. G. Allen et al., Crop evapotranspiration, FAO Irrigation and Drainage Paper 56, 1998. https://www.fao.org/4/x0490e/x0490e00.htm

[5] OASIS, MQTT Version 3.1.1, 2014. https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/mqtt-v3.1.1.html

[6] Espressif, ESP32 Datasheet. https://www.espressif.com/sites/default/files/documentation/esp32_datasheet_en.pdf

[7] TensorFlow Lite for Microcontrollers. https://github.com/tensorflow/tflite-micro

[8] Eclipse Mosquitto. https://mosquitto.org/

[9] Ubidots Docs. https://docs.ubidots.com/
