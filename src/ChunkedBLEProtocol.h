#ifndef CHUNKED_BLE_PROTOCOL_H
#define CHUNKED_BLE_PROTOCOL_H

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <vector>
#include <string>
#include <functional>

// UUIDs
#define DEFAULT_SERVICE_UUID "5b18eb9b-747f-47da-b7b0-a4e503f9a00f"
#define DEFAULT_CHAR_UUID "8f8b49a2-9117-4e9f-acfc-fda4d0db7408"
#define DEFAULT_CONTROL_CHAR_UUID "12345678-1234-1234-1234-123456789012"

// Control messages
#define CONTROL_NOP 0x00
#define CONTROL_REQUEST 0x01
#define CONTROL_REQUEST_ACK 0x02
#define CONTROL_REQUEST_NAK 0x03
#define CONTROL_DONE 0x04
#define CONTROL_DONE_ACK 0x05
#define CONTROL_DONE_NAK 0x06

class ChunkedBLEProtocol {
public:
    // Callback types (matching original interface)
    typedef std::function<void(const std::string& data)> DataReceivedCallback;
    typedef std::function<void(bool connected)> ConnectionCallback;
    typedef std::function<void(int currentChunk, int totalChunks, bool isReceiving)> ProgressCallback;
    
    // Simple transfer statistics (like in OTA)
    struct TransferStats {
        uint32_t transfersCompleted = 0;
    };
    
private:
    // BLE components
    BLEServer* bleServer;
    BLEService* bleService;
    BLECharacteristic* dataCharacteristic;
    BLECharacteristic* controlCharacteristic;
    
    // Callbacks
    DataReceivedCallback dataReceivedCallback;
    ConnectionCallback connectionCallback;
    ProgressCallback progressCallback;
    
    // Statistics
    TransferStats stats;
     
    // Transfer state (based on OTA algorithm)
    bool receiving;
    uint16_t packet_size;
    uint16_t num_pkgs_received;
    std::vector<uint8_t> receivedData;
    
    // Internal methods
    void handleDataMessage();
    void startReceiving();
    void completeReceiving(bool success);
    void writeReceivedData(const uint8_t* data, size_t length);

public:
    /**
     * Handle control messages (called by BLE callbacks)
     */
    void handleControlMessage();
    
    /**
     * Constructor - Creates complete BLE setup with default UUIDs
     * 
     * @param server BLE server instance (must be created beforehand)
     */
    explicit ChunkedBLEProtocol(BLEServer* server);
    
    /**
     * Constructor - Creates complete BLE setup with custom UUIDs
     * 
     * @param server BLE server instance  
     * @param serviceUUID Custom service UUID
     * @param charUUID Custom characteristic UUID
     */
    ChunkedBLEProtocol(BLEServer* server, const char* serviceUUID, const char* charUUID);
    
    /**
     * Destructor - Clean up resources
     */
    ~ChunkedBLEProtocol();
    
    /**
     * Set callback for complete data reception
     * 
     * @param callback Function called when complete data is received
     */
    void setDataReceivedCallback(DataReceivedCallback callback);
    
    /**
     * Set callback for connection status changes
     * 
     * @param callback Function called when connection status changes
     */
    void setConnectionCallback(ConnectionCallback callback);
    
    /**
     * Set callback for transfer progress updates
     * 
     * @param callback Function called during chunk transfer progress
     *                 Parameters: (currentChunk, totalChunks, isReceiving)
     */
    void setProgressCallback(ProgressCallback callback);
    
    /**
     * Send data using chunked protocol
     * 
     * @param data Data to send (will be automatically chunked)
     * @return true if sent successfully, false otherwise
     */
    bool sendData(const std::string& data);
    
    /**
     * Get transfer statistics
     * 
     * @return Current transfer statistics
     */
    TransferStats getStatistics() const;
    
    /**
     * Reset transfer statistics
     */
    void resetStatistics();
    
    /**
     * Check if transfer is currently in progress
     * 
     * @return true if transfer is active, false otherwise
     */
    bool isTransferInProgress() const;
    
    /**
     * Cancel current transfer (if any)
     * 
     * @param reason Reason for cancellation (for logging)
     */
    void cancelCurrentTransfer(const char* reason = "User requested");
    
    /**
     * Process received chunk (called internally)
     * 
     * @param data Raw chunk data
     * @param length Data length
     */
    void processReceivedChunk(const uint8_t* data, size_t length);
};

#endif // CHUNKED_BLE_PROTOCOL_H