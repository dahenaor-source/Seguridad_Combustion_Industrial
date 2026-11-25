# Arquitectura del Sistema

Sistema autónomo de monitoreo atmosférico y **seguridad diferenciada** para recintos
industriales de combustión limpia (gas natural CH₄ / GLP): salas de hornos (cerámica, vidrio,
metales) y cuartos de calderas.

---

## 1. Visión general

Una red inalámbrica en malla vigila la atmósfera del recinto y reacciona **según el tipo de
riesgo detectado**, en vez de aplicar una sola respuesta uniforme:

```
   [ Supervisor ]
     (navegador)
        │ Wi-Fi
        ▼
   ┌─────────────────────────┐        UART / Spinel
   │  UNIDAD CENTRAL          │◄────────────────────────┐
   │  BeaglePlay (Linux)      │                          │
   │  - Border Router Thread  │                     ┌────┴────┐
   │  - Lógica diferenciada   │                     │   RCP   │ (radio 802.15.4)
   │  - Panel web + 5 LEDs    │                     └────┬────┘
   └─────────────────────────┘                          │ malla Thread (Canal dinámico)
                                   ┌──────────────┬──────┴───────┬──────────────┐
                                   ▼              ▼              ▼              ▼
                              [ Nodo 1 ]     [ Nodo 2 ]     [ Nodo 3 ]      (routers
                              Tren de gas    Horno/Chim.    Pasillo         de respaldo)
                              MQ-6 + válvula MQ-7+BME680+   MQ-7 + baliza
                                             extractor
```

- **Transporte:** CoAP sobre UDP/IPv6, con payloads binarios **CBOR (< 52 bytes)**.
- **Red:** malla **Thread (IEEE 802.15.4)**, autorrecuperable. El canal lo selecciona la
  BeaglePlay (ver [`CANALES.md`](CANALES.md)); durante el desarrollo los nodos usan el Canal 25.
- **Autonomía:** si la central se apaga, los nodos siguen comunicándose entre sí y actuando
  localmente (RF-07).

---

## 2. La regla de oro: mitigación diferenciada

El sistema distingue **dos riesgos opuestos** que exigen **acciones contrarias**:

| Riesgo | Gas | Acción | ¿Enciende extractor? |
|--------|-----|--------|----------------------|
| **Combustión incompleta** | CO (tóxico, no explosivo) | Extracción forzada al 100% + alarma | **Sí** |
| **Fuga de combustible** | CH₄ / GLP (explosivo) | **Cortar el gas** (válvula NC) + evacuación | **No** (evita chispa) |

> Encender un motor eléctrico ante una fuga de gas combustible podría provocar una chispa en
> una atmósfera explosiva. Por eso el Nodo 1 **nunca** enciende extractores: corta el gas y avisa.

**Fail-safe inverso:** si un nodo pierde comunicación o una sonda falla, el sistema pasa a
**estado seguro** (corte de gas o extracción según la zona) en lugar de quedarse esperando.

---

## 3. Los tres nodos de campo

Todos son **ESP32-C6** con el mismo stack (OpenThread + CoAP + CBOR) y difieren en sensores,
actuadores y lógica.

### Nodo 1 — Tren de Gas ([firmware](../firmware/nodo1_tren_gas/))
- **Sensor:** MQ-6 (CH₄/GLP) en GPIO2.
- **Actuador:** relé → electroválvula **NC** en GPIO4.
- **Recurso:** `/act/cutoff`.
- **Acción:** ante fuga, **desenergiza el relé** y el resorte de la válvula NC corta el gas en < 1 s.

### Nodo 2 — Horno / Chimenea ([firmware](../firmware/nodo2_horno/))
- **Sensores:** MQ-7 (CO) en GPIO3 + **BME680** (temp/humedad) por I²C (GPIO6/GPIO7).
- **Actuador:** relé → extractor en GPIO5.
- **Recurso:** `/act/extractor`.
- **Acción:** ante CO alto o sobretemperatura, enciende el extractor (con histéresis).

### Nodo 3 — Pasillo / Baliza ([firmware](../firmware/nodo3_pasillo/))
- **Sensor:** MQ-7 (CO zona respirable) en GPIO3.
- **Actuadores:** buzzer en GPIO10 + baliza LED en GPIO11 (parpadeo de emergencia).
- **Recurso:** `/act/alarm`.
- **Acción:** activa la baliza por orden de la central **o** por CO local. Opera además como
  router Thread de respaldo.

---

## 4. Recursos CoAP del sistema

| Recurso | Nodo | Método | Descripción |
|---------|------|--------|-------------|
| `/env/air` | todos | GET | Telemetría del nodo (CBOR) |
| `/act/cutoff` | Nodo 1 | PUT CON | Corte / reposición de gas |
| `/act/extractor` | Nodo 2 | PUT CON | Encendido / apagado del extractor |
| `/act/alarm` | Nodo 3 | PUT CON | Activación / desactivación de la baliza |

Todos los PUT son **idempotentes**: repetir la misma orden deja el mismo estado, sin
conmutaciones erráticas (importante en radio, donde los mensajes se reenvían).

### Formato CBOR de `/env/air` por nodo

| Nodo | Claves | Significado |
|------|--------|-------------|
| 1 | `{t,g,v,f}` | tipo, gas 0–100, válvula (0/1), falla |
| 2 | `{t,c,T,h,x,f}` | tipo, CO 0–100, temp °C, humedad %, extractor (0/1), falla |
| 3 | `{t,c,a,f}` | tipo, CO 0–100, baliza (0/1), falla |

---

## 5. Escenarios de control (resumen)

- **Escenario A — Fuga CH₄/GLP (Nodo 1):** cerrar `/act/cutoff` + activar `/act/alarm`. **NO** extractor.
- **Escenario B — CO alto (Nodo 2/3):** activar `/act/extractor` al 100% + `/act/alarm` si supera el umbral crítico.
- **Escenario C — Sobretemperatura (BME680 del Nodo 2):** alerta + extracción.
- **Fail-Safe inverso:** sonda caída (`5.03` a 60 s) o nodo muerto (timeout 93 s) → estado seguro.

Ver el detalle de requisitos en [`REQUERIMIENTOS.md`](REQUERIMIENTOS.md) y las pruebas en
[`MATRIZ_DE_VERIFICACION.md`](MATRIZ_DE_VERIFICACION.md).
