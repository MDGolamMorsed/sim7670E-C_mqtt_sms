#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "mqtt_client.h"
#include "esp_modem_api.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

#define TAG "SIM7670_MQTT"

#if CONFIG_SIM7670_MQTT_ENABLE
static esp_mqtt_client_handle_t mqtt_client = NULL;

// --- MQTT Event Handler ---
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT Connected to HiveMQ");
        // Subscribe to the topic
        esp_mqtt_client_subscribe(mqtt_client, CONFIG_SIM7670_MQTT_TOPIC, 0);
        // Publish a test message
        esp_mqtt_client_publish(mqtt_client, CONFIG_SIM7670_MQTT_TOPIC, "Hello from SIM7670C via PPPoS", 0, 1, 0);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT Disconnected");
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "Message Received on topic: %.*s", event->topic_len, event->topic);
        ESP_LOGI(TAG, "DATA=%.*s", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT Error");
        break;
    default:
        break;
    }
}
#endif // CONFIG_SIM7670_MQTT_ENABLE

// --- Network Event Handler (PPPoS) ---
static void on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Modem Connected. Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

#if CONFIG_SIM7670_MQTT_ENABLE
        // Initialize MQTT only after we have an IP address
        esp_mqtt_client_config_t mqtt_cfg = {
            .broker.address.uri = CONFIG_SIM7670_MQTT_BROKER_URL,
            .credentials =
                {
                    .username = CONFIG_SIM7670_MQTT_USERNAME,
                    .authentication.password = CONFIG_SIM7670_MQTT_PASSWORD,
                },
        };

        mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
        esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        esp_mqtt_client_start(mqtt_client);
#endif // CONFIG_SIM7670_MQTT_ENABLE
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "Modem Lost IP");
#if CONFIG_SIM7670_MQTT_ENABLE
        if (mqtt_client) {
            esp_mqtt_client_stop(mqtt_client);
            esp_mqtt_client_destroy(mqtt_client);
            mqtt_client = NULL;
        }
#endif // CONFIG_SIM7670_MQTT_ENABLE
    }
}

// --- SMS Parsing Function ---
static void handle_sms_content(const char *sms_text) {
    int dht_h, dht_l, temp_h, temp_l;
    // Check for the specific format: #dht:H22,L20;temp:H23,L15;#
    // We look for the start of the pattern
    const char *pattern_start = strstr(sms_text, "#dht:");
    
    if (pattern_start) {
        if (sscanf(pattern_start, "#dht:H%d,L%d;temp:H%d,L%d;#", &dht_h, &dht_l, &temp_h, &temp_l) == 4) {
            ESP_LOGI(TAG, "SMS DECODE: dht sensor high threshold is %d and low threshold is %d. same as temp sensor (H%d, L%d)", 
                     dht_h, dht_l, temp_h, temp_l);
        } else {
            ESP_LOGW(TAG, "SMS matched prefix but failed to parse values: %s", pattern_start);
        }
    } else {
        ESP_LOGI(TAG, "Received SMS (Raw): %s", sms_text);
    }
}

void app_main(void) {
    // 1. Initialize NVS and Netif
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 2. Register IP Event Handlers to detect when 4G connects
#if CONFIG_SIM7670_MQTT_ENABLE
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));
#endif

    // 3. Configure Modem DTE (UART Interface)
    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_config.uart_config.baud_rate = CONFIG_SIM7670_BAUD_RATE;
    dte_config.uart_config.tx_io_num = CONFIG_SIM7670_TX_PIN;
    dte_config.uart_config.rx_io_num = CONFIG_SIM7670_RX_PIN;
    dte_config.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE; 
    dte_config.task_stack_size = 4096; // Increase stack for modem tasks
    dte_config.task_priority = 5;

    // 4. Configure Modem DCE (Device Configuration)
    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(CONFIG_SIM7670_APN);

    // 5. Create the Network Interface (PPP)
    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    esp_netif_t *esp_netif = esp_netif_new(&netif_ppp_config);
    
    // --- Fix for Warm Boot: Force Modem out of Data Mode ---
    // Configure DTR as GPIO to manually drop the connection
    gpio_config_t dtr_conf = {
        .pin_bit_mask = (1ULL << CONFIG_SIM7670_DTR_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&dtr_conf);
    ESP_LOGI(TAG, "Toggling DTR to exit Data Mode...");
    gpio_set_level(CONFIG_SIM7670_DTR_PIN, 0); // DTR low to hang up
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(CONFIG_SIM7670_DTR_PIN, 1); // DTR high to re-enable AT command mode
    vTaskDelay(pdMS_TO_TICKS(200));

    // 6. Initialize the Modem Device
    // We use ESP_MODEM_DCE_SIM7600 as it is compatible with SIM7670C for PPPoS
    esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte_config, &dce_config, esp_netif);

    if (!dce) {
        ESP_LOGE(TAG, "Failed to create modem device");
        return;
    }

    esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
    // esp_modem_destroy(dce);

    // Pulse the Power Key to turn on the modem
    // gpio_set_direction(4, GPIO_MODE_OUTPUT);
    // gpio_set_level(4, 0);
    // vTaskDelay(pdMS_TO_TICKS(1000));
    // gpio_set_level(4, 1);
    ESP_LOGI(TAG, "Waiting for modem to boot...");
    vTaskDelay(pdMS_TO_TICKS(1000)); // Wait for boot

    // Check if modem is responding to AT commands
    ESP_LOGI(TAG, "Checking modem response...");
    bool modem_ready = false;
    for (int i = 0; i < 20; i++) {
        esp_err_t err = esp_modem_sync(dce);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Modem responded to AT command");
            modem_ready = true;
            break;
        }
        ESP_LOGW(TAG, "Modem not responding... retrying (%d/20)", i + 1);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    if (!modem_ready) {
        ESP_LOGE(TAG, "Modem failed to respond. Aborting.");
        return;
    }

    // IMPORTANT: For SMS receiving to work, the modem must be configured for text mode
    // and to notify of new messages. This is typically done with:
    // AT+CMGF=1  (Set SMS to text mode)
    // AT+CNMI=2,1 (Notify DTE when new SMS is stored)
    // These commands should be sent to the modem once (e.g., using a serial tool).

#if CONFIG_SIM7670_MQTT_ENABLE
    ESP_LOGI(TAG, "MQTT mode enabled. Will connect to 4G network.");
    // If SMS is also enabled, send a test SMS before entering data mode
    #if CONFIG_SIM7670_SMS_ENABLE
        if (strlen(CONFIG_SIM7670_SMS_PHONE_NO) > 0) {
            ESP_LOGI(TAG, "Sending test SMS to %s", CONFIG_SIM7670_SMS_PHONE_NO);
            esp_err_t sms_err = esp_modem_send_sms(dce, CONFIG_SIM7670_SMS_PHONE_NO, "Hello from ESP32 & SIM7670!");
            if (sms_err == ESP_OK) {
                ESP_LOGI(TAG, "SMS sent successfully");
            } else {
                ESP_LOGE(TAG, "Failed to send SMS: %s", esp_err_to_name(sms_err));
            }
        }
    #endif

    // 7. Connect to the network (Enters Data Mode for MQTT)
    ESP_LOGI(TAG, "Connecting to 4G network...");
    esp_err_t err = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set data mode: %s", esp_err_to_name(err));
    }
    // The rest is handled by the on_ip_event handler
#elif CONFIG_SIM7670_SMS_ENABLE
    ESP_LOGI(TAG, "SMS-only mode enabled. Polling for incoming messages.");
    
    // Set SMS to Text Mode
    esp_modem_at(dce, "AT+CMGF=1", NULL, 1000);
    // Set preferred storage to SIM (SM). This ensures we look in the right place.
    esp_modem_at(dce, "AT+CPMS=\"SM\",\"SM\",\"SM\"", NULL, 1000);

    char *sms_buffer = (char *)malloc(1024);
    if (sms_buffer) {
        while (1) {
            memset(sms_buffer, 0, 1024);
            // List unread messages. They will be marked as read after listing.
            if (esp_modem_at(dce, "AT+CMGL=\"REC UNREAD\"", sms_buffer, 5000) == ESP_OK) {
                if (strlen(sms_buffer) > 4) { // Log if we get more than just "OK"
                    ESP_LOGI(TAG, "Raw SMS Response: %s", sms_buffer);
                    handle_sms_content(sms_buffer);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(5000)); // Check every 5 seconds
        }
        free(sms_buffer);
    }
#else
    ESP_LOGI(TAG, "Modem initialized. MQTT and SMS are disabled. Idling.");
#endif

    // The main task has finished its setup.
    // For SMS-only mode, the modem's event task will handle incoming messages.
    // For MQTT mode, the PPP and MQTT event handlers will manage the connection.
}