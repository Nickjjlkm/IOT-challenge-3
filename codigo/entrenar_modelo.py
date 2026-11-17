#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
HAPPY & MOIST - Modulo de IA (TinyML)
=====================================================================
1) Simula un tanque/embalse con clima de montana (~2600 m) y genera
   datos sinteticos: nivel, temperatura, humedad, presion, radiacion,
   evaporacion (Penman, identico al firmware), consumo, lluvias, recargas.
2) Entrena una red neuronal pequena con DOS salidas:
     - clase     : recomendacion (0 NORMAL, 1 VIGILANCIA, 2 PREVENTIVA, 3 CRITICA)
     - nivel12h  : nivel previsto dentro de HORIZONTE_H horas (cm / ALTURA)
3) Convierte a TensorFlow Lite y escribe modelo_tinyml.h para el ESP32.

Uso:
    pip install numpy tensorflow
    python entrenar_modelo.py                 # todo el proceso
    python entrenar_modelo.py --solo-datos    # solo genera el CSV
=====================================================================
"""
import argparse
import os
import sys
import numpy as np

# ---------------------------------------------------------------------
# Parametros: DEBEN coincidir con el firmware del ESP32
# ---------------------------------------------------------------------
ALTURA = 200.0        # cm (ALTURA_TANQUE_CM)
NIVEL_ALERTA = 40.0   # cm
EVAP_ALERTA = 4.0     # mm/dia
HUM_ALERTA = 20.0     # %
PRES_ALERTA = 735.0   # hPa
ALBEDO = 0.08
U2 = 2.0              # viento asumido m/s

HORIZONTE_H = 12      # la IA predice el nivel dentro de 12 h
VENTANA_H = 3         # las tendencias se calculan sobre las ultimas 3 h
CLIP_DN = 10.0        # cm/h   (limite de la tendencia de nivel)
CLIP_DP = 3.0         # hPa/h  (limite de la tendencia de presion)

FEATURES = ["nivel", "temp", "hum", "presion", "radiacion", "evap",
            "dNivel_h", "dPres_h"]
CLASES = ["NORMAL", "VIGILANCIA", "PREVENTIVA", "CRITICA"]


# ---------------------------------------------------------------------
# Fisica: Penman (copia exacta de calcularPenman() del firmware)
# ---------------------------------------------------------------------
def penman(T, HR, P_hpa, rad, u2=U2):
    k, z, z0, cp, Rd, rhoW = 0.41, 2.0, 0.0002, 1005.0, 287.05, 1000.0
    T_K = T + 273.15
    P = P_hpa * 100.0
    es = 0.6108 * np.exp(17.27 * T / (T + 237.3))
    ea = es * (HR / 100.0)
    de = (es - ea) * 1000.0
    m = 1000.0 * (4098.0 * es) / (T + 237.3) ** 2
    lv = (2.501 - 0.002361 * T) * 1.0e6
    gam = 0.665e-3 * P
    rho = P / (Rd * T_K)
    ga = (k * k * u2) / np.log(z / z0) ** 2
    Rn = np.maximum(rad * (1.0 - ALBEDO), 0.0)
    E = (m * Rn + rho * cp * de * ga) / (lv * (m + gam))
    E = np.maximum(E, 0.0)
    return E / rhoW * 1000.0 * 86400.0          # mm/dia


# ---------------------------------------------------------------------
# Generador de datos
# ---------------------------------------------------------------------
def ar1(rng, n, phi, sigma):
    x = np.zeros(n)
    e = rng.normal(0.0, sigma, n)
    for i in range(1, n):
        x[i] = phi * x[i - 1] + e[i]
    return x


def simular_serie(rng, horas):
    """Una serie horaria completa (un tanque durante `horas` horas)."""
    t = np.arange(horas)
    hod = t % 24

    # --- Lluvias (inflow) y caida de presion que las anticipa ---------
    lluvia = np.zeros(horas)                     # cm/h que entran
    pres_dev = np.zeros(horas)
    n_ev = rng.poisson(horas / 24.0 / rng.uniform(2.0, 8.0))
    for _ in range(n_ev):
        s = int(rng.integers(0, horas))
        d = int(rng.integers(3, 13))
        lluvia[s:min(horas, s + d)] += rng.uniform(0.2, 2.0)
        pres_dev -= rng.uniform(3.0, 9.0) * np.exp(-0.5 * ((t - s) / 6.0) ** 2)

    # --- Episodios secos (humedad muy baja, mas calor y sol) ----------
    seco = np.zeros(horas)
    n_sec = rng.poisson(horas / 24.0 / rng.uniform(6.0, 20.0))
    for _ in range(n_sec):
        s = int(rng.integers(0, horas))
        d = int(rng.integers(24, 97))
        e = min(horas, s + d)
        seco[s:e] = np.maximum(seco[s:e], np.hanning(e - s + 2)[1:-1] * rng.uniform(0.5, 1.0))

    # --- Nubosidad, temperatura, radiacion, humedad, presion ----------
    nub = np.clip(0.5 + ar1(rng, horas, 0.95, 0.15), 0.0, 1.0)
    nub = np.where(lluvia > 0, np.maximum(nub, 0.9), nub)
    nub = nub * (1.0 - 0.8 * seco)

    T_media = rng.uniform(10.0, 18.0)
    temp = (T_media + 4.0 * np.sin(2 * np.pi * (hod - 9) / 24.0)
            + 2.0 * (0.5 - nub) + ar1(rng, horas, 0.97, 0.25)
            + 3.0 * seco - 1.5 * (lluvia > 0))

    sol = np.maximum(0.0, np.sin(np.pi * (hod - 6) / 12.0))
    sol = np.where((hod >= 6) & (hod <= 18), sol, 0.0)
    rad = 950.0 * sol * (1.0 - 0.75 * nub)
    rad = np.where(lluvia > 0, rad * 0.3, rad)
    rad = np.maximum(rad, 0.0)

    hum = (72.0 - 2.5 * (temp - T_media) + 8.0 * nub - 55.0 * seco
           + ar1(rng, horas, 0.9, 1.5))
    hum = np.where(lluvia > 0, np.maximum(hum, 90.0), hum)
    hum = np.clip(hum, 8.0, 100.0)

    pres = 745.0 + ar1(rng, horas, 0.98, 0.25) + pres_dev
    pres = np.clip(pres, 725.0, 760.0)

    evap_mmdia = penman(temp, hum, pres, rad)

    # --- Consumo por regimenes (cm/h) + patron diario ------------------
    cons = np.zeros(horas)
    i = 0
    while i < horas:
        L = int(rng.integers(48, 240))
        base = rng.choice([0.05, 0.2, 0.5, 1.0, 1.8]) * rng.uniform(0.7, 1.3)
        cons[i:i + L] = base
        i += L
    cons *= 1.0 + 0.6 * np.sin(2 * np.pi * (hod - 12) / 24.0)

    # --- Balance de agua ------------------------------------------------
    umbral_recarga = rng.uniform(25.0, 60.0)
    p_recarga = rng.choice([0.003, 0.02, 0.06, 0.12])
    nivel = np.zeros(horas)
    nivel[0] = rng.uniform(10.0, 195.0)
    evap_cmh = evap_mmdia / 10.0 / 24.0
    for i in range(horas - 1):
        n = nivel[i] - cons[i] - evap_cmh[i] + lluvia[i]
        if n < umbral_recarga and rng.random() < p_recarga:
            n += rng.uniform(60.0, 140.0)
        nivel[i + 1] = min(max(n, 0.0), ALTURA)

    return nivel, temp, hum, pres, rad


def etiquetar(now, fut, evap, hum, pres):
    clima = (evap >= EVAP_ALERTA) | (hum < HUM_ALERTA) | (pres < PRES_ALERTA)
    y = np.zeros(len(now), dtype=np.int64)
    vigil = ((now <= NIVEL_ALERTA) & (fut > NIVEL_ALERTA)) | (fut <= 80.0) | \
            ((now <= 100.0) & clima)
    y[vigil] = 1
    y[(now > NIVEL_ALERTA) & (fut <= NIVEL_ALERTA)] = 2     # cruzara el umbral
    y[(now <= NIVEL_ALERTA) & (fut <= NIVEL_ALERTA)] = 3    # seguira bajo
    return y


def generar_dataset(n_series, horas, semilla):
    rng = np.random.default_rng(semilla)
    X, Yc, Yn, G = [], [], [], []
    for s in range(n_series):
        nivel, temp, hum, pres, rad = simular_serie(rng, horas)

        # Lo que "ve" el ESP32: los sensores tienen ruido
        nivel_m = np.clip(nivel + rng.normal(0, 0.5, horas), 0, ALTURA)
        temp_m = temp + rng.normal(0, 0.3, horas)
        hum_m = np.clip(hum + rng.normal(0, 2.0, horas), 0, 100)
        pres_m = pres + rng.normal(0, 0.3, horas)
        rad_m = np.maximum(rad * (1 + rng.normal(0, 0.05, horas)), 0)
        evap_m = penman(temp_m, hum_m, pres_m, rad_m)

        idx = np.arange(VENTANA_H, horas - HORIZONTE_H)
        dN = np.clip((nivel_m[idx] - nivel_m[idx - VENTANA_H]) / VENTANA_H, -CLIP_DN, CLIP_DN)
        dP = np.clip((pres_m[idx] - pres_m[idx - VENTANA_H]) / VENTANA_H, -CLIP_DP, CLIP_DP)

        x = np.column_stack([nivel_m[idx], temp_m[idx], hum_m[idx], pres_m[idx],
                             rad_m[idx], evap_m[idx], dN, dP])
        fut = nivel[idx + HORIZONTE_H]
        yc = etiquetar(nivel[idx], fut, evap_m[idx], hum_m[idx], pres_m[idx])

        X.append(x); Yc.append(yc); Yn.append(fut / ALTURA)
        G.append(np.full(len(idx), s))
    return (np.vstack(X).astype(np.float32), np.concatenate(Yc),
            np.concatenate(Yn).astype(np.float32), np.concatenate(G))


def balancear(X, yc, yn, rng, frac=0.5):
    """Repite ejemplos de las clases raras hasta `frac` de la clase mayor."""
    cuentas = np.bincount(yc, minlength=4)
    objetivo = int(cuentas.max() * frac)
    idx = [np.arange(len(yc))]
    for c in range(4):
        if 0 < cuentas[c] < objetivo:
            pos = np.where(yc == c)[0]
            idx.append(rng.choice(pos, objetivo - cuentas[c], replace=True))
    idx = rng.permutation(np.concatenate(idx))
    return X[idx], yc[idx], yn[idx]


# ---------------------------------------------------------------------
# Modelo
# ---------------------------------------------------------------------
def construir_modelo():
    from tensorflow import keras
    from tensorflow.keras import layers
    entrada = keras.Input(shape=(len(FEATURES),), name="entrada")
    x = layers.Dense(32, activation="relu")(entrada)
    x = layers.Dense(16, activation="relu")(x)
    clase = layers.Dense(4, activation="softmax", name="clase")(x)
    nivel = layers.Dense(1, activation="linear", name="nivel12h")(x)
    m = keras.Model(entrada, [clase, nivel])
    m.compile(optimizer=keras.optimizers.Adam(1e-3),
              loss={"clase": "sparse_categorical_crossentropy", "nivel12h": "mse"},
              loss_weights={"clase": 1.0, "nivel12h": 20.0},
              metrics={"clase": ["accuracy"], "nivel12h": ["mae"]})
    return m


def a_salidas(pred):
    """Devuelve (probs, nivel) sin importar el orden de las salidas."""
    a, b = pred
    return (a, b) if a.shape[-1] == 4 else (b, a)


def escribir_header(tflite, mu, sd, ruta):
    with open(ruta, "w", encoding="utf-8") as f:
        f.write("// Generado por entrenar_modelo.py -- NO EDITAR A MANO\n#pragma once\n\n")
        f.write(f"#define TINYML_N_ENTRADAS {len(mu)}\n")
        f.write(f"#define TINYML_HORIZONTE_H {HORIZONTE_H}\n")
        f.write(f"#define TINYML_VENTANA_H {VENTANA_H}\n")
        f.write(f"const float TINYML_CLIP_DN = {CLIP_DN:.1f}f;\n")
        f.write(f"const float TINYML_CLIP_DP = {CLIP_DP:.1f}f;\n")
        f.write(f"const float TINYML_ALTURA = {ALTURA:.1f}f;\n")
        f.write("const float TINYML_MEDIA[TINYML_N_ENTRADAS] = {" +
                ", ".join(f"{v:.6f}f" for v in mu) + "};\n")
        f.write("const float TINYML_DESV[TINYML_N_ENTRADAS] = {" +
                ", ".join(f"{v:.6f}f" for v in sd) + "};\n\n")
        f.write("alignas(8) const unsigned char modelo_tflite[] = {\n")
        for i in range(0, len(tflite), 12):
            f.write("  " + ", ".join(f"0x{b:02x}" for b in tflite[i:i + 12]) + ",\n")
        f.write("};\n")
        f.write(f"const unsigned int modelo_tflite_len = {len(tflite)};\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--series", type=int, default=400, help="tanques simulados")
    ap.add_argument("--horas", type=int, default=1440, help="horas por serie (1440 = 60 dias)")
    ap.add_argument("--epocas", type=int, default=60)
    ap.add_argument("--semilla", type=int, default=7)
    ap.add_argument("--salida", default=".", help="carpeta de salida")
    ap.add_argument("--solo-datos", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.salida, exist_ok=True)

    print(f"Generando {a.series} series x {a.horas} h ...")
    X, yc, yn, G = generar_dataset(a.series, a.horas, a.semilla)
    print(f"Muestras: {len(X)}")
    cu = np.bincount(yc, minlength=4)
    for c in range(4):
        print(f"  clase {c} {CLASES[c]:<11}: {cu[c]:>8}  ({100 * cu[c] / len(yc):.1f} %)")

    csv = os.path.join(a.salida, "datos_sinteticos.csv")
    np.savetxt(csv, np.column_stack([G, X, yc, yn * ALTURA]), delimiter=",", fmt="%.4f",
               header="serie," + ",".join(FEATURES) + ",clase,nivel_futuro_cm", comments="")
    print("CSV guardado en", csv)
    if a.solo_datos:
        return

    import tensorflow as tf
    from tensorflow import keras
    rng = np.random.default_rng(a.semilla)
    tf.random.set_seed(a.semilla)

    # Separacion por SERIE (no por fila) para no filtrar informacion
    series = np.unique(G)
    rng.shuffle(series)
    val_series = set(series[: max(1, int(0.2 * len(series)))])
    es_val = np.array([g in val_series for g in G])
    Xtr, yctr, yntr = X[~es_val], yc[~es_val], yn[~es_val]
    Xva, ycva, ynva = X[es_val], yc[es_val], yn[es_val]

    mu = Xtr.mean(axis=0)
    sd = Xtr.std(axis=0) + 1e-6
    Xtr, yctr, yntr = balancear(Xtr, yctr, yntr, rng)
    Xtr_n = (Xtr - mu) / sd
    Xva_n = (Xva - mu) / sd

    modelo = construir_modelo()
    modelo.summary()
    modelo.fit(Xtr_n, {"clase": yctr, "nivel12h": yntr},
               validation_data=(Xva_n, {"clase": ycva, "nivel12h": ynva}),
               epochs=a.epocas, batch_size=256, verbose=2,
               callbacks=[keras.callbacks.EarlyStopping(patience=8, restore_best_weights=True)])

    probs, niv = a_salidas(modelo.predict(Xva_n, verbose=0))
    pred = probs.argmax(axis=1)
    print(f"\nExactitud clase (validacion): {100 * (pred == ycva).mean():.1f} %")
    print(f"Error medio del nivel a {HORIZONTE_H} h: {np.abs(niv[:, 0] - ynva).mean() * ALTURA:.1f} cm")
    cm = np.zeros((4, 4), dtype=int)
    for r, p in zip(ycva, pred):
        cm[r, p] += 1
    print("Matriz de confusion (filas = real, columnas = predicho):")
    print(cm)

    # --- Conversion a TFLite (float32) ---------------------------------
    conv = tf.lite.TFLiteConverter.from_keras_model(modelo)
    tflite = conv.convert()
    print(f"\nModelo TFLite: {len(tflite)} bytes")

    # Verificacion: el modelo convertido debe coincidir con el de Keras
    it = tf.lite.Interpreter(model_content=tflite)
    it.allocate_tensors()
    ent = it.get_input_details()[0]
    sal = it.get_output_details()
    ok = 0
    n_chk = 300
    for i in range(n_chk):
        it.set_tensor(ent["index"], Xva_n[i:i + 1].astype(np.float32))
        it.invoke()
        for d in sal:
            if d["shape"][-1] == 4:
                ok += int(it.get_tensor(d["index"])[0].argmax() == pred[i])
    print(f"Coincidencia TFLite vs Keras: {ok}/{n_chk}")

    ruta_h = os.path.join(a.salida, "modelo_tinyml.h")
    escribir_header(tflite, mu, sd, ruta_h)
    with open(os.path.join(a.salida, "modelo.tflite"), "wb") as f:
        f.write(tflite)
    print("Header para Arduino:", ruta_h)


if __name__ == "__main__":
    sys.exit(main())
