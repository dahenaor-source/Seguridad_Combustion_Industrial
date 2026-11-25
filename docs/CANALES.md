# Selección de Canal de la Malla Thread

## ¿Por qué el Canal 25?

Thread opera sobre **IEEE 802.15.4 en la banda de 2.4 GHz**, la misma que usa el Wi-Fi. Como
ambos comparten espectro, se pueden interferir. Los canales 802.15.4 **15, 20, 25 y 26** caen
en los huecos entre los canales Wi-Fi típicos (1, 6, 11); el **Canal 25 (2475 MHz)** queda en
una banda de guarda con poca interferencia Wi-Fi.

Por eso, durante el desarrollo, los tres nodos arrancan fijando el Canal 25
(`otLinkSetChannel(ot, 25)`). Es una elección **estática y conservadora** que funciona bien en
la mayoría de entornos.

---

## Mejora planificada: selección automática de canal (Energy Scan en la BeaglePlay)

En vez de fijar siempre el Canal 25, el sistema puede **elegir automáticamente el mejor canal**
según la interferencia real del lugar. La decisión de diseño adoptada es:

> **La BeaglePlay (Border Router) se encarga de buscar el mejor canal** y forma la red Thread en
> él. Los nodos ESP32-C6 **no eligen canal por su cuenta**: se unen a la red que la central creó.

### ¿Por qué lo decide la central y no cada nodo?

En una malla, **todos los dispositivos deben estar en el mismo canal** para comunicarse. Si cada
ESP32 escogiera un canal distinto, quedarían aislados y no formarían red. Por eso **un solo
dispositivo decide** —la BeaglePlay, que es quien crea la red— y los demás se unen por
descubrimiento (*commissioning* de Thread).

### Cómo funcionará (Energy Scan)

1. Al arrancar, antes de formar la red, la BeaglePlay ejecuta un **Energy Scan**: mide el nivel
   de energía/ruido RF en cada canal del rango 802.15.4 (11–26).
2. Elige el canal con **menor energía** (menos interferencia).
3. Forma la red Thread en ese canal y publica el *dataset* de red.
4. Los nodos ESP32-C6 se comisionan a esa red (sin canal fijo en su código) y adoptan el canal
   elegido por la central.

En OpenThread esto se apoya en las APIs:
- `otLinkEnergyScan()` — barrido de energía por canal (el que usa la central para decidir).
- `otLinkActiveScan()` — descubrimiento de redes existentes (el que usan los nodos para unirse).

### Estado

- **Ahora:** canal fijo (25) en los tres nodos, para desarrollo y pruebas sin la central.
- **Planificado:** Energy Scan en el demonio de la BeaglePlay; los nodos pasarán a comisionarse
  a la red de la central en lugar de fijar el canal. Esto se implementará junto con el demonio
  central.

> Nota para la sustentación: la pregunta "¿pueden elegir el mejor canal?" se responde **sí**, con
> el matiz clave de que el canal lo decide **un** dispositivo (la central), no cada nodo por
> separado, porque la malla exige que todos coincidan en el mismo canal.
