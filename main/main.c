#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "mqtt_client.h"
#include "esp_modem_api.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#define TAG "SIM7670_MQTT"

// HiveMQ Public Broker Details
#define BROKER_URL "mqtt://broker.hivemq.com"
#define MQTT_TOPIC "esp32/response"

// Pin Configuration (Adjust based on your wiring)
#define MODEM_TX_PIN 18
#define MODEM_RX_PIN 17
#define MODEM_DTR_PIN 45
#define MODEM_RI_PIN 40

// APN Configuration (Change this to your SIM card's APN)
#define MODEM_APN "gpinternet" 

static esp_mqtt_client_handle_t mqtt_client = NULL;

// --- MQTT Event Handler ---
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT Connected to HiveMQ");
        // Subscribe to the topic
        esp_mqtt_client_subscribe(mqtt_client, MQTT_TOPIC, 0);
        // Publish a test message
        esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, "Hello from SIM7670C via PPPoS", 0, 1, 0);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT Disconnected");
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "Message Received on topic: %.*s", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT Error");
        break;
    default:
        break;
    }
}

// --- Network Event Handler (PPPoS) ---
static void on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Modem Connected. Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

        // Initialize MQTT only after we have an IP address
        esp_mqtt_client_config_t mqtt_cfg = {
            .broker.address.uri = BROKER_URL,
        };
        
        mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
        esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        esp_mqtt_client_start(mqtt_client);
        
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "Modem Lost IP");
        if (mqtt_client) {
            esp_mqtt_client_stop(mqtt_client);
            esp_mqtt_client_destroy(mqtt_client);
            mqtt_client = NULL;
        }
    }
}

void app_main(void) {
    // 1. Initialize NVS and Netif
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 2. Register IP Event Handlers to detect when 4G connects
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));

    // 3. Configure Modem DTE (UART Interface)
    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_config.uart_config.tx_io_num = MODEM_TX_PIN;
    dte_config.uart_config.rx_io_num = MODEM_RX_PIN;
    dte_config.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE; 
    dte_config.task_stack_size = 4096; // Increase stack for modem tasks
    dte_config.task_priority = 5;

    // 4. Configure Modem DCE (Device Configuration)
    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(MODEM_APN);

    // 5. Create the Network Interface (PPP)
    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    esp_netif_t *esp_netif = esp_netif_new(&netif_ppp_config);
    
    // --- Fix for Warm Boot: Force Modem out of Data Mode ---
    // Configure DTR as GPIO to manually drop the connection
    gpio_config_t dtr_conf = {
        .pin_bit_mask = (1ULL << MODEM_DTR_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&dtr_conf);
    ESP_LOGI(TAG, "Toggling DTR to exit Data Mode...");
    gpio_set_level(MODEM_DTR_PIN, 0); // Drop DTR (Hang up)
    vTaskDelay(pdMS_TO_TICKS(2000));
    // gpio_set_level(MODEM_DTR_PIN, 1); // Restore DTR
    // vTaskDelay(pdMS_TO_TICKS(1000));

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

    // 7. Connect to the network (Enters Data Mode)
    ESP_LOGI(TAG, "Connecting to 4G network...");
    esp_err_t err = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect to network: %s", esp_err_to_name(err));
    }
    
    // The system will now wait for IP_EVENT_PPP_GOT_IP in the event loop
}