# Componente principal del Nodo 2.
idf_component_register(
    SRCS "nodo2_horno.c"
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
