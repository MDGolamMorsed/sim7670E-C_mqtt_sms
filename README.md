# ESP32 & SIM7670 Dual-Mode (MQTT/SMS) Gateway

This project turns an ESP32 and a SIM7670 cellular modem into a versatile gateway that can operate in two distinct modes: **MQTT Mode** and **SMS Mode**. It allows for dynamic, remote switching between these modes, making it ideal for IoT applications that require both high-frequency data reporting and low-power command-and-control.

## Features

- **Dual-Mode Operation**:
  - **MQTT Mode**: Connects to the cellular network using PPPoS to establish a full TCP/IP connection. It then connects to an MQTT broker to send and receive data in real-time.
  - **SMS Mode**: Operates in a low-power state, polling for incoming SMS messages to receive commands or configuration updates.

- **Remote Runtime Switching**:
  - Switch from **SMS to MQTT** by sending an SMS containing `#mqtt#`.
  - Switch from **MQTT to SMS** by publishing a message containing `#sms#` to the subscribed MQTT topic.

- **Robust Modem Handling**:
  - Gracefully handles the transition between the modem's Command Mode (for SMS) and Data Mode (for PPPoS/MQTT).
  - Includes retry logic for modem initialization and configuration to ensure reliable operation.
  - Automatically clears the SMS inbox upon entering SMS mode to prevent processing stale commands.
  - Verifies that SMS messages are successfully deleted after being processed.

- **Data Parsing**:
  - In SMS mode, it can parse structured data from messages (e.g., `#dht:H22,L20;temp:H23,L15;#`).

- **Flexible Configuration**:
  - Primarily configured via `idf.py menuconfig`.
  - Includes fallback definitions, allowing runtime mode switching even if a mode is disabled as the default startup option.

## Hardware Requirements

- An ESP32 development board.
- A SIM7670 series cellular modem.
- A SIM card with an active data and/or SMS plan.
- Jumper wires for connections.

## Pin Mapping (Wiring)

Connect the ESP32 to the SIM7670 modem as follows. The GPIO pins on the ESP32 are configurable via `menuconfig`.

| ESP32 Pin                  | SIM7670 Pin | Description                  |
| -------------------------- | ----------- | ---------------------------- |
| `CONFIG_SIM7670_TX_PIN`    | `RXD`       | UART Transmit                |
| `CONFIG_SIM7670_RX_PIN`    | `TXD`       | UART Receive                 |
| `3V3`                      | `VCC`       | Power for the modem          |
| `GND`                      | `GND`       | Common Ground                |

## Configuration

All project settings are configured using the ESP-IDF `menuconfig` system.

1.  Open the project configuration menu:
    ```shell
    idf.py menuconfig
    ```

2.  Navigate to `Component config` ---> `ESP Modem` and configure the following:
    - **`ESP Modem DTE Baud Rate`**: Set the baud rate for your modem (e.g., `115200`).
    - **`ESP Modem DTE TX Pin`**: Set the GPIO pin connected to the modem's `RXD`.
    - **`ESP Modem DTE RX Pin`**: Set the GPIO pin connected to the modem's `TXD`.

3.  Navigate to `Example Connection Configuration` and set:
    - **`Modem APN`**: The Access Point Name for your SIM card's mobile network (e.g., "internet").

4.  Navigate to `SIM7670 Project Configuration` (or your custom menu) to set:
    - **`Enable MQTT Mode on Startup`**:
      - If checked (`CONFIG_SIM7670_MQTT_ENABLE=y`), the device will start in **MQTT Mode**.
      - If unchecked, the device will start in **SMS Mode**.
    - **`Enable SMS Mode`**: Ensure this is checked (`CONFIG_SIM7670_SMS_ENABLE=y`) to compile the SMS logic.
    - **(Optional)** If you have MQTT enabled, you can configure the broker details here:
      - `MQTT Broker URL`
      - `MQTT Username`
      - `MQTT Password`
      - `MQTT Topic`

5.  Save the configuration and exit `menuconfig`.

## How to Use

1.  **Build and Flash** the project to your ESP32:
    ```shell
    idf.py build
    idf.py flash monitor
    ```

2.  The device will start in the mode you selected in `menuconfig`.

3.  **Switching Modes**:
    - **If in SMS Mode**: Send a text message to the modem's phone number with the exact content:
      ```
      #mqtt#
      ```
      The device will log the command, switch to MQTT mode, and connect to the broker.

    - **If in MQTT Mode**: Use an MQTT client to publish a message to the topic `test/topic` (or your configured topic) with the exact payload:
      ```
      #sms#
      ```
      The device will disconnect from the MQTT broker, drop the cellular data connection, and enter SMS polling mode.

4.  **Sending Data via SMS**:
    - To send sensor threshold data, use the following format in an SMS:
      ```
      #dht:H22,L20;temp:H23,L15;#
      ```
      The ESP32 will parse and log these values.