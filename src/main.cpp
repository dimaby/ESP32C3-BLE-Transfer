#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>

static BLECharacteristic *pCharacteristic;
static std::string lastJson = "";
static unsigned long lastWrite = 0;

#define SERVICE_UUID "12345678-1234-1234-1234-1234567890ab"
#define CHAR_UUID    "abcd1234-5678-90ab-cdef-1234567890ab"

class JsonCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) override {
    std::string value = pChar->getValue();
    if (value.length() > 0) {
      Serial.print("Received JSON: ");
      Serial.println(value.c_str());
      lastJson = value;
      lastWrite = millis();
    }
  }
};

void setup() {
  Serial.begin(115200);
  BLEDevice::init("BLETT");
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
                      CHAR_UUID,
                      BLECharacteristic::PROPERTY_WRITE |
                      BLECharacteristic::PROPERTY_NOTIFY);
  pCharacteristic->setCallbacks(new JsonCallback());
  pService->start();
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(false);
  pAdvertising->setMinPreferred(0x06);  // functions that help with iPhone connections issue
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println("Waiting a client connection to notify...");
}

void loop() {
  if (!lastJson.empty() && millis() - lastWrite > 5000) {
    Serial.println("Sending JSON back...");
    pCharacteristic->setValue(lastJson);
    pCharacteristic->notify();
    lastJson.clear();
  }
  delay(100);
}
