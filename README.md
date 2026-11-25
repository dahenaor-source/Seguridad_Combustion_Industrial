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
  nodo3_pasillo/      ← Nodo 3: baliza (pendiente)
docs/                 ← requerimientos y matriz de verificación
```

---

## Estado del proyecto

- [x] **Nodo 1 — Tren de Gas** (firmware ESP-IDF, validado en placa) → [`firmware/nodo1_tren_gas/`](firmware/nodo1_tren_gas/)
- [x] **Nodo 2 — Horno / Chimenea** (firmware ESP-IDF listo para compilar) → [`firmware/nodo2_horno/`](firmware/nodo2_horno/)
- [ ] Nodo 3 — Pasillo / Baliza
- [ ] Demonio central (BeaglePlay, C++17)

Documentación: ver [`docs/REQUERIMIENTOS.md`](docs/REQUERIMIENTOS.md) y
[`docs/MATRIZ_DE_VERIFICACION.md`](docs/MATRIZ_DE_VERIFICACION.md).
