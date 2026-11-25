# Nodo 3 — Pasillo de Operarios y Baliza de Evacuación (ESP32-C6)

Tercer nodo de campo. Vigila el **CO en la zona respirable** del pasillo y activa la
**baliza de evacuación** (buzzer + LED de alto brillo). Opera además como **router Thread
de respaldo** de la malla.

> La baliza se activa por **dos vías**: cuando la central lo ordena (`/act/alarm`, por fuga de
> gas en el Nodo 1 o CO alto en el Nodo 2) **o** localmente si el CO del pasillo supera el
> umbral. Si hay CO alto local, la baliza sigue sonando aunque la central ordene apagarla
> (la seguridad del personal manda).

---

## Función

- Mide **CO** en zona respirable con un **MQ-7** (analógico, por ADC).
- Expone el recurso de actuación `/act/alarm` (**PUT CON idempotente**).
- Controla la baliza: **buzzer** + **LED** de alto brillo, con **parpadeo** mientras la alarma
  esté activa.
- Publica telemetría CoAP en `/env/air` (payload **CBOR < 52 bytes**).

---

## Mapa de pines

| Señal | Pin ESP32-C6 | Notas |
|-------|-------------|-------|
| MQ-7 `AO` (CO) | **GPIO3** (ADC1_CH3) | Salida 5 V por divisor **10 kΩ + 15 kΩ** → ≤ 3.0 V para el ADC |
| Buzzer activo | **GPIO10** | Disparo en **HIGH** (HIGH = suena) |
| Baliza LED | **GPIO11** | Disparo en **HIGH**, con resistencia **330 Ω** en serie |

Alimentación: 12 VDC regulada a 5 V. El buzzer suele ser de 5 V; el LED se maneja por GPIO (3.3 V) con su resistencia.

---

## Conexiones (tabla de cableado)

| # | Desde | Hacia | Cable | Notas |
|---|-------|-------|-------|-------|
| 1 | Fuente **+5V** | MQ-7 **VCC** | Rojo | Calefactor interno del MQ-7 |
| 2 | GND común | MQ-7 / ESP32 / buzzer / LED **GND** | Negro | **Tierra común obligatoria** |
| 3 | MQ-7 **AO** | **R1 (10 kΩ)** → nodo central del divisor | Azul | Señal analógica de CO |
| 4 | Nodo central del divisor | ESP32-C6 **GPIO3** | Azul | Tensión reducida a ≤ 3.0 V |
| 5 | **R2 (15 kΩ)** | GND común | Negro | Rama inferior del divisor |
| 6 | ESP32-C6 **GPIO10** | Buzzer **(+)** | Verde | Control del buzzer (HIGH = ON) |
| 7 | Buzzer **(−)** | GND común | Negro | Retorno del buzzer |
| 8 | ESP32-C6 **GPIO11** | **R (330 Ω)** → LED **ánodo (+)** | Verde | Control del LED de la baliza |
| 9 | LED **cátodo (−)** | GND común | Negro | Retorno del LED |

> La resistencia de **330 Ω** en serie con el LED es obligatoria para limitar la corriente y
> no dañar el pin ni el LED.

---

## Recursos CoAP

| Recurso | Método | Payload CBOR | Descripción |
|---------|--------|--------------|-------------|
| `/env/air` | GET | mapa `{t,c,a,f}` | Telemetría: tipo, CO 0–100, estado baliza, falla |
| `/act/alarm` | PUT CON | entero `1`=activar, `0`=desactivar | Control de la baliza desde la central. Idempotente |

**Telemetría `/env/air` (CBOR):**
- `t`: tipo de nodo (3 = pasillo)
- `c`: índice de CO 0–100
- `a`: estado de la baliza (0 off, 1 on)
- `f`: falla de sonda (0/1)

---

## Lógica de la baliza (dos fuentes de alarma)

La baliza se enciende si **cualquiera** de estas dos condiciones es verdadera:

1. **Alarma remota** — la central envía `PUT /act/alarm` con `v=1`.
2. **Alarma local** — el CO del pasillo supera el umbral (autonomía, RF-07).

Cuando la central ordena apagar (`v=0`), solo se cancela la fuente remota: **si todavía hay CO
alto local, la baliza sigue activa**. Esto protege al personal aunque la central se equivoque o
falle.

Umbrales de arranque (se calibran en campo), en `nodo3_pasillo.c`:

```c
#define UMBRAL_CO_ALARMA    45.0f   // dispara baliza local
#define UMBRAL_CO_APAGADO   30.0f   // histéresis de apagado
```

---

## Compilar y flashear

```bash
cd firmware/nodo3_pasillo
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

---

## Pruebas de la matriz que aplican a este nodo

- **PV-04A / PV-04B:** al dispararse el Nodo 1 (fuga CH₄) o el Nodo 2 (CO), la central envía
  `/act/alarm` y esta baliza debe sonar/encender.
- **PV-05 (Fail-Safe inverso):** desconectar la señal del MQ-7 o apagar el nodo → alarma local
  por precaución (o la central lo detecta por timeout).
- **PV-06 (Malla Thread):** este nodo actúa como **router de respaldo**; al caer un nodo
  intermedio, la malla reconverge usándolo.

---

## Notas de puesta en marcha

- **Sin el MQ-7 conectado**, GPIO3 queda flotante y el valor `CO=` del log es solo ruido;
  puede incluso disparar la alarma local de forma espuria (normal sin sensor).
- Puedes probar la baliza **sin sensor** enviando `/act/alarm` desde la central (cuando exista)
  o forzando `s_alarma_local` en código para banco.
- El buzzer activo suena al recibir HIGH; si usas un buzzer **pasivo**, habría que generar una
  frecuencia (PWM) — avísame y lo adapto.

## Estado de validación

- [ ] Compila y arranca sin panic.
- [ ] Radio Thread en Canal 25 + CoAP activo.
- [ ] Lectura del MQ-7 (CO).
- [ ] Baliza: buzzer + LED con parpadeo.
- [ ] Respuesta al recurso `/act/alarm`.
