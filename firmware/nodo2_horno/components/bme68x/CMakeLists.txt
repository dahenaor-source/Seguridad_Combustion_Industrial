# Componente local: driver oficial BME68x de Bosch Sensortec (BSD-3-Clause).
# Fuente: https://github.com/boschsensortec/BME68x_SensorAPI
idf_component_register(
    SRCS "bme68x.c"
    INCLUDE_DIRS "."
)
