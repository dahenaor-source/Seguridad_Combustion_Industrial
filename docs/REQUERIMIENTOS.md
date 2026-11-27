# Análisis de Requerimientos — Seguridad de Combustión Industrial

Sistema adaptado a hornos (cerámica/vidrio/metales) y cuartos de calderas a gas natural (CH₄) o GLP.

---

## Requerimientos Funcionales (RF)

| ID | Nombre | Descripción |
|----|--------|-------------|
| **RF-01** | Asistente de Bring-up inicial | Si no existe `/etc/tunnelsafe/config.conf`, la BeaglePlay inicia en modo AP Wi-Fi (`hostapd` + `dnsmasq`) y sirve un portal web de configuración (Wi-Fi local, nombres de zonas y umbrales de CH₄, CO y temperatura) sin consola/SSH. |
| **RF-02** | Restauración de fábrica | Manteniendo el botón USR ≥ 5.0 s (vía `libgpiod`), borra la configuración, ejecuta `ot factoryreset` en el RCP, parpadea LEDs y reinicia en Bring-up. |
| **RF-03** | Comunicación Host-RCP y malla Thread | La BeaglePlay se comunica por UART/USB (Spinel) con el RCP 802.15.4 vía `otbr-agent`, formando malla Thread en Canal 25 a +20 dBm con IPv6/SLAAC y persistencia por ML-EID. |
| **RF-04** | Monitoreo atmosférico por eventos | Los nodos muestrean CH₄/GLP, CO, temperatura, humedad y VOC (`/env/air`), aplican plausibilidad local y notifican por CoAP Observe con CBOR (< 52 bytes). |
| **RF-05** | Control diferenciado e idempotente | Accionar electroválvula NC (`/act/cutoff`), extractores (`/act/extractor`) y balizas (`/act/alarm`) por CoAP PUT CON idempotente, **diferenciando** fuga de combustible (corte + evacuación, sin extractor) de combustión incompleta (extracción al 100%). |
| **RF-06** | Contrato de frescura y fail-safe inverso | `/env/air` incluye Max-Age dinámico (60 s). Ante desconexión de sonda o fallo de plausibilidad (`5.03` a los 60 s) o caída de nodo (timeout a los 93 s), la central pasa a estado seguro (corte de gas / ventilación) y emite alarma de mantenimiento. |
| **RF-07** | Autonomía local sin pasarela | Si la BeaglePlay pierde Wi-Fi o se apaga, los nodos mantienen el enrutamiento interno y ejecutan localmente el cierre de electroválvula o el encendido de extractores ante niveles críticos. |
| **RF-08** | Interfaz web y señalización LED | El demonio C/C++17 sirve una interfaz web ligera (lecturas en tiempo real, estado de nodos, control manual de actuadores, historial) sincronizada con los 5 LEDs físicos. |
| **RF-09** | Instrumentación del sistema | Recolectar y exponer desde `/proc` y `/sys`: uso de CPU, RAM (RSS/VmSize), bytes RX/TX en `wpan0` y `wlan0`, puertos UDP6 activos y estado del descriptor UART del RCP. |
| **RF-10** | Autenticación e integridad de paquetes | Todos los mensajes CoAP (telemetría y actuación) deben estar autenticados/cifrados (DTLS 1.2 o firma HMAC-SHA256) para rechazar paquetes inyectados o suplantados. |

---

## Requerimientos No Funcionales (RNF)

| ID | Categoría | Especificación cuantitativa |
|----|-----------|-----------------------------|
| **RNF-01** | Eficiencia de recursos | Demonio en C/C++17: < 15 MB RAM (RSS), < 10% CPU en estado estable, 0 fugas (Valgrind). |
| **RNF-02** | Desempeño RF y latencia | RSSI > −70 dBm, PER < 1% a ~15 m (8–10 m entre estructuras metálicas); latencia pasarela OTBR < 100 ms. |
| **RNF-03** | Resiliencia / autorrecuperación | Ante caída de un nodo router intermedio, la malla reconverge la ruta en ≤ 8.0 s. |
| **RNF-04** | Usabilidad para no expertos | Bring-up completo (encender → primer nodo en el panel) en < 3 min, sin terminal. |
| **RNF-05** | Integración mecánica | Ensamblaje sobre baquelita FR-4 con borneras de tornillo (sin protoboard), carcasa PETG/ABS con ventilación sobre sensores, acceso al botón USR y LEDs visibles. |

---

## Reglas de seguridad (lógica de control diferenciado)

- **Escenario A — Fuga de CH₄/GLP (Nodo 1):** cerrar `/act/cutoff` (desenergizar relé → la válvula NC cierra por resorte), activar `/act/alarm` (Nodo 3). **NO** encender extractor.
- **Escenario B — CO alto (Nodo 2/3):** activar `/act/extractor` al 100% + `/act/alarm` si el CO supera el umbral crítico.
- **Escenario C — Sobretemperatura (BMP180 del Nodo 2):** alerta preventiva + extracción si supera el umbral térmico.
