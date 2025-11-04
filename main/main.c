
/* 
 * Sistema completo ESP32: OTA GitHub + WiFi + MQTT ThingsBoard
 * Código independiente sin protocol_examples_common
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "driver/adc.h"
#include "esp_crt_bundle.h"

// Declarar las funciones antes de usarlas
bool verify_url_no_ssl(void);
void check_ota_updates(void);


// =============================================================================
// CONFIGURACIÓN - ¡ACTUALIZA CON TUS DATOS REALES!
// =============================================================================
#define WIFI_SSID      "SBC"
#define WIFI_PASS      "SBCwifi$"      
#define MAX_INTENTOS   10
#define THINGSBOARD_MQTT_URI "mqtt://demo.thingsboard.io"
#define THINGSBOARD_ACCESS_TOKEN "CC34vYEp44Z00eoPKLfV"
#define GITHUB_FIRMWARE_URL "https://raw.githubusercontent.com/carlxx2/Hito_6/main/firmware/hito6.bin"
#define LDR_ADC_CHANNEL    ADC1_CHANNEL_4


static const char *TAG = "SENSOR_OTA_SYSTEM";

// =============================================================================
// VARIABLES GLOBALES
// =============================================================================
static EventGroupHandle_t s_wifi_event_group;
static int wifi_conectado = 0;
static bool mqtt_conectado = false;
static esp_mqtt_client_handle_t mqtt_client = NULL;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// =============================================================================
// WIFI - CÓDIGO INDEPENDIENTE
// =============================================================================
static void wifi_event_handler(void* arg, esp_event_base_t event_base, 
                              int32_t event_id, void* event_data) {
    static int retry_num = 0;
    
    // CORREGIDO: Usar %d para int32_t
    ESP_LOGI(TAG, "Evento WiFi: %s, ID: %d", event_base, (int)event_id);
    
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "📶 WiFi iniciado - Conectando...");
        esp_wifi_connect();
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_conectado = 0;
        mqtt_conectado = false;
        
        wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGE(TAG, "📶 WiFi desconectado. Razón: %d", disconnected->reason);
        
        if (retry_num < MAX_INTENTOS) {
            esp_wifi_connect();
            retry_num++;
            ESP_LOGI(TAG, "🔄 Reintento WiFi %d/%d", retry_num, MAX_INTENTOS);
        } else {
            ESP_LOGE(TAG, "❌ Fallo permanente WiFi");
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        }
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_conectado = 1;
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "✅ WiFi conectado! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        retry_num = 0;
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

void wifi_init_sta(void) {
    ESP_LOGI(TAG, "🔌 Inicializando WiFi...");
    ESP_LOGI(TAG, "Conectando a: %s", WIFI_SSID);
    
    s_wifi_event_group = xEventGroupCreate();
    
    // Inicializar stack de red
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    
    // Configurar WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Registrar manejadores
    esp_event_handler_instance_t instance_any_id, instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, &instance_got_ip));
    
    // Configurar credenciales
    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "Esperando conexión WiFi...");
    
    // Esperar conexión con timeout
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    
    // Limpiar
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    
    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "✅ WiFi CONECTADO!");
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "❌ FALLO WiFi - Verifica SSID/Password");
    } else {
        ESP_LOGE(TAG, "⏰ TIMEOUT WiFi");
    }
}

// =============================================================================
// MQTT
// =============================================================================
static void mqtt_event_handler(void *arg, esp_event_base_t event_base, 
                              int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            mqtt_conectado = true;
            ESP_LOGI(TAG, "✅ MQTT conectado a ThingsBoard");
            break;
        case MQTT_EVENT_DISCONNECTED:
            mqtt_conectado = false;
            ESP_LOGW(TAG, "❌ MQTT desconectado");
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "📤 MQTT mensaje publicado (ID: %d)", event->msg_id);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "❌ Error MQTT");
            break;
        default:
            break;
    }
}

void mqtt_init(void) {
    ESP_LOGI(TAG, "🌐 Inicializando MQTT...");
    
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = THINGSBOARD_MQTT_URI,
        .credentials.username = THINGSBOARD_ACCESS_TOKEN,
    };
    
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!mqtt_client) {
        ESP_LOGE(TAG, "❌ Error inicializando cliente MQTT");
        return;
    }
    
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_err_t start_result = esp_mqtt_client_start(mqtt_client);
    if (start_result != ESP_OK) {
        ESP_LOGE(TAG, "❌ Error iniciando cliente MQTT: %s", esp_err_to_name(start_result));
    }
}

void send_mqtt_telemetry(float luminosity) {
    if (mqtt_conectado && mqtt_client) {
        char message[100];
        snprintf(message, sizeof(message), "{\"luminosity\":%.2f}", luminosity);
        
        int msg_id = esp_mqtt_client_publish(mqtt_client, 
                                           "v1/devices/me/telemetry",
                                           message, 0, 1, 0);
        
        if (msg_id < 0) {
            ESP_LOGE(TAG, "❌ Error publicando mensaje MQTT");
        } else {
            ESP_LOGI(TAG, "📤 Datos enviados: %s", message);
        }
    } else {
        ESP_LOGW(TAG, "⚠️ MQTT no conectado, no se pueden enviar datos");
    }
}

// =============================================================================
// SENSORES
// =============================================================================
float read_ldr_value(void) {
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(LDR_ADC_CHANNEL, ADC_ATTEN_DB_11);
    
    int total = 0;
    for (int i = 0; i < 10; i++) {
        total += adc1_get_raw(LDR_ADC_CHANNEL);
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    int raw_value = total / 10;
    float percentage = (raw_value / 4095.0) * 100.0;
    
    ESP_LOGI(TAG, "💡 LDR - Raw: %d, Porcentaje: %.2f%%", raw_value, percentage);
    return percentage;
}

void init_sensors(void) {
    ESP_LOGI(TAG, "🔧 Inicializando sensores...");
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(LDR_ADC_CHANNEL, ADC_ATTEN_DB_11);
}

// =============================================================================
// VERIFICACIÓN DE URL SIN SSL
// =============================================================================
// Agrega esta función para diagnosticar la URL
/*void verify_github_url(void) {
    ESP_LOGI(TAG, "🔍 Verificando URL de GitHub...");
    ESP_LOGI(TAG, "📋 URL actual: %s", GITHUB_FIRMWARE_URL);
    
    // Prueba estas URLs alternativas:
    const char* test_urls[] = {
        "https://raw.githubusercontent.com/carlxx2/Hito_6/main/firmware/hito6.bin",
        "https://raw.githubusercontent.com/carlxx2/Hito_6/main/build/hito6.bin",
        "https://github.com/carlxx2/Hito_6/raw/main/firmware/hito6.bin",  // Formato alternativo
    };
    
    for (int i = 0; i < sizeof(test_urls)/sizeof(test_urls[0]); i++) {
        ESP_LOGI(TAG, "🔗 Probando: %s", test_urls[i]);
        
        esp_http_client_config_t config = {
            .url = test_urls[i],
            .method = HTTP_METHOD_HEAD,
            .timeout_ms = 10000,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        
        esp_http_client_handle_t client = esp_http_client_init(&config);
        esp_err_t err = esp_http_client_perform(client);
        
        if (err == ESP_OK) {
            int status = esp_http_client_get_status_code(client);
            ESP_LOGI(TAG, "📡 Status: %d - %s", status, 
                    status == 200 ? "✅ ARCHIVO ENCONTRADO" : "❌ NO ENCONTRADO");
        } else {
            ESP_LOGE(TAG, "❌ Error: %s", esp_err_to_name(err));
        }
        
        esp_http_client_cleanup(client);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}*/

// =============================================================================
// OTA COMPLETO SIN SSL
// =============================================================================
void check_ota_updates(void) {
    ESP_LOGI(TAG, "🔍 OTA con diagnóstico completo...");
    
    // 1. Primero verificar la URL
    //verify_github_url();
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    
    // 2. Intentar OTA
    ESP_LOGI(TAG, "📥 Iniciando descarga OTA...");
    
    esp_http_client_config_t config = {
        .url = GITHUB_FIRMWARE_URL,
        .timeout_ms = 60000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
    };
    
    esp_https_ota_config_t ota_config = {
        .http_config = &config,
        .partial_http_download = true,
    };
    
    esp_err_t ret = esp_https_ota(&ota_config);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "🎉 OTA EXITOSO! Reiniciando...");
        vTaskDelay(3000 / portTICK_PERIOD_MS);
        esp_restart();
    } else {
        ESP_LOGE(TAG, "❌ OTA falló: %s", esp_err_to_name(ret));
        
    }
}

// =============================================================================
// FUNCIÓN PRINCIPAL
// =============================================================================
void app_main(void) {
    ESP_LOGI(TAG, "🚀 Iniciando Sistema...");
    
    // 1. INICIALIZAR NVS
    ESP_LOGI(TAG, "📁 Inicializando NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // 2. INICIALIZAR WIFI
    wifi_init_sta();
    
    if (wifi_conectado) {
        // 3. INICIALIZAR MQTT
        mqtt_init();
        
        // 4. INICIALIZAR SENSORES
        init_sensors();
        
        // 5. OTA SIN CERTIFICADOS - ELIGE UNA OPCIÓN:
        
        // Opción A: Simple (recomendada)
        ESP_LOGI(TAG, "🔍 Verificando OTA...");
        check_ota_updates();
        
        // Opción B: Con verificación previa
        // check_ota_updates_simple();
        
        ESP_LOGI(TAG, "✅ Sistema operativo");
        
        // 6. LOOP PRINCIPAL
        int cycle_count = 0;
        while (1) {
            cycle_count++;
            ESP_LOGI(TAG, "=== CICLO %d ===", cycle_count);
            
            float luminosity = read_ldr_value();
            send_mqtt_telemetry(luminosity);
            
            vTaskDelay(6000 / portTICK_PERIOD_MS);
        }
    } else {
        ESP_LOGE(TAG, "❌ Fallo WiFi - Reiniciando...");
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        esp_restart();
    }
}
