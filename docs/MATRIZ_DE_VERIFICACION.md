# Matriz de Verificación — Seguridad de Combustión Industrial

Verificación cruzada entre los 3 integrantes (quien implementa un módulo no lo verifica):

- **I1 — Arquitecto Linux Core:** demonio C++17, Bring-up, Factory Reset, instrumentación `/proc`.
- **I2 — Ingeniero IoT y Seguridad:** firmware ESP32-C6 Thread, CoAP/CBOR, control diferenciado, fail-safe, criptografía.
- **I3 — Interfaz, Hardware y QA:** servidor web, LEDs, ensamblaje FR-4, carcasas, pruebas y video.

---

| ID | Req. | Tipo | Procedimiento | Criterio de aceptación | Impl. | Verif. |
|----|------|------|---------------|------------------------|:-----:|:------:|
| **PV-01** | RF-01, RNF-04 | Integración (Bring-up) | Arrancar sin `config.conf`; conectarse al AP, configurar Wi-Fi y zonas por web, reiniciar. | AP en < 20 s; tras guardar, se conecta al Wi-Fi local y comisiona la malla sin consola. | I1 | I3 |
| **PV-02** | RF-02 | Integración (Factory Reset) | Mantener el botón USR 5 s. | LEDs parpadean 3×; borra `/etc/tunnelsafe/`, el RCP hace `ot factoryreset` y vuelve al modo AP. | I1 | I2 |
| **PV-03** | RF-04, RF-05 | Unitaria (Firmware) | Validar CBOR de `/env/air` y actuadores; idempotencia ante 3 PUT CON duplicados. | Payload CBOR < 52 B; actuadores en estado estable (`2.04 Changed`) sin conmutaciones erráticas. | I2 | I1 |
| **PV-04A** | RF-04, RF-05, RNF-02 | Integración (Fuga CH₄) | Liberar gas frío de encendedor (sin chispa) ~2 s sobre el Nodo 1 (MQ-6). | En < 2.0 s cierra la electroválvula (`/act/cutoff`), activa baliza (`/act/alarm`) y el extractor queda **apagado**. | I2 | I3 |
| **PV-04B** | RF-04, RF-05, RNF-02 | Integración (Fuga CO) | Humo frío de incienso sobre el Nodo 2 (MQ-7). | En < 2.0 s enciende el extractor (`/act/extractor`) al 100% **sin** cerrar la válvula de gas; se apaga al disiparse. | I2 | I3 |
| **PV-05** | RF-06 | Integración (Fail-Safe inverso) | Caso A: desconectar la señal del MQ-7. Caso B: apagar un nodo sensor. | A: error `5.03` a los 60 s. B: timeout a los 93 s. En ambos: estado seguro + alarma de falla. | I2 | I1 |
| **PV-06** | RF-03, RNF-03 | Integración (Malla Thread) | Cadena A (central) – B (router) – C (nodo tren de gas) en Canal 25; apagar B durante transmisión. | La malla reconverge por ruta de respaldo en ≤ 8.0 s manteniendo ML-EID, sin pérdida permanente. | I2 | I3 |
| **PV-07** | RF-07 | Integración (Autonomía) | Desconectar la central (OTBR) e inyectar gas frío en el Nodo 1. | El Nodo 1 envía `PUT /act/cutoff` y `/act/alarm` dentro de la malla y cierra la válvula sin central. | I2 | I1 |
| **PV-08** | RF-08, RNF-05 | Unitaria/Sistema (UI y chasis) | Verificar sincronía entre los 5 LEDs físicos y los estados web; validar ensamblaje FR-4 y carcasa. | Latencia visual web < 100 ms; LEDs reflejan el estado real; botón USR accesible y prensaestopas firmes. | I3 | I1 |
| **PV-09** | RF-09, RNF-01 | Unitaria/Sistema (Instrumentación) | Ejecutar el demonio C++ bajo carga CoAP 30 min monitoreando fugas de memoria y contadores de red. | RAM RSS < 15 MB, CPU < 10%, 0 bytes perdidos (Valgrind); contadores RX/TX de `wpan0`/`wlan0` coinciden. | I1 | I3 |
| **PV-10** | RF-10 | Integración (Seguridad) | Enviar `PUT /act/cutoff` o `/act/extractor` desde un cliente no autorizado sin clave PSK/HMAC válida. | El nodo y la central rechazan el paquete no autenticado y registran el intento de intrusión. | I2 | I3 |
