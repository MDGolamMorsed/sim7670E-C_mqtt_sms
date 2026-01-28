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

// Fallback definitions to allow runtime switching even if disabled in menuconfig
#ifndef CONFIG_SIM7670_MQTT_BROKER_URL
#define CONFIG_SIM7670_MQTT_BROKER_URL "mqtt://broker.hivemq.com"
#endif
#ifndef CONFIG_SIM7670_MQTT_USERNAME
#define CONFIG_SIM7670_MQTT_USERNAME ""
#endif
#ifndef CONFIG_SIM7670_MQTT_PASSWORD
#define CONFIG_SIM7670_MQTT_PASSWORD ""
#endif
#ifndef CONFIG_SIM7670_MQTT_TOPIC
#define CONFIG_SIM7670_MQTT_TOPIC "test/topic"
#endif
#ifndef CONFIG_TARGET_PHONE_NUMBER
#define CONFIG_TARGET_PHONE_NUMBER "+8801521475412"
#endif

typedef enum {
    MODE_SMS,
    MODE_MQTT
} app_mode_t;

// Default mode based on config, but can be switched at runtime
static esp_mqtt_client_handle_t mqtt_client = NULL;
static volatile bool s_ppp_connected = false;
#ifdef CONFIG_SIM7670_MQTT_ENABLE
static volatile app_mode_t s_current_mode = MODE_MQTT;
#else
static volatile app_mode_t s_current_mode = MODE_SMS;
#endif

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

        // Check for mode switch command #sms#
        if (event->data_len > 0) {
            char *payload = malloc(event->data_len + 1);
            if (payload) {
                memcpy(payload, event->data, event->data_len);
                payload[event->data_len] = '\0';
                if (strstr(payload, "#sms#")) {
                    ESP_LOGI(TAG, "Command received: Switching to SMS Mode");
                    s_current_mode = MODE_SMS;
                }
                free(payload);
            }
        }
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
        s_ppp_connected = true;
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Modem Connected. Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

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
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        s_ppp_connected = false;
        ESP_LOGW(TAG, "Modem Lost IP");
        if (mqtt_client) {
            esp_mqtt_client_stop(mqtt_client);
            esp_mqtt_client_destroy(mqtt_client);
            mqtt_client = NULL;
        }
    }
}

// --- SMS Sending Function ---
static esp_err_t send_sms(esp_modem_dce_t *dce, const char *phone_number, const char *message) {
    if (!dce || !phone_number || !message || strlen(phone_number) == 0) {
        ESP_LOGE(TAG, "send_sms: Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "Attempting to send SMS to %s", phone_number);
 
    // Use the public API function to send SMS.
    // This is more robust and abstracts away the low-level AT command sequence.
    esp_err_t err = esp_modem_send_sms(dce, phone_number, message);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send SMS: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "SMS sent successfully.");
    }
    return err;
}

// --- SMS Parsing Function ---
static void handle_sms_content(esp_modem_dce_t *dce, const char *sms_text) {
    int dht_h, dht_l, temp_h, temp_l;
    // Check for the specific format: #dht:H22,L20;temp:H23,L15;#
    // We look for the start of the pattern
    const char *pattern_start = strstr(sms_text, "#dht:");

    // Check for mode switch command #mqtt#
    if (strstr(sms_text, "#mqtt#")) {
        ESP_LOGI(TAG, "Command received: Switching to MQTT Mode");
        s_current_mode = MODE_MQTT;
        return; // Command handled, no need to parse further
    }
    
    if (pattern_start) {
        if (sscanf(pattern_start, "#dht:H%d,L%d;temp:H%d,L%d;#", &dht_h, &dht_l, &temp_h, &temp_l) == 4) {
            ESP_LOGI(TAG, "SMS DECODE: dht sensor high threshold is %d and low threshold is %d. same as temp sensor (H%d, L%d)",
                     dht_h, dht_l, temp_h, temp_l);
            
            // Send a success response to the pre-configured phone number
            if (strlen(CONFIG_TARGET_PHONE_NUMBER) > 0) {
                send_sms(dce, CONFIG_TARGET_PHONE_NUMBER, "Success: DHT & Temp thresholds received.");
            } else {
                ESP_LOGW(TAG, "Target phone number not configured in menuconfig. Cannot send success SMS.");
            }
        } else {
            ESP_LOGW(TAG, "SMS matched prefix but failed to parse values: %s", pattern_start);
        }
    } else {
        // Only log as raw if it wasn't a command or a known pattern
        ESP_LOGI(TAG, "Received SMS (Raw): %s", sms_text);
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
    dte_config.uart_config.baud_rate = CONFIG_SIM7670_BAUD_RATE;
    dte_config.uart_config.tx_io_num = CONFIG_SIM7670_TX_PIN;
    dte_config.uart_config.rx_io_num = CONFIG_SIM7670_RX_PIN;
    dte_config.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE; 
    dte_config.uart_config.rx_buffer_size = 4096; // Increase RX buffer to prevent "Ring Buffer Full"
    dte_config.task_stack_size = 4096; // Increase stack for modem tasks
    dte_config.task_priority = 5;

    // 4. Configure Modem DCE (Device Configuration)
    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(CONFIG_SIM7670_APN);

    // 5. Create the Network Interface (PPP)
    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    esp_netif_t *esp_netif = esp_netif_new(&netif_ppp_config);
    
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

    // --- Main State Machine Loop ---
    while (1) {
        if (s_current_mode == MODE_MQTT) {
            ESP_LOGI(TAG, "Entering MQTT Mode...");
            
            // Enter Data Mode (PPPoS)
            esp_err_t err = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set data mode: %s", esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }

            // Wait loop: Stay in MQTT mode until flag changes
            while (s_current_mode == MODE_MQTT) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            // Cleanup before switching to SMS
            ESP_LOGI(TAG, "Stopping MQTT to switch modes...");
            if (mqtt_client) {
                esp_mqtt_client_stop(mqtt_client);
                esp_mqtt_client_destroy(mqtt_client);
                mqtt_client = NULL;
            }

            // Force Command Mode (this will drop PPP and trigger LOST_IP)
            esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
            // vTaskDelay(pdMS_TO_TICKS(5000)); // Removed hard delay, relying on event flag

        } else { // s_current_mode == MODE_SMS
#if CONFIG_SIM7670_SMS_ENABLE
            ESP_LOGI(TAG, "Entering SMS Mode...");
            
            // Wait for PPP to fully disconnect if it was active
            while (s_ppp_connected) {
                ESP_LOGI(TAG, "Waiting for PPP disconnection...");
                vTaskDelay(pdMS_TO_TICKS(100)); // Check more frequently
            }
            
            // Give the modem a moment to settle after dropping the IP
            vTaskDelay(pdMS_TO_TICKS(200)); // Reduced stabilization time

            // Ensure Command Mode
            esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);

            // Sync with modem to ensure it's ready for AT commands
            int retry = 0;
            while (esp_modem_sync(dce) != ESP_OK && retry < 5) {
                ESP_LOGW(TAG, "Modem not responding to AT... retrying (%d/5)", retry + 1);
                vTaskDelay(pdMS_TO_TICKS(1000));
                retry++;
            }

            esp_modem_at(dce, "ATE0", NULL, 1000); // Disable command echo
            
            // Retry SMS configuration to ensure modem is in Text Mode
            // If this fails, AT+CMGL="ALL" will likely fail or return PDU garbage
            for (int i = 0; i < 3; i++) {
                if (esp_modem_at(dce, "AT+CMGF=1", NULL, 1000) == ESP_OK &&
                    esp_modem_at(dce, "AT+CPMS=\"SM\",\"SM\",\"SM\"", NULL, 1000) == ESP_OK &&
                    esp_modem_at(dce, "AT+CNMI=2,1", NULL, 1000) == ESP_OK) {
                    break;
                }
                ESP_LOGW(TAG, "SMS config failed, retrying...");
                vTaskDelay(pdMS_TO_TICKS(1000));
            }

            // Clear any old messages that may have arrived during MQTT mode
            ESP_LOGI(TAG, "Clearing SMS inbox before polling...");
            esp_modem_at(dce, "AT+CMGD=1,4", NULL, 5000);

            char *sms_buffer = (char *)malloc(4096); // Increase buffer size for large SMS lists
            if (sms_buffer) {
                // Polling loop: Stay in SMS mode until flag changes
                while (s_current_mode == MODE_SMS) {
                    memset(sms_buffer, 0, 4096);
                    // Use "ALL" because "REC UNREAD" changes status to READ immediately.
                    // If the ESP32 misses the response once, it won't see it again.
                    if (esp_modem_at(dce, "AT+CMGL=\"ALL\"", sms_buffer, 5000) == ESP_OK) {
                        if (strlen(sms_buffer) > 5) {
                            ESP_LOGI(TAG, "SMS Buffer: %s", sms_buffer);
                        }
                        // Check for +CMGL: OR specific commands directly.
                        // This handles cases where the header might be lost due to buffer overflow.
                        if (strstr(sms_buffer, "+CMGL:") || strstr(sms_buffer, "#mqtt#") || strstr(sms_buffer, "#dht:")) {
                            handle_sms_content(dce, sms_buffer);
                            // Delete all messages to prevent repeated processing
                            // We delete all messages to ensure the inbox is clean for the next command
                            ESP_LOGI(TAG, "Deleting processed SMS...");
                            bool deleted = false;
                            for (int i = 0; i < 3; i++) {
                                if (esp_modem_at(dce, "AT+CMGD=1,4", NULL, 5000) == ESP_OK) {
                                    ESP_LOGI(TAG, "SMS deleted successfully.");
                                    deleted = true;
                                    break;
                                }
                                ESP_LOGW(TAG, "Delete failed, retrying (%d/3)...", i + 1);
                                vTaskDelay(pdMS_TO_TICKS(1000));
                            }
                            if (!deleted) {
                                ESP_LOGE(TAG, "Failed to delete SMS. Inbox may be full.");
                            }
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(5000));
                }
                free(sms_buffer);
            }
#else
            ESP_LOGW(TAG, "SMS mode requested, but SMS is disabled in menuconfig. Reverting to MQTT mode.");
            s_current_mode = MODE_MQTT;
            vTaskDelay(pdMS_TO_TICKS(2000)); // Prevent busy-looping
#endif
        }
    }
}