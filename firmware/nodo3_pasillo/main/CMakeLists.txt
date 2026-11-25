# Componente principal del Nodo 3.
idf_component_register(
    SRCS "nodo3_pasillo.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_adc
        nvs_flash
        openthread
        esp_netif
        esp_event
        vfs
)
