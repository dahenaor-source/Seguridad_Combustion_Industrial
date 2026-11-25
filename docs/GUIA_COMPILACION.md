# Guía de Compilación y Flasheo (Ubuntu)

Pasos para compilar y cargar el firmware de los nodos ESP32-C6 desde cero.

---

## 1. Requisitos: instalar ESP-IDF (una sola vez)

Se usó **ESP-IDF v5.4** (cualquier versión ≥ 5.1 con soporte OpenThread para ESP32-C6 sirve).

```bash
# Dependencias del sistema
sudo apt update
sudo apt install -y git wget flex bison gperf python3 python3-pip python3-venv \
     cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0

# Clonar ESP-IDF
mkdir -p ~/esp && cd ~/esp
git clone -b release/v5.4 --recursive https://github.com/espressif/esp-idf.git

# Instalar el toolchain para ESP32-C6
cd ~/esp/esp-idf
./install.sh esp32c6
```

### Alias recomendado (para no cargar el entorno a mano cada vez)

```bash
echo "alias get_idf='. \$HOME/esp/esp-idf/export.sh'" >> ~/.bashrc
source ~/.bashrc
```

A partir de ahí, en cada terminal nueva basta con escribir `get_idf`.

---

## 2. Clonar el repositorio

```bash
mkdir -p ~/SistemasLinuxEmbebidos/PROYECTO_3_SCI
cd ~/SistemasLinuxEmbebidos/PROYECTO_3_SCI
git clone https://github.com/dahenaor-source/Seguridad_Combustion_Industrial.git
cd Seguridad_Combustion_Industrial
```

Para traer cambios nuevos más adelante: `git pull`.

---

## 3. Compilar y flashear un nodo

El flujo es el mismo para los tres nodos; solo cambia la carpeta.

```bash
# Cargar el entorno (o usar el alias: get_idf)
. ~/esp/esp-idf/export.sh

# Entrar a la carpeta del nodo (ejemplo: Nodo 1)
cd firmware/nodo1_tren_gas      # o nodo2_horno / nodo3_pasillo

# Fijar el chip (solo la primera vez por carpeta)
idf.py set-target esp32c6

# Compilar
idf.py build

# Ver el puerto de la placa conectada
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null

# Flashear y abrir el monitor serie (ajusta el puerto)
idf.py -p /dev/ttyACM0 flash monitor
```

- **Salir del monitor:** `Ctrl + ]`
- **Permisos del puerto** (si da error de acceso): `sudo usermod -aG dialout $USER` y reiniciar sesión.

---

## 4. Consejos y problemas comunes

| Síntoma | Causa / solución |
|---------|------------------|
| `idf.py: command not found` | No cargaste el entorno en esa terminal: `. ~/esp/esp-idf/export.sh` (o `get_idf`) |
| `espressif/tinycbor doesn't match` | Dependencia mal nombrada; el correcto es `espressif/cbor` (ya corregido en el repo) |
| Error de red/panic al arrancar | Falta `esp_netif_init` + `esp_event_loop_create_default` (ya incluidos) |
| Arranque "sucio" (reusa config vieja) | Borrar la flash: `idf.py -p /dev/ttyACM0 erase-flash` y volver a flashear |
| `BME680 no responde` | Normal si el sensor no está conectado; el Nodo 2 continúa solo con CO |
| Lecturas de gas sin sentido | Normal si el sensor MQ no está conectado (pin flotante = ruido) |

---

## 5. Prueba rápida sin sensores

Los tres nodos arrancan y corren aunque **no haya sensores conectados**. En el monitor verás:

- El banner del nodo (`=== NODO X ... ===`).
- Arranque seguro del actuador (válvula cerrada / extractor apagado / baliza apagada).
- `StartingChannel:25` y la malla Thread formándose.
- `Recursos CoAP registrados: ...`.
- Líneas periódicas de telemetría con el valor del sensor (que será ruido sin el MQ conectado).

En el Nodo 3, el ruido del pin flotante suele cruzar el umbral y **disparar la baliza**, lo que
permite ver el LED + buzzer parpadear sin necesidad del sensor.
