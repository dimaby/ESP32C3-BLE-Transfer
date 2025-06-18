/**
 * @file OTA.cpp
 * @brief Implementation of the Over-The-Air (OTA) update functionality for the ESP32 device using BLE.
 * 
 * This file contains the implementation of BLE characteristics for managing OTA updates, including 
 * handling control commands and writing firmware data. It also includes functions for initiating 
 * the OTA process, writing data, and completing or aborting the update.
 * 
 * This implementation is based on the concepts and code discussed in the article by Michael Angerer:
 * "Over-the-Air Updates for ESP32 with BLE" (https://michaelangerer.dev/esp32/ble/ota/2021/06/01/esp32-ota-part-1.html).
 * 
 * @author Dzmitry Voitsik
 * @date 16.08.2023
 */

#include <Arduino.h>
#include "common.h"
#include "OTA.h"

// Global variables related to OTA
uint8_t otaControlValue; // Stores the current control command value
uint8_t otaDataValue[512]; // Buffer for OTA data packets
esp_ota_handle_t update_handle; // Handle for the OTA update process
const esp_partition_t *update_partition; // Pointer to the partition where the update is stored
uint16_t packet_size = 0; // Size of the incoming data packet
uint16_t num_pkgs_received = 0; // Number of packets received so far
bool updating = false; // Flag indicating if an OTA update is in progress
bool rollback_needed = false; // Flag indicating if a rollback is required
bool isOtaTokenValid = false; // Flag indicating if the OTA token is valid

// BLE characteristics for OTA
BLECharacteristic *otaControlCharacteristic;
BLECharacteristic *otaDataCharacteristic;

/**
 * @class OtaUpdateCallbacks
 * @brief Callback class for handling write events on the OTA control and data characteristics.
 * 
 * This class overrides the onWrite method to handle incoming data and control commands for the OTA process.
 */
class OtaUpdateCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) override {
        // Update client activity timestamp on any OTA interaction
        OtaClientActivityCallback callback = getOtaClientActivityCallback();
        if (callback != nullptr) {
            callback();
        }
        
        if (pCharacteristic == otaControlCharacteristic) {
            handleOtaControl(); // Handle OTA control commands
        } else if (pCharacteristic == otaDataCharacteristic) {
            handleOtaData(); // Handle incoming OTA data packets
        }
    }
};

/**
 * @brief Initializes the OTA service by setting up BLE characteristics for control and data.
 * 
 * @param server A pointer to the BLEServer object to which the OTA service will be added.
 */
void initOtaService(BLEServer *server) {
    // Service: OTA_SERVICE - handles OTA (Over-The-Air) updates
    BLEService *otaService = server->createService(OTA_SERVICE_UUID);

    // Create characteristic for OTA control, used to manage OTA updates
    otaControlCharacteristic = otaService->createCharacteristic(OTA_CONTROL_CHAR_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY);
    otaControlCharacteristic->setCallbacks(new OtaUpdateCallbacks());
    otaControlCharacteristic->addDescriptor(new BLE2902());
    // otaControlCharacteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED);
    const uint8_t initial_value = 0;
    otaControlCharacteristic->setValue((uint8_t*)&initial_value, 1);

    // Create characteristic for OTA data, used to send the firmware data during OTA updates
    otaDataCharacteristic = otaService->createCharacteristic(OTA_DATA_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE);
    otaDataCharacteristic->setCallbacks(new OtaUpdateCallbacks());
    // otaDataCharacteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED);

    // Start the OTA_SERVICE service
    otaService->start();
}

/**
 * @brief Handles incoming OTA control commands.
 * 
 * This function processes control commands received via BLE and manages the OTA process accordingly.
 */
void handleOtaControl() {
    std::string receivedData = otaControlCharacteristic->getValue();
    uint8_t otaControlValue = receivedData.c_str()[0];

    // Check the token if it hasn't been validated yet
    if (!isOtaTokenValid) {
        String receivedToken = receivedData.c_str(); 

        LOG_INFO("Received token: %s", receivedToken.c_str());
        LOG_INFO("Expected token: %s", authToken.c_str()); 

        if (authToken.isEmpty()) {
            otaControlValue = OTA_TOKEN_INVALID;
            LOG_ERROR("authToken is empty or not initialized!");
        } else if (receivedToken == authToken) {
            isOtaTokenValid = true;
            otaControlValue = OTA_TOKEN_VALID;
            LOG_INFO("Valid OTA Token received");
        } else {
            otaControlValue = OTA_TOKEN_INVALID;
            LOG_ERROR("Invalid OTA Token received");
        }        

        // Notify the client about the token verification result
        otaControlCharacteristic->setValue((uint8_t*)&otaControlValue, 1);
        otaControlCharacteristic->notify();

        // After sending the token verification result, return and wait for the next command
        return;
    }

    // Proceed with OTA commands if the token has already been validated
    switch (otaControlValue) {
        case SVR_CHR_OTA_CONTROL_REQUEST:
            LOG_INFO("OTA Request received");
            startOtaUpdate(); // Start the OTA update process
            break;
        case SVR_CHR_OTA_CONTROL_DONE:
            LOG_INFO("OTA Done received");
            completeOtaUpdate(true); // Complete the OTA update process
            isOtaTokenValid = false; // Reset the token validation after OTA is done
            break;
        default:
            LOG_ERROR("Unknown OTA control value received");
            break;
    }
}

/**
 * @brief Handles incoming OTA data packets.
 * 
 * This function writes the received data packets to the OTA partition during an ongoing OTA update.
 */
void handleOtaData() {
    std::string value = otaDataCharacteristic->getValue();
    memcpy(otaDataValue, value.data(), value.length());
    writeOtaData(value.length());
}

/**
 * @brief Starts the OTA update process.
 * 
 * This function initializes the OTA update process by setting up the partition and handling the first packet.
 */
void startOtaUpdate() {
    update_partition = esp_ota_get_next_update_partition(NULL);
    if (esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle) != ESP_OK) {
        LOG_ERROR("esp_ota_begin failed");
        esp_ota_abort(update_handle);
        otaControlValue = SVR_CHR_OTA_CONTROL_REQUEST_NAK; // NAK
        rollback_needed = true;
    } else {
        otaControlValue = SVR_CHR_OTA_CONTROL_REQUEST_ACK; // ACK
        updating = true;
        packet_size = (otaDataValue[1] << 8) + otaDataValue[0];
        LOG_INFO("Packet size is: %d", packet_size);
        num_pkgs_received = 0;
        rollback_needed = false;
    }

    otaControlCharacteristic->setValue((uint8_t*)&otaControlValue, 1);
    otaControlCharacteristic->notify();
}

/**
 * @brief Completes the OTA update process.
 * 
 * This function finalizes the OTA update by either committing the update or rolling back if there was a failure.
 * 
 * @param success A boolean value indicating whether the update was successful.
 */
void completeOtaUpdate(bool success) {
    updating = false;
    if (success) {
        if (esp_ota_end(update_handle) != ESP_OK) {
            LOG_ERROR("esp_ota_end failed");
            otaControlValue = SVR_CHR_OTA_CONTROL_DONE_NAK; // DONE_NAK
        } else {
            if (esp_ota_set_boot_partition(update_partition) != ESP_OK) {
                LOG_ERROR("esp_ota_set_boot_partition failed");
                otaControlValue = SVR_CHR_OTA_CONTROL_DONE_NAK; // DONE_NAK
            } else {
                otaControlValue = SVR_CHR_OTA_CONTROL_DONE_ACK; // DONE_ACK
            }
        }

        otaControlCharacteristic->setValue((uint8_t*)&otaControlValue, 1);
        otaControlCharacteristic->notify();

        if (otaControlValue == SVR_CHR_OTA_CONTROL_DONE_ACK) { // DONE_ACK
            LOG_INFO("Preparing to restart!");
            delay(500);
            esp_restart();
        }
    } else {
        esp_ota_abort(update_handle);
        rollback_needed = true;
    }
}

/**
 * @brief Writes OTA data packets to the partition.
 * 
 * This function handles the actual writing of firmware data to the OTA partition during the update process.
 * 
 * @param length The length of the data to be written.
 */
void writeOtaData(int length) {
    if (updating) {
        if (esp_ota_write(update_handle, (const void *)otaDataValue, length) != ESP_OK) {
            LOG_ERROR("esp_ota_write failed");
            rollback_needed = true;
            return;
        }

        num_pkgs_received++;
        LOG_INFO("Received packet %d", num_pkgs_received);
    }
}

/**
 * @brief Runs diagnostics on the updated firmware.
 * 
 * This function can be used to perform checks on the new firmware before marking it as valid.
 * 
 * @return True if diagnostics pass, otherwise false.
 */
bool run_diagnostics() {
    return true;
}

/**
 * @brief Checks the current OTA state and handles any necessary rollbacks.
 * 
 * This function checks the OTA partition for any pending updates and decides whether to proceed or roll back.
 */
void checkOtaState() {
    const esp_partition_t *partition = esp_ota_get_running_partition();
    switch (partition->address) {
        case 0x10000:
            LOG_INFO("Running partition: ota_0");
            break;
        case 0x1E0000:
            LOG_INFO("Running partition: ota_1");
            break;
        default:
            LOG_ERROR("Running partition: unknown");
            break;
    }

    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(partition, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            LOG_INFO("An OTA update has been detected.");
            if (run_diagnostics()) {
                LOG_INFO("Diagnostics completed successfully! Continuing execution.");
                esp_ota_mark_app_valid_cancel_rollback();
            } else {
                LOG_ERROR("Diagnostics failed! Start rollback to the previous version.");
                esp_ota_mark_app_invalid_rollback_and_reboot();
            }
        }
    }
}