# Módulo de IA

## Qué hace

Una red neuronal pequeña corre dentro del ESP32. Predice el nivel del agua dentro de 12 horas y recomienda una de cuatro acciones:

- **Normal:** seguir monitoreando.
- **Vigilancia:** revisar el tablero más seguido.
- **Preventiva:** el nivel va a cruzar el umbral. Reducir el consumo y programar una recarga.
- **Crítica:** el nivel está bajo y seguirá así. Restringir el uso y activar el plan de contingencia.

La IA no decide la alarma. Eso lo hace la regla de alerta. Si la IA falla el sistema avisa igual.

## Cómo funciona

Recibe ocho valores: nivel, temperatura, humedad, presión, radiación, evaporación y cuánto han cambiado el nivel y la presión en las últimas 3 horas. Pasa por dos capas de 32 y 16 neuronas y sale con la clase y el nivel previsto.

Pesa 6 KB y usa 16 KB de memoria. Si falta el dato de algún sensor no responde, para no inventar.

## Entrenamiento

Usamos datos simulados. Un embalse cambia en semanas y en el semestre solo habríamos medido unos días sin sequías ni lluvias. Con un simulador generamos meses de cientos de tanques distintos, incluidos los casos raros que más importan.

El simulador sigue un tanque de 200 cm hora por hora durante 60 días. Tiene lluvias con una caída de presión antes, días secos, nubes, consumo y recargas. La evaporación usa la misma función de Penman del firmware y le agregamos ruido como el de un sensor real.

Generamos 400 tanques, unas 570 000 muestras. Entrenamos con el 80 % de los tanques y validamos con el resto. El código está en [entrenar_modelo.py](https://github.com/Nickjjlkm/IOT-challenge-3/blob/main/codigo/entrenar_modelo.py).

## Resultados

En validación acierta el 89.5 % de las veces. Detecta el 98.8 % de los casos críticos y el 88.8 % de los preventivos. Ninguno de esos dos se confundió con normal.

El punto débil es que la mitad de sus avisos preventivos eran en realidad de vigilancia. Para una alerta preferimos avisar de más.

La predicción del nivel falla en promedio por 10.4 cm. Suponer que el nivel no cambia falla por 11.3 cm, así que mejora poco. Lo útil del modelo es la recomendación.

## Por qué TinyML

Funciona sin internet, no cuesta nada por consulta y responde en milisegundos. Siempre da la misma respuesta con los mismos datos y nada sale de la red local. En zonas rurales con mala conexión eso pesa más que usar una API como ChatGPT.

## Limitaciones

Las métricas miden qué tan bien aprendió el simulador. Falta probarlo con un embalse real. Está entrenado para un tanque de 200 cm y con otro tamaño habría que reentrenarlo. En la demo las tendencias se miden en un minuto y no en 3 horas, así que ahí sus predicciones no son confiables.
