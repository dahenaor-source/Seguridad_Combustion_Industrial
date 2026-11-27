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
 *    - Mide temperatura con un BMP180 (GY-68) por bus I2C (no mide humedad).
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
 *    - MQ-7   AO  -> GPIO3  (ADC1_CH3)   [salida 5V por divisor 10k+15k -> <=3.0 V]
 *    - BMP180 SDA -> GPIO6  (I2C, 3.3 V)
 *    - BMP180 SCL -> GPIO7  (I2C, 3.3 V)
 *    - Rele   IN  -> GPIO5  (disparo en LOW / Low Level Trigger)
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

/* NOTA: el sensor ambiental de este montaje es un BMP180 (GY-68), que mide
 * temperatura y presion (no humedad ni VOC). El driver se implementa abajo
 * directamente por I2C, sin componentes externos. */

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
#define I2C_SDA_GPIO            6          /* BMP180 (GY-68) SDA                  */
#define I2C_SCL_GPIO            7          /* BMP180 (GY-68) SCL                  */
#define BMP180_I2C_ADDR         0x77       /* direccion fija del BMP180           */

/* El modulo rele dispara en LOW (Low Level Trigger):
 *   GPIO = 0 (LOW)  -> rele ENERGIZADO  -> extractor ENCENDIDO
 *   GPIO = 1 (HIGH) -> rele APAGADO     -> extractor APAGADO                     */
#define RELE_ON_EXTRACTOR       0
#define RELE_OFF_EXTRACTOR      1

/* --- Umbrales (se calibran en campo).
 *  co_pct: indice de CO normalizado 0..100.
 *  temp:   grados Celsius del BMP180.                                           --- */
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
static volatile float  s_temp_c   = 0.0f;   /* temperatura BMP180 (C)      */
static volatile float  s_hum_pct  = 0.0f;   /* humedad relativa (%)        */
static volatile bool   s_sensor_falla = false;
static volatile bool   s_bme_ok   = false;
static volatile estado_extractor_t s_estado = EXTRACTOR_OFF;
static int             s_cuenta_implausible = 0;  /* lecturas malas seguidas */

static otCoapResource s_recurso_ext;        /* /act/extractor */
static otCoapResource s_recurso_env;        /* /env/air       */

/* --- Handles I2C y coeficientes de calibracion del BMP180 ---
 * El BMP180 trae 11 coeficientes de fabrica (EEPROM) que hay que leer una vez
 * y usar en la formula de compensacion de temperatura. */
static i2c_master_bus_handle_t  s_i2c_bus = NULL;
static i2c_master_dev_handle_t  s_bmp_dev = NULL;
static struct {
    int16_t  AC1, AC2, AC3, B1, B2, MB, MC, MD;
    uint16_t AC4, AC5, AC6;
} s_cal;

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
 *  BMP180 (GY-68) POR I2C (solo temperatura; tambien mide presion)
 *  Driver escrito a mano por I2C master, sin componentes externos. El BMP180
 *  NO mide humedad, por eso s_hum_pct se mantiene en 0.
 * ========================================================================== */
/* Oversampling del BMP180 (0=menos preciso/rapido .. 3=mas preciso/lento). */
#define BMP180_OSS              1

/* --- Helpers I2C de bajo nivel para el BMP180 --- */
static esp_err_t bmp_write8(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_bmp_dev, buf, 2, 1000 / portTICK_PERIOD_MS);
}

static esp_err_t bmp_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_bmp_dev, &reg, 1, data, len,
                                       1000 / portTICK_PERIOD_MS);
}

/* Lee un registro de 16 bits con signo (big-endian) del BMP180. */
static int16_t bmp_read_s16(uint8_t reg)
{
    uint8_t d[2] = {0};
    bmp_read(reg, d, 2);
    return (int16_t)((d[0] << 8) | d[1]);
}

static uint16_t bmp_read_u16(uint8_t reg)
{
    uint8_t d[2] = {0};
    bmp_read(reg, d, 2);
    return (uint16_t)((d[0] << 8) | d[1]);
}

/* Inicializa el bus I2C y el BMP180. Devuelve true si el sensor respondio.
 * El BMP180 se identifica porque el registro 0xD0 (chip id) vale 0x55. */
static bool bmp180_init(void)
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
        .device_address  = BMP180_I2C_ADDR,
        .scl_speed_hz    = 100000,
    };
    if (i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_bmp_dev) != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo anadir el BMP180 al bus I2C");
        return false;
    }

    /* Verificar chip id (debe ser 0x55 en el BMP180). */
    uint8_t chip_id = 0;
    if (bmp_read(0xD0, &chip_id, 1) != ESP_OK || chip_id != 0x55) {
        ESP_LOGW(TAG, "BMP180 no responde (chip_id=0x%02X, esperado 0x55) - revisar I2C",
                 chip_id);
        return false;
    }

    /* Leer los 11 coeficientes de calibracion de fabrica (registros 0xAA..0xBF). */
    s_cal.AC1 = bmp_read_s16(0xAA);
    s_cal.AC2 = bmp_read_s16(0xAC);
    s_cal.AC3 = bmp_read_s16(0xAE);
    s_cal.AC4 = bmp_read_u16(0xB0);
    s_cal.AC5 = bmp_read_u16(0xB2);
    s_cal.AC6 = bmp_read_u16(0xB4);
    s_cal.B1  = bmp_read_s16(0xB6);
    s_cal.B2  = bmp_read_s16(0xB8);
    s_cal.MB  = bmp_read_s16(0xBA);
    s_cal.MC  = bmp_read_s16(0xBC);
    s_cal.MD  = bmp_read_s16(0xBE);

    ESP_LOGI(TAG, "BMP180 (GY-68) inicializado correctamente");
    return true;
}

/* Lee la temperatura del BMP180 aplicando la formula de compensacion oficial.
 * El BMP180 no mide humedad, por eso s_hum_pct queda en 0. */
static void bmp180_leer(void)
{
    if (!s_bme_ok) return;

    /* 1. Pedir medicion de temperatura: escribir 0x2E en el registro 0xF4. */
    if (bmp_write8(0xF4, 0x2E) != ESP_OK) return;
    vTaskDelay(pdMS_TO_TICKS(5));        /* conversion: 4.5 ms */

    int32_t UT = bmp_read_u16(0xF6);     /* temperatura sin compensar */

    /* 2. Formula de compensacion de temperatura (datasheet BMP180). */
    int32_t X1 = ((UT - (int32_t)s_cal.AC6) * (int32_t)s_cal.AC5) >> 15;
    int32_t X2 = ((int32_t)s_cal.MC << 11) / (X1 + (int32_t)s_cal.MD);
    int32_t B5 = X1 + X2;
    float temp = ((B5 + 8) >> 4) / 10.0f;   /* en grados Celsius */

    s_temp_c  = temp;
    s_hum_pct = 0.0f;   /* el BMP180 no mide humedad */
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
        bmp180_leer();
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

    /* Hardware local: extractor apagado, ADC del MQ-7 y BMP180 por I2C. */
    rele_init();
    adc_init();
    s_bme_ok = bmp180_init();
    if (!s_bme_ok) {
        ESP_LOGW(TAG, "Continuando sin BMP180 (solo CO por MQ-7)");
    }

    xTaskCreate(tarea_openthread, "openthread", 8192, NULL, 5, NULL);
    xTaskCreate(tarea_sensor, "sensor", 4096, NULL, 4, NULL);
}
