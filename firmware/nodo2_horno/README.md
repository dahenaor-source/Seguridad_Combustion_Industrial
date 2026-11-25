# Nodo 2 — Cuerpo del Horno / Chimenea y Extracción (ESP32-C6)

Segundo nodo de campo del sistema de **seguridad diferenciada para recintos de combustión**.
Este nodo vigila la **combustión incompleta** (monóxido de carbono, CO) y la **temperatura**
del horno, y ante peligro **enciende la extracción forzada** de aire.

> Diferencia clave con el Nodo 1: aquí el riesgo es **CO**, un gas tóxico pero **no explosivo**
> en estas concentraciones. Por eso este nodo **sí** enciende un motor eléctrico (el extractor)
> para purgar el aire. En el Nodo 1 (gas combustible) eso estaría prohibido por riesgo de chispa.

---

## Función

- Mide **CO** con un sensor **MQ-7** (analógico, por ADC).
- Mide **temperatura, humedad y presión** con un **BME680** por bus **I²C**.
- Publica telemetría por CoAP en `/env/air` (payload **CBOR < 52 bytes**).
- Expone el recurso de actuación `/act/extractor` (**PUT CON idempotente**).
- Controla el relé del **ventilador extractor**:
  - relé energizado (GPIO en **LOW**) → extractor **encendido**;
  - relé apagado (GPIO en **HIGH**) → extractor **apagado**.

---

## Mapa de pines

| Señal | Pin ESP32-C6 | Notas |
|-------|-------------|-------|
| MQ-7 `AO` (CO) | **GPIO3** (ADC1_CH3) | Salida 5 V por divisor **10 kΩ + 15 kΩ** → ≤ 3.0 V para el ADC |
| BME680 `SDA` | **GPIO6** | Bus I²C, alimentado a **3.3 V** (no 5 V) |
| BME680 `SCL` | **GPIO7** | Bus I²C, 3.3 V. Dirección I²C `0x76` (SDO a GND) |
| Relé `IN` (extractor) | **GPIO5** | Módulo relé, **disparo en LOW** |

Alimentación: 12 VDC regulada a 5 V (MQ-7 y relé) y a 3.3 V (BME680).

---

## Conexiones (tabla de cableado)

| # | Desde | Hacia | Cable | Notas |
|---|-------|-------|-------|-------|
| 1 | Fuente **+5V** | MQ-7 **VCC** | Rojo | Calefactor interno del MQ-7 |
| 2 | Fuente **+5V** | Relé **VCC** | Rojo | Módulo relé de 5 V |
| 3 | ESP32-C6 **3V3** | BME680 **VCC** | Rojo | El BME680 es de **3.3 V** |
| 4 | GND común | MQ-7 / BME680 / ESP32 / relé / extractor **GND** | Negro | **Tierra común obligatoria** |
| 5 | MQ-7 **AO** | **R1 (10 kΩ)** → nodo central del divisor | Azul | Señal analógica de CO |
| 6 | Nodo central del divisor | ESP32-C6 **GPIO3** | Azul | Tensión reducida a ≤ 3.0 V |
| 7 | **R2 (15 kΩ)** | GND común | Negro | Rama inferior del divisor |
| 8 | BME680 **SDA** | ESP32-C6 **GPIO6** | Verde | Dato I²C |
| 9 | BME680 **SCL** | ESP32-C6 **GPIO7** | Verde | Reloj I²C |
| 10 | ESP32-C6 **GPIO5** | Relé **IN** | Verde | Control del extractor (LOW = ON) |
| 11 | Fuente **+12V** | Relé **COM** | Rojo | Alimenta el motor del extractor |
| 12 | Relé **NA** | Extractor **(+)** | Rojo | Al cerrar el relé, energiza el extractor |
| 13 | Extractor **(−)** | GND común | Negro | Retorno del extractor |

> ⚠️ El BME680 va a **3.3 V**, no a 5 V. El MQ-7 sí a 5 V (pero su salida pasa por el divisor
> antes del ADC). Todos los GND deben estar unidos.

### Divisor de tensión (MQ-7)

```
Vout = Vin × R2 / (R1 + R2) = 5.0 × 15 / (10 + 15) = 3.0 V
```
- R1 = 10 kΩ en serie (AO → nodo), R2 = 15 kΩ (nodo → GND), nodo central → GPIO3.

---

## Recursos CoAP

| Recurso | Método | Payload CBOR | Descripción |
|---------|--------|--------------|-------------|
| `/env/air` | GET | mapa `{t,c,T,h,x,f}` | Telemetría: tipo, CO 0–100, temperatura, humedad, estado extractor, falla |
| `/act/extractor` | PUT CON | entero `1`=encender, `0`=apagar | Control del extractor. Idempotente. Apagar se rechaza (4.03) si hay riesgo vigente |

**Telemetría `/env/air` (CBOR):**
- `t`: tipo de nodo (2 = horno/chimenea)
- `c`: índice de CO 0–100
- `T`: temperatura (°C)
- `h`: humedad relativa (%)
- `x`: estado extractor (0 off, 1 on)
- `f`: falla de sonda (0/1)

---

## Lógica de seguridad del nodo

La central decide globalmente, pero el nodo actúa solo si pierde el enlace (RF-07):

- **CO ≥ umbral de extracción** → enciende el extractor.
- **Temperatura ≥ umbral** → enciende el extractor + alerta de sobretemperatura.
- **Sonda de CO en falla** → enciende el extractor por precaución (ventilar de más es seguro).
- **Histéresis:** el extractor se apaga solo cuando el CO baja del *umbral de apagado* y la
  temperatura es segura, para evitar encendidos/apagados erráticos.

Umbrales de arranque (se calibran en campo), en `nodo2_horno.c`:

```c
#define UMBRAL_CO_ALERTA       35.0f   // aviso temprano
#define UMBRAL_CO_EXTRACCION   50.0f   // enciende extractor
#define UMBRAL_CO_APAGADO      30.0f   // histéresis de apagado
#define UMBRAL_TEMP_ALERTA     60.0f   // °C: sobretemperatura
```

---

## Compilar y flashear

```bash
cd firmware/nodo2_horno
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

El driver del BME680 (Bosch, BSD-3-Clause) viene incluido como componente local en
`components/bme68x/`, así que no requiere descargar dependencias externas aparte de CBOR.

---

## Pruebas de la matriz que aplican a este nodo

- **PV-04B (Fuga CO):** introducir humo frío de incienso sobre el MQ-7.
  *Criterio:* en < 2.0 s enciende el extractor (`/act/extractor`) al 100% **sin** cerrar la
  válvula de gas; se apaga al disiparse.
- **PV-05 (Fail-Safe inverso):** desconectar la señal del MQ-7 o apagar el nodo.
  *Criterio:* error `5.03` a los 60 s o timeout a los 93 s → estado seguro + alarma.
- **PV-03 (CBOR + idempotencia):** validar el payload `/env/air` y 3 PUT CON duplicados.

---

## Notas de puesta en marcha

- **Sin el MQ-7 conectado**, GPIO3 queda flotante y el valor `CO=` del log es solo ruido.
- **Sin el BME680 conectado**, el nodo sigue funcionando con solo CO; en el log verás
  `Continuando sin BME680` y `temp=0.0C`.
- El MQ-7 requiere precalentamiento antes de dar lecturas estables.

## Estado de validación

- [ ] Compila y arranca sin panic.
- [ ] Radio Thread en Canal 25 + CoAP activo.
- [ ] Lectura del MQ-7 (CO).
- [ ] Lectura del BME680 (temp/humedad) por I²C.
- [ ] Relé + extractor (encendido/apagado por umbral e histéresis).
- [ ] Calibración de umbrales a ppm / °C reales.
