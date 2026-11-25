/*
 * ============================================================================
 *  Sistema Autonomo de Monitoreo Atmosferico y Seguridad Diferenciada
 *  Recintos industriales de combustion limpia (Gas Natural CH4 / GLP)
 * ----------------------------------------------------------------------------
 *  NODO 3 - PASILLO DE OPERARIOS Y BALIZA DE EVACUACION
 *  Placa: ESP32-C6  |  Stack: ESP-IDF + OpenThread + CoAP (UDP/IPv6) + CBOR
 * ----------------------------------------------------------------------------
 *  Funcion:
 *    - Mide CO en la zona respirable del pasillo con un MQ-7 (analogico, ADC).
 *    - Expone el recurso de actuacion  /act/alarm (PUT CON idempotente).
 *    - Controla la baliza de evacuacion: buzzer activo + LED de alto brillo.
 *    - Opera ademas como Router Thread de respaldo de la malla.
 *
 *  La baliza se activa cuando:
 *    - La central ordena /act/alarm (por fuga de gas en Nodo 1 o CO en Nodo 2).
 *    - O localmente, si el CO en el pasillo supera el umbral critico.
 *
 *  MAPA DE PINES (segun especificacion del proyecto):
 *    - MQ-7  AO  -> GPIO3  (ADC1_CH3)   [salida 5V por divisor 10k+15k -> <=3.0V]
 *    - Buzzer    -> GPIO10 (disparo en HIGH)
 *    - Baliza LED-> GPIO11 (disparo en HIGH, con resistencia 330 ohm)
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

static const char *TAG = "nodo3_pasillo";

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
#define BUZZER_GPIO             10         /* GPIO10 -> buzzer activo (HIGH = ON) */
#define BALIZA_LED_GPIO         11         /* GPIO11 -> LED baliza  (HIGH = ON)   */

#define ACT_ON                  1          /* nivel para activar (HIGH)           */
#define ACT_OFF                 0          /* nivel para desactivar (LOW)         */

/* --- Umbrales de CO en el pasillo (zona respirable).
 *  co_pct: indice normalizado 0..100. Se calibra en campo.                      --- */
#define UMBRAL_CO_ALARMA        45.0f      /* dispara baliza local                */
#define UMBRAL_CO_APAGADO       30.0f      /* histeresis de apagado               */

/* --- Telemetria --- */
#define PERIODO_MUESTREO_MS     1000
#define PERIODO_LATIDO_MS       30000
#define MAX_AGE_SEG             60
#define VENTANA_FILTRO          8

/* --- Patron de parpadeo de la baliza (ms) --- */
#define BALIZA_PARPADEO_MS      400

/* ============================================================================
 *  ESTADO GLOBAL
 * ========================================================================== */
typedef enum {
    ALARMA_OFF = 0,
    ALARMA_ON  = 1,
} estado_alarma_t;

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t         s_adc_cali   = NULL;
static bool                      s_adc_cali_ok = false;

static float   s_ventana[VENTANA_FILTRO] = {0};
static int     s_ventana_idx = 0;
static bool    s_ventana_llena = false;

static volatile float s_co_pct = 0.0f;
static volatile bool  s_sensor_falla = false;
static volatile estado_alarma_t s_estado = ALARMA_OFF;

/* La alarma puede venir de la central (remota) o de la deteccion local.
 * La baliza se enciende si CUALQUIERA de las dos esta activa. */
static volatile bool s_alarma_remota = false;   /* ordenada por la central      */
static volatile bool s_alarma_local  = false;   /* por CO alto en este pasillo  */

static otCoapResource s_recurso_alarm;   /* /act/alarm */
static otCoapResource s_recurso_env;     /* /env/air   */

/* ============================================================================
 *  CONTROL DE LA BALIZA (buzzer + LED)
 * ========================================================================== */
static void baliza_salida(bool encendida)
{
    gpio_set_level(BUZZER_GPIO,     encendida ? ACT_ON : ACT_OFF);
    gpio_set_level(BALIZA_LED_GPIO, encendida ? ACT_ON : ACT_OFF);
}

static void actuadores_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BUZZER_GPIO) | (1ULL << BALIZA_LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    baliza_salida(false);   /* arranque: baliza apagada */
}

/* Recalcula el estado de la alarma a partir de las dos fuentes. */
static void actualizar_estado_alarma(void)
{
    estado_alarma_t nuevo = (s_alarma_remota || s_alarma_local) ? ALARMA_ON : ALARMA_OFF;
    if (nuevo != s_estado) {
        s_estado = nuevo;
        if (nuevo == ALARMA_ON) {
            ESP_LOGW(TAG, "BALIZA ACTIVADA (remota=%d local=%d)",
                     s_alarma_remota, s_alarma_local);
        } else {
            ESP_LOGI(TAG, "Baliza apagada (sin condicion de alarma)");
            baliza_salida(false);
        }
    }
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
        s_sensor_falla = true;
        return s_co_pct;
    }
    int mv = raw;
    if (s_adc_cali_ok) adc_cali_raw_to_voltage(s_adc_cali, raw, &mv);

    if (mv < 50 || mv > 3150) {
        s_sensor_falla = true;
        ESP_LOGW(TAG, "MQ-7 implausible (%d mV) -> posible sonda desconectada", mv);
        return s_co_pct;
    }
    s_sensor_falla = false;

    float pct = (mv / 3000.0f) * 100.0f;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return filtro_media_movil(pct);
}

/* ============================================================================
 *  LOGICA LOCAL (autonomia - RF-07): CO alto en el pasillo dispara la baliza.
 * ========================================================================== */
static void aplicar_logica_local(void)
{
    if (s_sensor_falla) {
        /* Sonda en falla -> activar alarma local por precaucion. */
        s_alarma_local = true;
    } else if (s_co_pct >= UMBRAL_CO_ALARMA) {
        s_alarma_local = true;
    } else if (s_co_pct <= UMBRAL_CO_APAGADO) {
        s_alarma_local = false;   /* histeresis */
    }
    actualizar_estado_alarma();
}

/* ============================================================================
 *  SERIALIZACION CBOR DE /env/air  (payload < 52 bytes)
 *    "t": tipo de nodo (3 = pasillo)
 *    "c": indice de CO 0..100
 *    "a": estado baliza (0 off, 1 on)
 *    "f": flag de falla de sonda (0/1)
 * ========================================================================== */
static int construir_cbor_env(uint8_t *buf, size_t buf_len)
{
    CborEncoder enc, mapa;
    cbor_encoder_init(&enc, buf, buf_len, 0);
    cbor_encoder_create_map(&enc, &mapa, 4);

    cbor_encode_text_stringz(&mapa, "t");
    cbor_encode_uint(&mapa, 3);

    cbor_encode_text_stringz(&mapa, "c");
    cbor_encode_uint(&mapa, (uint64_t)(s_co_pct + 0.5f));

    cbor_encode_text_stringz(&mapa, "a");
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
 *  RECURSO CoAP  /act/alarm  (PUT CON idempotente)
 *  Payload CBOR: entero  1 = activar baliza , 0 = desactivar (orden de central)
 *  Nota: si hay CO alto local, la baliza seguira activa aunque la central
 *        ordene apagar (la condicion local manda para proteger al personal).
 * ========================================================================== */
static void act_alarm_handler(void *ctx, otMessage *msg, const otMessageInfo *info)
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

    int activar = -1;
    if (len > 0) {
        CborParser p; CborValue v;
        if (cbor_parser_init(payload, len, 0, &p, &v) == CborNoError &&
            cbor_value_is_integer(&v)) {
            int val = 0;
            cbor_value_get_int(&v, &val);
            activar = (val != 0) ? 1 : 0;
        }
    }

    otCoapCode resp_code = OT_COAP_CODE_CHANGED;   /* 2.04 */
    if (activar == 1) {
        s_alarma_remota = true;    /* idempotente */
    } else if (activar == 0) {
        s_alarma_remota = false;   /* la central cancela; la local puede seguir */
    } else {
        resp_code = OT_COAP_CODE_BAD_REQUEST;
    }
    actualizar_estado_alarma();

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

    s_recurso_alarm.mUriPath = "act/alarm";
    s_recurso_alarm.mHandler = act_alarm_handler;
    s_recurso_alarm.mContext = NULL;
    s_recurso_alarm.mNext    = NULL;
    otCoapAddResource(ot, &s_recurso_alarm);

    ESP_LOGI(TAG, "Recursos CoAP registrados: /env/air  /act/alarm");
}

/* ============================================================================
 *  TAREA DE LA BALIZA (parpadeo mientras la alarma este activa)
 * ========================================================================== */
static void tarea_baliza(void *arg)
{
    (void)arg;
    bool on = false;
    while (1) {
        if (s_estado == ALARMA_ON) {
            on = !on;
            baliza_salida(on);
        } else {
            on = false;
            baliza_salida(false);
        }
        vTaskDelay(pdMS_TO_TICKS(BALIZA_PARPADEO_MS));
    }
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
        aplicar_logica_local();

        bool cambio    = (co_prev < -999.0f) || (fabsf(s_co_pct - co_prev) >= 2.0f);
        bool es_latido = (xTaskGetTickCount() - ultimo_latido)
                         >= pdMS_TO_TICKS(PERIODO_LATIDO_MS);

        if (cambio || es_latido || s_sensor_falla) {
            co_prev = s_co_pct;
            if (es_latido) ultimo_latido = xTaskGetTickCount();
            ESP_LOGI(TAG, "CO=%.1f%% baliza=%s (rem=%d loc=%d) falla=%d",
                     s_co_pct, s_estado == ALARMA_ON ? "ON" : "OFF",
                     s_alarma_remota, s_alarma_local, s_sensor_falla);
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
    ESP_LOGI(TAG, "=== NODO 3 - Pasillo de Operarios y Baliza ===");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    actuadores_init();   /* baliza apagada al arranque */
    adc_init();

    xTaskCreate(tarea_openthread, "openthread", 8192, NULL, 5, NULL);
    xTaskCreate(tarea_sensor, "sensor", 4096, NULL, 4, NULL);
    xTaskCreate(tarea_baliza, "baliza", 2048, NULL, 4, NULL);
}
