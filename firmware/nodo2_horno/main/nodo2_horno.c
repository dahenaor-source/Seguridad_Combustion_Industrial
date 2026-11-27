/*
 * ============================================================================
 *  Sistema Autonomo de Monitoreo Atmosferico y Seguridad Diferenciada
 *  Recintos industriales de combustion limpia (Gas Natural CH4 / GLP)
 * ----------------------------------------------------------------------------
 *  NODO 2 - CUERPO DEL HORNO / CHIMENEA Y EXTRACCION
 *  Placa: ESP32-C6  |  Stack: ESP-IDF + OpenThread + CoAP (UDP/IPv6) + CBOR
 * ----------------------------------------------------------------------------
 *  Funcion:
 *    - Mide monoxido de carbono (CO) con un sensor MQ-7 (analogico, ADC).
 *    - Mide temperatura, humedad y presion con un BME680 por bus I2C.
 *    - Publica telemetria por CoAP en  /env/air  (payload CBOR < 52 B).
 *    - Expone el recurso de actuacion   /act/extractor (PUT CON idempotente).
 *    - Controla un rele que enciende el ventilador EXTRACTOR de aire.
 *
 *  LOGICA DE SEGURIDAD (combustion incompleta):
 *    A diferencia del Nodo 1 (gas combustible), aqui el riesgo es CO, un gas
 *    toxico pero NO explosivo en estas concentraciones. Por eso SI se permite
 *    encender el extractor electrico para purgar el aire:
 *      - CO  >= umbral        -> extractor ON al 100%.
 *      - Temp >= umbral        -> extractor ON + alerta de sobretemperatura.
 *    El extractor se apaga automaticamente cuando el aire vuelve a ser seguro.
 *
 *  MAPA DE PINES (segun especificacion del proyecto):
 *    - MQ-7  AO  -> GPIO3  (ADC1_CH3)   [salida 5V por divisor 10k+15k -> <=3.0 V]
 *    - BME680 SDA-> GPIO6  (I2C, 3.3 V)
 *    - BME680 SCL-> GPIO7  (I2C, 3.3 V)
 *    - Rele  IN  -> GPIO5  (disparo en LOW / Low Level Trigger)
 * ============================================================================
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

/* ----- Stack Thread + CoAP (ESP-IDF / OpenThread) ----- */
#include "esp_openthread.h"
#include "esp_openthread_types.h"
#include "esp_openthread_netif_glue.h"
#include "esp_vfs_eventfd.h"
#include "esp_netif.h"
#include "openthread/thread.h"
#include "openthread/coap.h"
#include "openthread/ip6.h"
#include "openthread/instance.h"
#include "openthread/tasklet.h"

/* ----- Serializacion CBOR (componente espressif/cbor = TinyCBOR) ----- */
#include "cbor.h"

/* ----- Driver BME680 de Bosch (componente boschsensortec/bme68x) ----- */
#include "bme68x.h"

static const char *TAG = "nodo2_horno";

/* ----------------------------------------------------------------------------
 *  Macros de configuracion por defecto de OpenThread (ver nota en Nodo 1).
 * ------------------------------------------------------------------------- */
#ifndef ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG
#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG()                    \
    {                                                            \
        .radio_mode = RADIO_MODE_NATIVE,                         \
    }
#endif
#ifndef ESP_OPENTHREAD_DEFAULT_HOST_CONFIG
#define ESP_OPENTHREAD_DEFAULT_HOST_CONFIG()                     \
    {                                                            \
        .host_connection_mode = HOST_CONNECTION_MODE_NONE,       \
    }
#endif
#ifndef ESP_OPENTHREAD_DEFAULT_PORT_CONFIG
#define ESP_OPENTHREAD_DEFAULT_PORT_CONFIG()                     \
    {                                                            \
        .storage_partition_name = "nvs",                         \
        .netif_queue_size = 10,                                  \
        .task_queue_size = 10,                                   \
    }
#endif

/* ============================================================================
 *  CONFIGURACION DE HARDWARE Y UMBRALES
 * ========================================================================== */

/* --- Pines --- */
#define MQ7_ADC_GPIO            3          /* GPIO3 = ADC1_CH3 (AO del MQ-7)      */
#define MQ7_ADC_CHANNEL         ADC_CHANNEL_3
#define RELE_EXTRACTOR_GPIO     5          /* GPIO5 -> IN del modulo rele         */
#define I2C_SDA_GPIO            6          /* BME680 SDA                          */
#define I2C_SCL_GPIO            7          /* BME680 SCL                          */
#define BME680_I2C_ADDR         BME68X_I2C_ADDR_LOW   /* 0x76 (SDO a GND)         */

/* El modulo rele dispara en LOW (Low Level Trigger):
 *   GPIO = 0 (LOW)  -> rele ENERGIZADO  -> extractor ENCENDIDO
 *   GPIO = 1 (HIGH) -> rele APAGADO     -> extractor APAGADO                     */
#define RELE_ON_EXTRACTOR       0
#define RELE_OFF_EXTRACTOR      1

/* --- Umbrales (se calibran en campo).
 *  co_pct: indice de CO normalizado 0..100.
 *  temp:   grados Celsius del BME680.                                           --- */
/* Umbrales calibrados al MQ-7 real del montaje:
 *   aire limpio ~5% (35 mV), con gas sube hasta ~25% (380 mV).                  */
#define UMBRAL_CO_ALERTA        15.0f      /* aviso temprano de CO                */
#define UMBRAL_CO_EXTRACCION    18.0f      /* enciende extractor al 100%          */
#define UMBRAL_CO_APAGADO       12.0f      /* histeresis: por debajo, apaga       */
#define UMBRAL_TEMP_ALERTA      60.0f      /* grados C: sobretemperatura          */

/* --- Telemetria --- */
#define PERIODO_MUESTREO_MS     1000
#define PERIODO_LATIDO_MS       30000
#define MAX_AGE_SEG             60

/* --- Filtro de media movil del MQ-7 --- */
#define VENTANA_FILTRO          8

/* --- Plausibilidad del ADC (calibrado al MQ-7 real) ---
 * Un pico de ruido aislado NO marca falla: se exigen FALLA_LECTURAS_MIN seguidas. */
#define MV_MIN_VALIDO           12         /* reposo del MQ-7 ~35 mV -> valido    */
#define MV_MAX_VALIDO           3250
#define FALLA_LECTURAS_MIN      3

/* ============================================================================
 *  ESTADO GLOBAL DEL NODO
 * ========================================================================== */
typedef enum {
    EXTRACTOR_OFF = 0,
    EXTRACTOR_ON  = 1,
} estado_extractor_t;

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t         s_adc_cali   = NULL;
static bool                      s_adc_cali_ok = false;

static float   s_ventana[VENTANA_FILTRO] = {0};
static int     s_ventana_idx = 0;
static bool    s_ventana_llena = false;

static volatile float  s_co_pct   = 0.0f;   /* indice de CO 0..100         */
static volatile float  s_temp_c   = 0.0f;   /* temperatura BME680 (C)      */
static volatile float  s_hum_pct  = 0.0f;   /* humedad relativa (%)        */
static volatile bool   s_sensor_falla = false;
static volatile bool   s_bme_ok   = false;
static volatile estado_extractor_t s_estado = EXTRACTOR_OFF;
static int             s_cuenta_implausible = 0;  /* lecturas malas seguidas */

static otCoapResource s_recurso_ext;        /* /act/extractor */
static otCoapResource s_recurso_env;        /* /env/air       */

/* --- Handles I2C y BME680 --- */
static i2c_master_bus_handle_t  s_i2c_bus = NULL;
static i2c_master_dev_handle_t  s_bme_dev = NULL;
static struct bme68x_dev        s_bme;
static struct bme68x_conf       s_bme_conf;
static struct bme68x_heatr_conf s_bme_heatr;

/* ============================================================================
 *  CONTROL DEL RELE / EXTRACTOR
 * ========================================================================== */
static void extractor_encender(void)
{
    gpio_set_level(RELE_EXTRACTOR_GPIO, RELE_ON_EXTRACTOR);
    s_estado = EXTRACTOR_ON;
    ESP_LOGW(TAG, "Extractor ENCENDIDO (purga de aire al 100%%)");
}

static void extractor_apagar(void)
{
    gpio_set_level(RELE_EXTRACTOR_GPIO, RELE_OFF_EXTRACTOR);
    s_estado = EXTRACTOR_OFF;
    ESP_LOGI(TAG, "Extractor APAGADO (aire en rango seguro)");
}

static void rele_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << RELE_EXTRACTOR_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    extractor_apagar();   /* arranque: extractor apagado para ahorrar energia */
}

/* ============================================================================
 *  LECTURA DEL SENSOR MQ-7 (ADC + filtro + plausibilidad)
 * ========================================================================== */
static void adc_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, MQ7_ADC_CHANNEL, &chan_cfg));

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .chan     = MQ7_ADC_CHANNEL,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali) == ESP_OK) {
        s_adc_cali_ok = true;
    } else {
        ESP_LOGW(TAG, "Calibracion ADC no disponible; se usa valor crudo");
    }
}

static float filtro_media_movil(float muestra)
{
    s_ventana[s_ventana_idx] = muestra;
    s_ventana_idx = (s_ventana_idx + 1) % VENTANA_FILTRO;
    if (s_ventana_idx == 0) s_ventana_llena = true;
    int n = s_ventana_llena ? VENTANA_FILTRO : s_ventana_idx;
    if (n == 0) return muestra;
    float acc = 0.0f;
    for (int i = 0; i < n; i++) acc += s_ventana[i];
    return acc / n;
}

static float leer_co(void)
{
    int raw = 0;
    if (adc_oneshot_read(s_adc_handle, MQ7_ADC_CHANNEL, &raw) != ESP_OK) {
        s_cuenta_implausible++;
        if (s_cuenta_implausible >= FALLA_LECTURAS_MIN) s_sensor_falla = true;
        return s_co_pct;
    }
    int mv = raw;
    if (s_adc_cali_ok) adc_cali_raw_to_voltage(s_adc_cali, raw, &mv);

    /* Un pico aislado NO marca falla: se exigen FALLA_LECTURAS_MIN seguidas. */
    if (mv < MV_MIN_VALIDO || mv > MV_MAX_VALIDO) {
        s_cuenta_implausible++;
        if (s_cuenta_implausible >= FALLA_LECTURAS_MIN) {
            if (!s_sensor_falla) {
                ESP_LOGW(TAG, "MQ-7 implausible (%d mV) x%d -> sonda desconectada",
                         mv, s_cuenta_implausible);
            }
            s_sensor_falla = true;
        }
        return s_co_pct;   /* mantener ultimo valor valido */
    }

    s_cuenta_implausible = 0;
    s_sensor_falla = false;

    float pct = (mv / 3000.0f) * 100.0f;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return filtro_media_movil(pct);
}

/* ============================================================================
 *  BME680 POR I2C (temperatura / humedad / presion)
 *  Se usa el driver oficial de Bosch (bme68x) con callbacks a I2C master.
 * ========================================================================== */
static BME68X_INTF_RET_TYPE bme_i2c_read(uint8_t reg, uint8_t *data,
                                         uint32_t len, void *intf_ptr)
{
    (void)intf_ptr;
    esp_err_t e = i2c_master_transmit_receive(s_bme_dev, &reg, 1, data, len,
                                              1000 / portTICK_PERIOD_MS);
    return (e == ESP_OK) ? BME68X_OK : BME68X_E_COM_FAIL;
}

static BME68X_INTF_RET_TYPE bme_i2c_write(uint8_t reg, const uint8_t *data,
                                          uint32_t len, void *intf_ptr)
{
    (void)intf_ptr;
    uint8_t buf[32];
    if (len + 1 > sizeof(buf)) return BME68X_E_COM_FAIL;
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    esp_err_t e = i2c_master_transmit(s_bme_dev, buf, len + 1,
                                      1000 / portTICK_PERIOD_MS);
    return (e == ESP_OK) ? BME68X_OK : BME68X_E_COM_FAIL;
}

static void bme_delay_us(uint32_t period, void *intf_ptr)
{
    (void)intf_ptr;
    if (period < 1000) {
        esp_rom_delay_us(period);
    } else {
        vTaskDelay(pdMS_TO_TICKS(period / 1000));
    }
}

/* Inicializa el bus I2C y el BME680. Devuelve true si el sensor respondio. */
static bool bme680_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = I2C_SDA_GPIO,
        .scl_io_num        = I2C_SCL_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_i2c_bus) != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo crear el bus I2C");
        return false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BME680_I2C_ADDR,
        .scl_speed_hz    = 100000,
    };
    if (i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_bme_dev) != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo anadir el BME680 al bus I2C");
        return false;
    }

    /* Enlazar el driver Bosch con nuestros callbacks I2C. */
    s_bme.intf     = BME68X_I2C_INTF;
    s_bme.read     = bme_i2c_read;
    s_bme.write    = bme_i2c_write;
    s_bme.delay_us = bme_delay_us;
    s_bme.amb_temp = 25;
    s_bme.intf_ptr = NULL;

    if (bme68x_init(&s_bme) != BME68X_OK) {
        ESP_LOGW(TAG, "BME680 no responde (revisar cableado I2C)");
        return false;
    }

    /* Configuracion de sobremuestreo (modo forzado, lectura puntual). */
    s_bme_conf.filter  = BME68X_FILTER_OFF;
    s_bme_conf.odr     = BME68X_ODR_NONE;
    s_bme_conf.os_hum  = BME68X_OS_2X;
    s_bme_conf.os_temp = BME68X_OS_4X;
    s_bme_conf.os_pres = BME68X_OS_2X;
    bme68x_set_conf(&s_bme_conf, &s_bme);

    /* Calefactor de gas desactivado: solo usamos temp/hum/presion. */
    s_bme_heatr.enable     = BME68X_DISABLE;
    s_bme_heatr.heatr_temp = 0;
    s_bme_heatr.heatr_dur  = 0;
    bme68x_set_heatr_conf(BME68X_FORCED_MODE, &s_bme_heatr, &s_bme);

    ESP_LOGI(TAG, "BME680 inicializado correctamente");
    return true;
}

static void bme680_leer(void)
{
    if (!s_bme_ok) return;

    if (bme68x_set_op_mode(BME68X_FORCED_MODE, &s_bme) != BME68X_OK) return;

    uint32_t dur = bme68x_get_meas_dur(BME68X_FORCED_MODE, &s_bme_conf, &s_bme);
    s_bme.delay_us(dur, s_bme.intf_ptr);

    struct bme68x_data data;
    uint8_t n = 0;
    if (bme68x_get_data(BME68X_FORCED_MODE, &data, &n, &s_bme) == BME68X_OK && n > 0) {
        s_temp_c  = data.temperature;    /* grados C          */
        s_hum_pct = data.humidity;       /* % humedad relativa */
    }
}

/* ============================================================================
 *  LOGICA LOCAL DE SEGURIDAD (autonomia sin central - RF-07)
 * ========================================================================== */
static void aplicar_logica_local(void)
{
    /* Fail-safe inverso: si la sonda de CO falla, encender extractor por
     * precaucion (en zona de CO ventilar de mas es seguro). */
    if (s_sensor_falla) {
        if (s_estado != EXTRACTOR_ON) {
            ESP_LOGW(TAG, "Sonda CO en falla -> extractor ON (precaucion)");
            extractor_encender();
        }
        return;
    }

    bool co_alto   = (s_co_pct >= UMBRAL_CO_EXTRACCION);
    bool temp_alta = (s_bme_ok && s_temp_c >= UMBRAL_TEMP_ALERTA);

    if (co_alto || temp_alta) {
        if (s_estado != EXTRACTOR_ON) {
            ESP_LOGW(TAG, "CO=%.1f%% temp=%.1fC -> ENCENDER extractor",
                     s_co_pct, s_temp_c);
            extractor_encender();
        }
    } else if (s_co_pct <= UMBRAL_CO_APAGADO &&
               (!s_bme_ok || s_temp_c < UMBRAL_TEMP_ALERTA)) {
        /* Histeresis: solo apaga cuando el CO baja del umbral de apagado. */
        if (s_estado != EXTRACTOR_OFF) {
            extractor_apagar();
        }
    }
}

/* ============================================================================
 *  SERIALIZACION CBOR DE /env/air  (payload < 52 bytes)
 *  Mapa CBOR compacto:
 *    "t": tipo de nodo (2 = horno/chimenea)
 *    "c": indice de CO 0..100 (entero)
 *    "T": temperatura en C (entero)
 *    "h": humedad relativa % (entero)
 *    "x": estado extractor (0 off, 1 on)
 *    "f": flag de falla de sonda (0/1)
 * ========================================================================== */
static int construir_cbor_env(uint8_t *buf, size_t buf_len)
{
    CborEncoder enc, mapa;
    cbor_encoder_init(&enc, buf, buf_len, 0);
    cbor_encoder_create_map(&enc, &mapa, 6);

    cbor_encode_text_stringz(&mapa, "t");
    cbor_encode_uint(&mapa, 2);

    cbor_encode_text_stringz(&mapa, "c");
    cbor_encode_uint(&mapa, (uint64_t)(s_co_pct + 0.5f));

    cbor_encode_text_stringz(&mapa, "T");
    cbor_encode_int(&mapa, (int64_t)(s_temp_c + 0.5f));

    cbor_encode_text_stringz(&mapa, "h");
    cbor_encode_uint(&mapa, (uint64_t)(s_hum_pct + 0.5f));

    cbor_encode_text_stringz(&mapa, "x");
    cbor_encode_uint(&mapa, (uint64_t)s_estado);

    cbor_encode_text_stringz(&mapa, "f");
    cbor_encode_uint(&mapa, s_sensor_falla ? 1 : 0);

    cbor_encoder_close_container(&enc, &mapa);
    return (int)cbor_encoder_get_buffer_size(&enc, buf);
}

/* ============================================================================
 *  RECURSO CoAP  /env/air  (GET -> telemetria CBOR)
 * ========================================================================== */
static void env_air_handler(void *ctx, otMessage *msg, const otMessageInfo *info)
{
    (void)ctx;
    otInstance *ot = esp_openthread_get_instance();
    if (otCoapMessageGetCode(msg) != OT_COAP_CODE_GET) return;

    uint8_t payload[52];
    int len = construir_cbor_env(payload, sizeof(payload));

    otMessage *resp = otCoapNewMessage(ot, NULL);
    if (resp == NULL) return;

    otCoapMessageInitResponse(resp, msg, OT_COAP_TYPE_ACKNOWLEDGMENT, OT_COAP_CODE_CONTENT);
    otCoapMessageSetToken(resp, otCoapMessageGetToken(msg), otCoapMessageGetTokenLength(msg));
    otCoapMessageAppendMaxAgeOption(resp, MAX_AGE_SEG);
    otCoapMessageAppendContentFormatOption(resp, OT_COAP_OPTION_CONTENT_FORMAT_CBOR);
    otCoapMessageSetPayloadMarker(resp);
    otMessageAppend(resp, payload, len);

    if (otCoapSendResponse(ot, resp, info) != OT_ERROR_NONE) {
        otMessageFree(resp);
        ESP_LOGW(TAG, "Error enviando /env/air");
    }
}

/* ============================================================================
 *  RECURSO CoAP  /act/extractor  (PUT CON idempotente)
 *  Payload CBOR: entero  1 = encender , 0 = apagar
 * ========================================================================== */
static void act_extractor_handler(void *ctx, otMessage *msg, const otMessageInfo *info)
{
    (void)ctx;
    otInstance *ot = esp_openthread_get_instance();

    if (otCoapMessageGetCode(msg) != OT_COAP_CODE_PUT) {
        otMessage *r = otCoapNewMessage(ot, NULL);
        if (r) {
            otCoapMessageInitResponse(r, msg, OT_COAP_TYPE_ACKNOWLEDGMENT,
                                      OT_COAP_CODE_METHOD_NOT_ALLOWED);
            if (otCoapSendResponse(ot, r, info) != OT_ERROR_NONE) otMessageFree(r);
        }
        return;
    }

    uint16_t off = otMessageGetOffset(msg);
    uint16_t len = otMessageGetLength(msg) - off;
    uint8_t  payload[16] = {0};
    if (len > sizeof(payload)) len = sizeof(payload);
    otMessageRead(msg, off, payload, len);

    int encender = -1;
    if (len > 0) {
        CborParser p; CborValue v;
        if (cbor_parser_init(payload, len, 0, &p, &v) == CborNoError &&
            cbor_value_is_integer(&v)) {
            int val = 0;
            cbor_value_get_int(&v, &val);
            encender = (val != 0) ? 1 : 0;
        }
    }

    otCoapCode resp_code = OT_COAP_CODE_CHANGED;  /* 2.04 */
    if (encender == 1) {
        extractor_encender();   /* idempotente: ON repetido queda ON */
    } else if (encender == 0) {
        /* No permitir apagar manualmente si aun hay CO/temperatura peligrosa. */
        if (s_sensor_falla || s_co_pct >= UMBRAL_CO_EXTRACCION ||
            (s_bme_ok && s_temp_c >= UMBRAL_TEMP_ALERTA)) {
            resp_code = OT_COAP_CODE_FORBIDDEN;   /* 4.03 */
            ESP_LOGW(TAG, "Rechazado apagar extractor: condicion de riesgo vigente");
        } else {
            extractor_apagar();
        }
    } else {
        resp_code = OT_COAP_CODE_BAD_REQUEST;     /* 4.00 */
    }

    if (otCoapMessageGetType(msg) == OT_COAP_TYPE_CONFIRMABLE) {
        otMessage *resp = otCoapNewMessage(ot, NULL);
        if (resp) {
            otCoapMessageInitResponse(resp, msg, OT_COAP_TYPE_ACKNOWLEDGMENT, resp_code);
            otCoapMessageSetToken(resp, otCoapMessageGetToken(msg),
                                  otCoapMessageGetTokenLength(msg));
            if (otCoapSendResponse(ot, resp, info) != OT_ERROR_NONE) otMessageFree(resp);
        }
    }
}

/* ============================================================================
 *  REGISTRO DE RECURSOS CoAP
 * ========================================================================== */
static void coap_init_recursos(void)
{
    otInstance *ot = esp_openthread_get_instance();
    ESP_ERROR_CHECK(otCoapStart(ot, OT_DEFAULT_COAP_PORT) == OT_ERROR_NONE
                    ? ESP_OK : ESP_FAIL);

    s_recurso_env.mUriPath = "env/air";
    s_recurso_env.mHandler = env_air_handler;
    s_recurso_env.mContext = NULL;
    s_recurso_env.mNext    = NULL;
    otCoapAddResource(ot, &s_recurso_env);

    s_recurso_ext.mUriPath = "act/extractor";
    s_recurso_ext.mHandler = act_extractor_handler;
    s_recurso_ext.mContext = NULL;
    s_recurso_ext.mNext    = NULL;
    otCoapAddResource(ot, &s_recurso_ext);

    ESP_LOGI(TAG, "Recursos CoAP registrados: /env/air  /act/extractor");
}

/* ============================================================================
 *  TAREA DE MUESTREO Y TELEMETRIA
 * ========================================================================== */
static void tarea_sensor(void *arg)
{
    (void)arg;
    TickType_t ultimo_latido = xTaskGetTickCount();
    float co_prev = -1000.0f;

    while (1) {
        s_co_pct = leer_co();
        bme680_leer();
        aplicar_logica_local();

        bool cambio    = (co_prev < -999.0f) || (fabsf(s_co_pct - co_prev) >= 2.0f);
        bool es_latido = (xTaskGetTickCount() - ultimo_latido)
                         >= pdMS_TO_TICKS(PERIODO_LATIDO_MS);

        if (cambio || es_latido || s_sensor_falla) {
            co_prev = s_co_pct;
            if (es_latido) ultimo_latido = xTaskGetTickCount();
            ESP_LOGI(TAG, "CO=%.1f%% temp=%.1fC hum=%.0f%% extractor=%s falla=%d",
                     s_co_pct, s_temp_c, s_hum_pct,
                     s_estado == EXTRACTOR_ON ? "ON" : "OFF", s_sensor_falla);
        }

        vTaskDelay(pdMS_TO_TICKS(PERIODO_MUESTREO_MS));
    }
}

/* ============================================================================
 *  ARRANQUE DE OPENTHREAD (Canal 25, +20 dBm)
 * ========================================================================== */
static void tarea_openthread(void *arg)
{
    (void)arg;

    esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config  = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config  = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };

    esp_vfs_eventfd_config_t eventfd_cfg = { .max_fds = 3 };
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_cfg));

    ESP_ERROR_CHECK(esp_openthread_init(&config));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_OPENTHREAD();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    assert(netif != NULL);
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_openthread_netif_glue_init(&config)));

    otInstance *ot = esp_openthread_get_instance();
    otLinkSetChannel(ot, 25);                 /* Canal 25 (2475 MHz) */
    otPlatRadioSetTransmitPower(ot, 20);      /* +20 dBm             */
    otIp6SetEnabled(ot, true);
    otThreadSetEnabled(ot, true);

    coap_init_recursos();

    esp_openthread_launch_mainloop();

    esp_netif_destroy(netif);
    esp_openthread_netif_glue_deinit();
    esp_openthread_deinit();
    vTaskDelete(NULL);
}

/* ============================================================================
 *  app_main
 * ========================================================================== */
void app_main(void)
{
    ESP_LOGI(TAG, "=== NODO 2 - Cuerpo del Horno / Chimenea y Extraccion ===");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Hardware local: extractor apagado, ADC del MQ-7 y BME680 por I2C. */
    rele_init();
    adc_init();
    s_bme_ok = bme680_init();
    if (!s_bme_ok) {
        ESP_LOGW(TAG, "Continuando sin BME680 (solo CO por MQ-7)");
    }

    xTaskCreate(tarea_openthread, "openthread", 8192, NULL, 5, NULL);
    xTaskCreate(tarea_sensor, "sensor", 4096, NULL, 4, NULL);
}
