/*
 * ============================================================================
 *  Sistema Autonomo de Monitoreo Atmosferico y Seguridad Diferenciada
 *  Recintos industriales de combustion limpia (Gas Natural CH4 / GLP)
 * ----------------------------------------------------------------------------
 *  NODO 1 - TREN DE GAS Y CORTE DE SUMINISTRO
 *  Placa: ESP32-C6  |  Stack: ESP-IDF + OpenThread + CoAP (UDP/IPv6) + CBOR
 * ----------------------------------------------------------------------------
 *  Funcion:
 *    - Mide gas combustible (CH4/GLP) con un sensor MQ-6 (analogico, ADC).
 *    - Publica telemetria por CoAP Observe en  /env/air  (payload CBOR < 52 B).
 *    - Expone el recurso de actuacion         /act/cutoff (PUT CON idempotente).
 *    - Controla un rele que energiza una electrovalvula NORMALMENTE CERRADA (NC):
 *        rele energizado (GPIO en LOW)  -> valvula ABIERTA  -> paso de gas (LED verde)
 *        rele desenergizado (GPIO HIGH) -> valvula CERRADA  -> corte       (LED rojo)
 *
 *  REGLA DE SEGURIDAD (fail-safe por diseno):
 *    Ante fuga de CH4/GLP o perdida de energia, el rele se DESENERGIZA y el
 *    resorte mecanico de la valvula NC corta el gas en < 1 s. Este nodo NUNCA
 *    enciende extractores (se evita chispa en atmosfera potencialmente explosiva).
 *
 *  MAPA DE PINES (segun especificacion del proyecto):
 *    - MQ-6  AO  -> GPIO2  (ADC1_CH2)   [salida 5V pasada por divisor 10k+15k -> <=3.0 V]
 *    - Rele  IN  -> GPIO4  (disparo en LOW / Low Level Trigger)
 *
 *  Autoria: equipo de Linux Embebido / IoT.
 * ============================================================================
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include <math.h>

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

static const char *TAG = "nodo1_tren_gas";

/* ----------------------------------------------------------------------------
 *  Macros de configuracion por defecto de OpenThread.
 *  En los ejemplos de ESP-IDF estas macros viven en "esp_ot_config.h" (un
 *  header propio del ejemplo, no instalado). Las definimos aqui con los mismos
 *  valores estandar para no depender de ese archivo.
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
#define MQ6_ADC_GPIO            2          /* GPIO2  = ADC1_CH2 (AO del MQ-6)       */
#define MQ6_ADC_CHANNEL         ADC_CHANNEL_2
#define RELE_CUTOFF_GPIO        4          /* GPIO4  -> IN del modulo rele (NC)     */

/* El modulo rele RM1-5V-1 dispara en LOW (Low Level Trigger):
 *   GPIO = 0 (LOW)  -> rele ENERGIZADO  -> valvula ABIERTA (paso de gas)
 *   GPIO = 1 (HIGH) -> rele APAGADO     -> valvula CERRADA (corte de seguridad)   */
#define RELE_ON_ABRIR_GAS       0          /* nivel para energizar rele / abrir     */
#define RELE_OFF_CORTAR_GAS     1          /* nivel para desenergizar / cortar      */

/* --- Umbrales (valores en "cuentas" relativas 0..100 del gas combustible).
 *  Se calibran en campo; aqui se dejan valores de arranque conservadores.
 *  El valor reportado "gas_pct" es un indice de concentracion normalizado.      --- */
#define UMBRAL_GAS_ALERTA       45.0f      /* aviso temprano                        */
#define UMBRAL_GAS_CORTE        60.0f      /* corte inmediato de la valvula (LEL)   */

/* --- Telemetria --- */
#define PERIODO_MUESTREO_MS     1000       /* lectura del sensor cada 1 s           */
#define PERIODO_LATIDO_MS       30000      /* heartbeat aunque no haya cambios      */
#define MAX_AGE_SEG             60         /* frescura del dato (contrato RF-06)    */

/* --- Filtro de media movil --- */
#define VENTANA_FILTRO          8

/* ============================================================================
 *  ESTADO GLOBAL DEL NODO
 * ========================================================================== */

typedef enum {
    VALVULA_ABIERTA = 0,    /* operacion normal: paso de gas habilitado */
    VALVULA_CERRADA = 1,    /* corte de seguridad activo                */
} estado_valvula_t;

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t         s_adc_cali   = NULL;
static bool                      s_adc_cali_ok = false;

static float                     s_ventana[VENTANA_FILTRO] = {0};
static int                       s_ventana_idx = 0;
static bool                      s_ventana_llena = false;

static volatile float            s_gas_pct = 0.0f;      /* ultimo indice de gas  */
static volatile bool             s_sensor_falla = false;/* plausibilidad (5.03)  */
static volatile estado_valvula_t s_estado = VALVULA_ABIERTA;

static otCoapResource            s_recurso_cutoff;      /* /act/cutoff           */
static otCoapResource            s_recurso_env;         /* /env/air (GET+Observe)*/

/* ============================================================================
 *  CONTROL DEL RELE / ELECTROVALVULA NC
 * ========================================================================== */

/* Abre el gas: energiza el rele (vence el resorte de la valvula NC). */
static void valvula_abrir(void)
{
    gpio_set_level(RELE_CUTOFF_GPIO, RELE_ON_ABRIR_GAS);
    s_estado = VALVULA_ABIERTA;
    ESP_LOGI(TAG, "Valvula ABIERTA (rele energizado, paso de gas habilitado)");
}

/* Corta el gas: desenergiza el rele -> el resorte cierra la valvula en < 1 s. */
static void valvula_cortar(void)
{
    gpio_set_level(RELE_CUTOFF_GPIO, RELE_OFF_CORTAR_GAS);
    s_estado = VALVULA_CERRADA;
    ESP_LOGW(TAG, "Valvula CERRADA (CORTE DE SEGURIDAD de gas)");
}

static void rele_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << RELE_CUTOFF_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    /* ARRANQUE SEGURO: iniciar con la valvula cerrada hasta confirmar el enlace. */
    valvula_cortar();
}

/* ============================================================================
 *  LECTURA DEL SENSOR MQ-6 (ADC + filtro + plausibilidad)
 * ========================================================================== */

static void adc_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,   /* rango ~0..3.1 V (coincide con divisor)  */
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, MQ6_ADC_CHANNEL, &chan_cfg));

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .chan     = MQ6_ADC_CHANNEL,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali) == ESP_OK) {
        s_adc_cali_ok = true;
    } else {
        ESP_LOGW(TAG, "Calibracion ADC no disponible; se usa valor crudo");
    }
}

/* Aplica filtro de media movil sobre el porcentaje instantaneo. */
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

/*
 * Lee el MQ-6, detecta fallas de sonda (plausibilidad) y devuelve un indice
 * normalizado 0..100. Marca s_sensor_falla si la lectura es implausible
 * (0 V = sonda desconectada, o saturacion = cortocircuito).
 */
static float leer_gas(void)
{
    int raw = 0;
    if (adc_oneshot_read(s_adc_handle, MQ6_ADC_CHANNEL, &raw) != ESP_OK) {
        s_sensor_falla = true;
        return s_gas_pct;
    }

    int mv = raw;
    if (s_adc_cali_ok) {
        adc_cali_raw_to_voltage(s_adc_cali, raw, &mv);  /* mv = milivoltios */
    }

    /* Plausibilidad: el divisor entrega ~0.1..3.0 V en operacion valida.
     * < 50 mV  -> sonda desconectada / sin alimentacion del calefactor.
     * > 3150 mV-> saturacion / cortocircuito.                                   */
    if (mv < 50 || mv > 3150) {
        s_sensor_falla = true;
        ESP_LOGW(TAG, "MQ-6 implausible (%d mV) -> posible sonda desconectada", mv);
        return s_gas_pct;
    }
    s_sensor_falla = false;

    /* Normaliza 0..3000 mV -> 0..100 (indice de concentracion). */
    float pct = (mv / 3000.0f) * 100.0f;
    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;

    return filtro_media_movil(pct);
}

/* ============================================================================
 *  LOGICA LOCAL DE SEGURIDAD (autonomia sin central - RF-07)
 *  Si la central esta presente, ella decide; pero si se pierde el enlace,
 *  el nodo debe actuar por si mismo. Aqui se implementa esa red de respaldo.
 * ========================================================================== */
static void aplicar_logica_local(void)
{
    /* Fail-safe inverso: si la sonda falla, pasar a estado seguro = CERRAR. */
    if (s_sensor_falla) {
        if (s_estado != VALVULA_CERRADA) {
            ESP_LOGW(TAG, "Sonda en falla -> CORTE preventivo de gas (estado seguro)");
            valvula_cortar();
        }
        return;
    }

    if (s_gas_pct >= UMBRAL_GAS_CORTE) {
        if (s_estado != VALVULA_CERRADA) {
            ESP_LOGW(TAG, "Gas %.1f%% >= corte %.1f%% -> CIERRE electrovalvula",
                     s_gas_pct, UMBRAL_GAS_CORTE);
            valvula_cortar();
        }
    }
    /* Nota: la reapertura NO es automatica; requiere orden explicita de la
     * central (PUT /act/cutoff con v=0) tras verificar que la zona es segura. */
}

/* ============================================================================
 *  SERIALIZACION CBOR DE /env/air   (payload < 52 bytes)
 *  Mapa CBOR compacto con claves de un caracter:
 *    "t": tipo de nodo (1 = tren de gas)
 *    "g": indice de gas combustible 0..100 (entero)
 *    "v": estado valvula (0 abierta, 1 cerrada)
 *    "f": flag de falla de sonda (0/1)
 * ========================================================================== */
static int construir_cbor_env(uint8_t *buf, size_t buf_len)
{
    CborEncoder enc, mapa;
    cbor_encoder_init(&enc, buf, buf_len, 0);
    cbor_encoder_create_map(&enc, &mapa, 4);

    cbor_encode_text_stringz(&mapa, "t");
    cbor_encode_uint(&mapa, 1);                      /* nodo 1 */

    cbor_encode_text_stringz(&mapa, "g");
    cbor_encode_uint(&mapa, (uint64_t)(s_gas_pct + 0.5f));

    cbor_encode_text_stringz(&mapa, "v");
    cbor_encode_uint(&mapa, (uint64_t)s_estado);

    cbor_encode_text_stringz(&mapa, "f");
    cbor_encode_uint(&mapa, s_sensor_falla ? 1 : 0);

    cbor_encoder_close_container(&enc, &mapa);
    return (int)cbor_encoder_get_buffer_size(&enc, buf);
}

/* ============================================================================
 *  RECURSO CoAP  /env/air   (GET -> telemetria CBOR, soporta Observe)
 * ========================================================================== */
static void env_air_handler(void *ctx, otMessage *msg, const otMessageInfo *info)
{
    (void)ctx;
    otInstance *ot = esp_openthread_get_instance();

    otCoapCode code = otCoapMessageGetCode(msg);
    if (code != OT_COAP_CODE_GET) {
        return;  /* solo lectura */
    }

    uint8_t payload[52];
    int len = construir_cbor_env(payload, sizeof(payload));

    otMessage *resp = otCoapNewMessage(ot, NULL);
    if (resp == NULL) return;

    otCoapMessageInitResponse(resp, msg, OT_COAP_TYPE_ACKNOWLEDGMENT,
                              OT_COAP_CODE_CONTENT);
    otCoapMessageSetToken(resp, otCoapMessageGetToken(msg),
                          otCoapMessageGetTokenLength(msg));

    /* Max-Age dinamico: declara que el dato caduca a los 60 s (RF-06). */
    otCoapMessageAppendMaxAgeOption(resp, MAX_AGE_SEG);
    otCoapMessageAppendContentFormatOption(resp,
        OT_COAP_OPTION_CONTENT_FORMAT_CBOR);
    otCoapMessageSetPayloadMarker(resp);
    otMessageAppend(resp, payload, len);

    otError err = otCoapSendResponse(ot, resp, info);
    if (err != OT_ERROR_NONE) {
        otMessageFree(resp);
        ESP_LOGW(TAG, "Error enviando /env/air: %d", err);
    }
}

/* ============================================================================
 *  RECURSO CoAP  /act/cutoff  (PUT CON idempotente)
 *  Payload CBOR: entero  1 = cerrar gas (corte) ,  0 = abrir gas (reponer)
 *  Idempotente: repetir el mismo valor deja el mismo estado, sin parpadeos.
 * ========================================================================== */
static void act_cutoff_handler(void *ctx, otMessage *msg, const otMessageInfo *info)
{
    (void)ctx;
    otInstance *ot = esp_openthread_get_instance();
    otCoapCode  code = otCoapMessageGetCode(msg);

    if (code != OT_COAP_CODE_PUT) {
        /* responder metodo no permitido */
        otMessage *r = otCoapNewMessage(ot, NULL);
        if (r) {
            otCoapMessageInitResponse(r, msg, OT_COAP_TYPE_ACKNOWLEDGMENT,
                                      OT_COAP_CODE_METHOD_NOT_ALLOWED);
            if (otCoapSendResponse(ot, r, info) != OT_ERROR_NONE) otMessageFree(r);
        }
        return;
    }

    /* Leer payload (1 byte CBOR suele bastar: 0x00=abrir, 0x01=cerrar). */
    uint16_t off = otMessageGetOffset(msg);
    uint16_t len = otMessageGetLength(msg) - off;
    uint8_t  payload[16] = {0};
    if (len > sizeof(payload)) len = sizeof(payload);
    otMessageRead(msg, off, payload, len);

    int cerrar = -1;
    if (len > 0) {
        CborParser p; CborValue v;
        if (cbor_parser_init(payload, len, 0, &p, &v) == CborNoError &&
            cbor_value_is_integer(&v)) {
            int val = 0;
            cbor_value_get_int(&v, &val);
            cerrar = (val != 0) ? 1 : 0;
        }
    }

    otCoapCode resp_code = OT_COAP_CODE_CHANGED;  /* 2.04 */
    if (cerrar == 1) {
        valvula_cortar();                 /* idempotente: cerrar siempre deja cerrado */
    } else if (cerrar == 0) {
        /* Solo reponer gas si NO hay condicion de peligro vigente. */
        if (s_sensor_falla || s_gas_pct >= UMBRAL_GAS_CORTE) {
            resp_code = OT_COAP_CODE_FORBIDDEN;  /* 4.03: no se permite reabrir */
            ESP_LOGW(TAG, "Rechazada reapertura: condicion de peligro vigente");
        } else {
            valvula_abrir();
        }
    } else {
        resp_code = OT_COAP_CODE_BAD_REQUEST;    /* 4.00 */
    }

    if (otCoapMessageGetType(msg) == OT_COAP_TYPE_CONFIRMABLE) {
        otMessage *resp = otCoapNewMessage(ot, NULL);
        if (resp) {
            otCoapMessageInitResponse(resp, msg, OT_COAP_TYPE_ACKNOWLEDGMENT, resp_code);
            otCoapMessageSetToken(resp, otCoapMessageGetToken(msg),
                                  otCoapMessageGetTokenLength(msg));
            if (otCoapSendResponse(ot, resp, info) != OT_ERROR_NONE) {
                otMessageFree(resp);
            }
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

    s_recurso_cutoff.mUriPath = "act/cutoff";
    s_recurso_cutoff.mHandler = act_cutoff_handler;
    s_recurso_cutoff.mContext = NULL;
    s_recurso_cutoff.mNext    = NULL;
    otCoapAddResource(ot, &s_recurso_cutoff);

    ESP_LOGI(TAG, "Recursos CoAP registrados: /env/air  /act/cutoff");
}

/* ============================================================================
 *  TAREA DE MUESTREO Y TELEMETRIA
 * ========================================================================== */
static void tarea_sensor(void *arg)
{
    (void)arg;
    TickType_t ultimo_latido = xTaskGetTickCount();
    float gas_prev = -1000.0f;

    while (1) {
        float gas = leer_gas();
        s_gas_pct = gas;

        /* Logica local de respaldo (por si no hay central). */
        aplicar_logica_local();

        /* Marca de cambio significativo o latido periodico.
         * NOTA: el servidor CoAP de OpenThread no reenvia Observe de forma
         * automatica. En esta etapa la central lee /env/air por peticion GET
         * (polling). Cuando integremos la central decidiremos si el nodo
         * publica como cliente CoAP o si se implementa Observe completo. */
        bool cambio    = (gas_prev < -999.0f) || (fabsf(gas - gas_prev) >= 2.0f);
        bool es_latido = (xTaskGetTickCount() - ultimo_latido)
                         >= pdMS_TO_TICKS(PERIODO_LATIDO_MS);

        if (cambio || es_latido || s_sensor_falla) {
            gas_prev = gas;
            if (es_latido) ultimo_latido = xTaskGetTickCount();
            ESP_LOGI(TAG, "gas=%.1f%% valvula=%s falla=%d",
                     gas, s_estado == VALVULA_CERRADA ? "CERRADA" : "ABIERTA",
                     s_sensor_falla);
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

    /* El driver OpenThread de ESP-IDF usa eventfd para notificar tareas. */
    esp_vfs_eventfd_config_t eventfd_cfg = {
        .max_fds = 3,
    };
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_cfg));

    ESP_ERROR_CHECK(esp_openthread_init(&config));

    /* Crear e instalar la interfaz de red (netif) de OpenThread. */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_OPENTHREAD();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    assert(netif != NULL);
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_openthread_netif_glue_init(&config)));

    otInstance *ot = esp_openthread_get_instance();

    /* Parametros de red definidos por el proyecto. */
    otLinkSetChannel(ot, 25);                 /* Canal 25 (2475 MHz) */
    otPlatRadioSetTransmitPower(ot, 20);      /* +20 dBm             */

    /* Habilitar la pila y unirse/crear la malla Thread. */
    otIp6SetEnabled(ot, true);
    otThreadSetEnabled(ot, true);

    coap_init_recursos();

    /* Lanzar el bucle principal de OpenThread (bloqueante). */
    esp_openthread_launch_mainloop();

    /* Si el mainloop retorna, limpiar. */
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
    ESP_LOGI(TAG, "=== NODO 1 - Tren de Gas y Corte de Suministro ===");

    ESP_ERROR_CHECK(nvs_flash_init());

    /* Infraestructura de red: pila netif + bucle de eventos del sistema.
     * El driver de OpenThread registra sus manejadores en este event loop;
     * si no existe, esp_netif_attach falla con ESP_ERR_INVALID_STATE (259). */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Hardware local primero: dejar la valvula en estado seguro (cerrada). */
    rele_init();
    adc_init();

    /* Red Thread + CoAP en su propia tarea. */
    xTaskCreate(tarea_openthread, "openthread", 8192, NULL, 5, NULL);

    /* Muestreo de sensor y telemetria. */
    xTaskCreate(tarea_sensor, "sensor", 4096, NULL, 4, NULL);
}
