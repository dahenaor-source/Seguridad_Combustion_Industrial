# Nodo 1 — Tren de Gas y Corte de Suministro (ESP32-C6)

Firmware del primer nodo de campo del sistema de **seguridad diferenciada para recintos
de combustión** (gas natural CH₄ / GLP). Este nodo vigila la **fuga de combustible sin quemar**
y, ante peligro, **corta el suministro de gas** cerrando una electroválvula normalmente cerrada (NC).

> Regla de seguridad clave: este nodo **nunca** enciende extractores. Ante una fuga de gas
> combustible, encender un motor eléctrico no certificado podría provocar una chispa en una
> atmósfera potencialmente explosiva. La acción correcta es **cortar el gas** y **avisar** (la
> baliza la activa el Nodo 3).

---

## Función

- Mide gas combustible (CH₄/GLP) con un sensor **MQ-6** (analógico, por ADC).
- Publica telemetría por **CoAP Observe** en `/env/air` con payload **CBOR < 52 bytes**.
- Expone el recurso de actuación `/act/cutoff` (**PUT CON idempotente**).
- Controla el relé que energiza la **electroválvula NC**:
  - relé energizado (GPIO en **LOW**) → válvula **abierta** → paso de gas (piloto verde);
  - relé apagado (GPIO en **HIGH**) → válvula **cerrada** → corte (piloto rojo).

### Principio fail-safe (normalmente cerrada)
La válvula NC cierra por **resorte mecánico** cuando pierde energía. Por eso, ante fuga,
falla de sonda o corte eléctrico, el firmware **desenergiza el relé** y el gas se corta en < 1 s.
El arranque también es seguro: el nodo inicia con la **válvula cerrada** hasta que la red está lista.

---

## Mapa de pines

| Señal | Pin ESP32-C6 | Notas |
|-------|-------------|-------|
| MQ-6 `AO` (gas combustible) | **GPIO2** (ADC1_CH2) | Salida 5 V pasada por divisor **10 kΩ + 15 kΩ** → ≤ 3.0 V seguros para el ADC de 3.3 V |
| Relé `IN` (electroválvula NC) | **GPIO4** | Módulo RM1-5V-1, **disparo en LOW** (Low Level Trigger) |

Alimentación: 12 VDC regulada localmente a 5 V (L7805). El MQ-6 trabaja a 5 V.

---

## Recursos CoAP

| Recurso | Método | Payload CBOR | Descripción |
|---------|--------|--------------|-------------|
| `/env/air` | GET (+ Observe) | mapa `{t,g,v,f}` | Telemetría: tipo de nodo, índice de gas 0–100, estado de válvula, flag de falla |
| `/act/cutoff` | PUT CON | entero `1`=cerrar, `0`=abrir | Corte/reposición de gas. Idempotente. La reapertura se rechaza (4.03) si hay peligro vigente |

**Telemetría `/env/air` (CBOR, claves de 1 carácter para caber en < 52 B):**
- `t`: tipo de nodo (1 = tren de gas)
- `g`: índice de gas combustible 0–100
- `v`: estado de válvula (0 abierta, 1 cerrada)
- `f`: falla de sonda (0/1)

**Contrato de frescura (RF-06):** `/env/air` declara `Max-Age = 60 s`. Si la sonda se desconecta
o satura, el filtro de plausibilidad lo detecta y la central lo interpreta como `5.03 Service
Unavailable` a los 60 s.

---

## Lógica de seguridad del nodo

La **central (BeaglePlay)** toma la decisión global, pero el nodo también actúa por sí solo si
pierde el enlace (autonomía, RF-07):

- **Gas ≥ umbral de corte** → cierra la válvula (`/act/cutoff`).
- **Sonda en falla** → corte preventivo (estado seguro).
- La **reapertura nunca es automática**: requiere `PUT /act/cutoff` con `v=0` desde la central,
  y solo se acepta si ya no hay condición de peligro.

Umbrales de arranque (se calibran en campo), en `nodo1_tren_gas.c`:

```c
#define UMBRAL_GAS_ALERTA   45.0f   // aviso temprano
#define UMBRAL_GAS_CORTE    60.0f   // corte inmediato (cercanía al LEL)
```

---

## Compilar y flashear

Requiere **ESP-IDF ≥ 5.1** con soporte OpenThread para ESP32-C6.

```bash
cd firmware/nodo1_tren_gas
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

El `sdkconfig.defaults` ya fija: target `esp32c6`, OpenThread FTD, CoAP, radio 802.15.4 nativa,
IPv6 y Canal 25.

---

## Pruebas de la matriz que aplican a este nodo

- **PV-04A (Fuga CH₄):** liberar gas frío de encendedor (sin chispa) ~2 s sobre el MQ-6.
  *Criterio:* en < 2.0 s la válvula cierra (`/act/cutoff`), se activa la baliza (Nodo 3) y el
  extractor permanece **apagado**.
- **PV-05 (Fail-Safe inverso):** desconectar la señal del sensor o apagar el nodo.
  *Criterio:* error `5.03` a los 60 s o timeout a los 93 s → transición a estado seguro (corte).
- **PV-07 (Autonomía):** apagar la central e inyectar gas. *Criterio:* el Nodo 1 envía
  `PUT /act/cutoff` dentro de la malla y cierra la válvula **sin** la central.

---

## Estado

Primer nodo en fase de montaje/soldadura. El firmware está listo para compilar; los umbrales
se ajustarán con la calibración del MQ-6 en banco.
