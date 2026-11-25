# Componente principal del Nodo 1.
idf_component_register(
    SRCS "nodo1_tren_gas.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_adc
        nvs_flash
        openthread
        esp_netif
)
