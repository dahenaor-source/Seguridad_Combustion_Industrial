# Seguridad de Combustión Industrial

**Sistema autónomo de monitoreo atmosférico y seguridad diferenciada para recintos
industriales de combustión limpia** (gas natural CH₄ / GLP).

Arquitectura distribuida: unidad central **BeaglePlay** (Linux embebido) + red malla
**Thread (IEEE 802.15.4)** con nodos **ESP32-C6** que se comunican por **CoAP sobre UDP/IPv6**
con payloads **CBOR**.

---

## Idea del sistema

En salas de hornos (cerámica, vidrio, metales) y cuartos de calderas a gas, los riesgos
atmosféricos son **predecibles** y dependen de la química de la combustión. El sistema aplica
**mitigación diferenciada** según el gas detectado:

| Riesgo | Gas | Acción de seguridad |
|--------|-----|---------------------|
| **Combustión incompleta** | CO (monóxido de carbono) | **Extracción forzada al 100%** + alarma |
| **Fuga de combustible sin quemar** | CH₄ / GLP | **Cortar el gas** (electroválvula NC) + evacuación. **NO** encender extractores (riesgo de chispa) |

> **Fail-safe inverso:** si un nodo pierde comunicación o falla una sonda, el sistema pasa a
> **estado seguro** (corte de gas o extracción según la zona) en vez de quedarse esperando.

---

## Topología de nodos

| Nodo | Zona | Sensores | Actuador | Recurso CoAP |
|------|------|----------|----------|--------------|
| **Central** | Caseta/tablero | — | 5 LEDs de estado | — |
| **Nodo 1** | Tren de gas / quemadores | MQ-6 (CH₄/GLP) | Relé → electroválvula NC | `/act/cutoff` |
| **Nodo 2** | Cuerpo del horno / chimenea | MQ-7 (CO) + BME680 (I²C) | Relé → extractor | `/act/extractor` |
| **Nodo 3** | Pasillo de operarios | MQ-7 (CO) | Buzzer + baliza LED | `/act/alarm` |

Los nodos forman una malla autorrecuperable en **Canal 25 (2475 MHz)** y operan de forma
autónoma aunque se apague la central.

---

## Estructura del repositorio

```
firmware/
  nodo1_tren_gas/     ← Nodo 1: corte de gas (ESP-IDF, ESP32-C6)
  nodo2_horno/        ← Nodo 2: CO + BME680 + extractor (ESP-IDF, ESP32-C6)
  nodo3_pasillo/      ← Nodo 3: CO + buzzer + baliza LED (ESP-IDF, ESP32-C6)
docs/
  ARQUITECTURA.md          ← visión general del sistema y los nodos
  REQUERIMIENTOS.md        ← requerimientos funcionales y no funcionales
  MATRIZ_DE_VERIFICACION.md← plan de pruebas (PV-01 a PV-10)
  CANALES.md               ← selección de canal (Energy Scan en la BeaglePlay)
  GUIA_COMPILACION.md      ← cómo compilar y flashear en Ubuntu
```

---

## Estado del proyecto

- [x] **Nodo 1 — Tren de Gas** — firmware validado en placa → [`firmware/nodo1_tren_gas/`](firmware/nodo1_tren_gas/)
- [x] **Nodo 2 — Horno / Chimenea** — firmware validado en placa → [`firmware/nodo2_horno/`](firmware/nodo2_horno/)
- [x] **Nodo 3 — Pasillo / Baliza** — firmware validado en placa (baliza con parpadeo de emergencia) → [`firmware/nodo3_pasillo/`](firmware/nodo3_pasillo/)
- [ ] Demonio central (BeaglePlay, C++17): lógica diferenciada, panel web, 5 LEDs, **Energy Scan** para elegir el canal de la malla
- [ ] Integración de los 3 nodos con la central y calibración de sensores en campo

> "Validado en placa" = el firmware arranca sin errores, levanta la malla Thread (Canal 25),
> expone sus recursos CoAP y ejecuta su lógica de seguridad. Falta conectar los sensores y
> actuadores físicos y calibrar los umbrales a ppm/°C reales.

---

## Documentación

| Documento | Contenido |
|-----------|-----------|
| [`docs/ARQUITECTURA.md`](docs/ARQUITECTURA.md) | Visión general, los 3 nodos, recursos CoAP, escenarios de control |
| [`docs/REQUERIMIENTOS.md`](docs/REQUERIMIENTOS.md) | Requerimientos funcionales (RF) y no funcionales (RNF) |
| [`docs/MATRIZ_DE_VERIFICACION.md`](docs/MATRIZ_DE_VERIFICACION.md) | Plan de pruebas PV-01 a PV-10 |
| [`docs/CANALES.md`](docs/CANALES.md) | Por qué el Canal 25 y la selección automática de canal |
| [`docs/GUIA_COMPILACION.md`](docs/GUIA_COMPILACION.md) | Compilar y flashear los nodos paso a paso (Ubuntu) |

Cada nodo tiene además su propio `README.md` con mapa de pines, tabla de cableado, recursos
CoAP, lógica de seguridad y pruebas aplicables.
