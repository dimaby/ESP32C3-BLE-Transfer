/**
 * @file OTA.h
 * @brief Header file for the Over-The-Air (OTA) update functionality for the ESP32 device using BLE.
 * 
 * This header file contains declarations of functions and variables used for managing OTA updates via BLE.
 * It includes functionalities such as initializing the OTA service, handling control commands, writing firmware data,
 * and checking the OTA state.
 * 
 * This implementation is based on the concepts and code discussed in the article by Michael Angerer:
 * "Over-the-Air Updates for ESP32 with BLE" (https://michaelangerer.dev/esp32/ble/ota/2021/06/01/esp32-ota-part-1.html).
 * 
 * @note Make sure to manage the OTA process carefully to avoid issues during firmware updates.
 * 
 * @warning Improper handling of OTA updates can result in a non-functional device. Ensure all processes complete successfully.
 * 
 * @see BLEDevice, BLEServer, BLEService
 * 
 * @author Dzmitry Voitsik
 * @date 16.08.2023
 */

#ifndef OTA_H
#define OTA_H

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <nvs.h>
#include <nvs_flash.h>

// **
// @brief Definition of UUIDs and values for OTA control
// **
// UUID for the OTA service
#define OTA_SERVICE_UUID "d6f1d96d-594c-4c53-b1c6-244a1dfde6d8"

// **
// @brief UUIDs for OTA control and data characteristics
// **
#define OTA_CONTROL_CHAR_UUID "7ad671aa-21c0-46a4-b722-270e3ae3d830"
#define OTA_DATA_CHAR_UUID "23408888-1f40-4cd8-9b89-ca8d45f8a5b0"

// **
// @brief Control codes for OTA process management
// **
#define SVR_CHR_OTA_CONTROL_NOP 0x00
#define SVR_CHR_OTA_CONTROL_REQUEST 0x01
#define SVR_CHR_OTA_CONTROL_REQUEST_ACK 0x02
#define SVR_CHR_OTA_CONTROL_REQUEST_NAK 0x03
#define SVR_CHR_OTA_CONTROL_DONE 0x04
#define SVR_CHR_OTA_CONTROL_DONE_ACK 0x05
#define SVR_CHR_OTA_CONTROL_DONE_NAK 0x06

// **
// @brief Token validation results
// **
#define OTA_TOKEN_VALID 0x07
#define OTA_TOKEN_INVALID 0x08

// **
// @brief Type definition for OTA client activity update function
// **
typedef void (*OtaClientActivityCallback)();

// **
// @brief Set the OTA client activity callback
// @param callback The callback function to be called when the client activity changes
// **
void setOtaClientActivityCallback(OtaClientActivityCallback callback);

// **
// @brief Get the OTA client activity callback
// @return The callback function currently set for client activity updates
// **
OtaClientActivityCallback getOtaClientActivityCallback();

// **
// @brief Declaration of global variables
// **
extern uint8_t otaControlValue;  ///< Stores the current control value for OTA
extern uint8_t otaDataValue[512];  ///< Buffer for holding OTA data packets
extern esp_ota_handle_t update_handle;  ///< Handle for the ongoing OTA update process
extern const esp_partition_t *update_partition;  ///< Pointer to the partition where the OTA update is stored
extern uint16_t packet_size;  ///< Size of the data packet being sent during OTA
extern uint16_t num_pkgs_received;  ///< Counter for the number of packets received during OTA
extern bool updating;  ///< Flag to indicate if an OTA update is in progress
extern bool rollback_needed;  ///< Flag to indicate if a rollback is needed in case of failure
extern bool isOtaTokenValid;  ///< Flag to indicate if the OTA token is valid
extern BLECharacteristic *otaControlCharacteristic;  ///< BLE characteristic for OTA control
extern BLECharacteristic *otaDataCharacteristic;  ///< BLE characteristic for OTA data

/**
 * @brief Initializes the OTA service and characteristics on the BLE server.
 * 
 * This function sets up the OTA service, defining the characteristics for control and data, and starts the service.
 * 
 * @param server A pointer to the BLEServer object where the OTA service will be initialized.
 */
void initOtaService(BLEServer *server);

/**
 * @brief Handles incoming OTA control commands from the BLE client.
 * 
 * This function processes control commands received over the OTA control characteristic and manages the OTA process accordingly.
 */
void handleOtaControl();

/**
 * @brief Handles incoming OTA data packets from the BLE client.
 * 
 * This function processes data packets received over the OTA data characteristic and writes them to the appropriate partition.
 */
void handleOtaData();

/**
 * @brief Initiates the OTA update process.
 * 
 * This function begins the OTA update by preparing the partition and handling the first data packet.
 */
void startOtaUpdate();

/**
 * @brief Completes the OTA update process.
 * 
 * This function finalizes the OTA update, marking it as complete and committing the changes or rolling back in case of failure.
 * 
 * @param success A boolean indicating whether the OTA update was successful.
 */
void completeOtaUpdate(bool success);

/**
 * @brief Writes a data packet to the OTA partition during the update.
 * 
 * This function writes the incoming data packets to the OTA partition.
 * 
 * @param length The length of the data packet being written.
 */
void writeOtaData(int length);

/**
 * @brief Checks the current OTA state and manages rollbacks if necessary.
 * 
 * This function checks the state of the OTA partition and performs diagnostics to determine if a rollback is needed.
 */
void checkOtaState();

/**
 * @brief Runs diagnostics on the updated firmware.
 * 
 * This function performs diagnostic checks on the firmware after the update to ensure it is functioning correctly.
 * 
 * @return true if diagnostics pass, otherwise false.
 */
bool run_diagnostics();

#endif // OTA_H
