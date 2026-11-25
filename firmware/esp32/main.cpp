#include <Arduino.h>
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <esp_system.h>
#include "servo_frames.h"

static constexpr char service_uuid[] = "4cd118d0-4372-40fe-aee0-0c20a21f0071";
static constexpr char command_uuid[] = "4cd118d1-4372-40fe-aee0-0c20a21f0071";
static constexpr char status_uuid[]  = "4cd118d2-4372-40fe-aee0-0c20a21f0071";

static HardwareSerial stm(1);
static BLECharacteristic *status_characteristic;
static BLESecurity security;
static portMUX_TYPE mailbox_lock = portMUX_INITIALIZER_UNLOCKED;
static bool connected, disconnected, stop_requested;
static int16_t requested_us;
static uint16_t sequence;
static rc_parser_t parser;
static uint32_t passkey;

class Pairing final : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest() override { return passkey; }
    void onPassKeyNotify(uint32_t pin) override {
        Serial.printf("Enter this Bluetooth PIN: %06lu\n", (unsigned long)pin);
    }
    bool onConfirmPIN(uint32_t) override { return false; }
    bool onSecurityRequest() override { return true; }
    void onAuthenticationComplete(esp_ble_auth_cmpl_t result) override {
        Serial.println(result.success ? "BLE paired" : "BLE pairing failed");
    }
};

class Connection final : public BLEServerCallbacks {
    void onConnect(BLEServer *) override {
        portENTER_CRITICAL(&mailbox_lock);
        connected = true;
        portEXIT_CRITICAL(&mailbox_lock);
    }

    void onDisconnect(BLEServer *) override {
        portENTER_CRITICAL(&mailbox_lock);
        connected = false;
        disconnected = true;
        requested_us = 0;
        stop_requested = true;
        portEXIT_CRITICAL(&mailbox_lock);
    }
};

class Command final : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *characteristic) override {
        const uint8_t *bytes = characteristic->getData();
        const size_t size = characteristic->getLength();
        int16_t width = 0;
        const bool stop = size == 4 && memcmp(bytes, "STOP", 4) == 0;
        if (size == 2 && memcmp(bytes, "CW", 2) == 0) width = 1380;
        if (size == 3 && memcmp(bytes, "CCW", 3) == 0) width = 1620;

        portENTER_CRITICAL(&mailbox_lock);
        if (stop) {
            stop_requested = true;
            requested_us = 0;
        } else if (connected && !stop_requested && width && !requested_us) {
            requested_us = width;
        }
        portEXIT_CRITICAL(&mailbox_lock);
    }
};

static Connection connection_callbacks;
static Command command_callbacks;
static Pairing pairing_callbacks;

static void send(uint8_t type, int16_t width = 0)
{
    const rc_frame_t frame{type, ++sequence, width, SERVO_BENCH_TAG};
    uint8_t bytes[RC_FRAME_SIZE];
    rc_encode(bytes, &frame);
    stm.write(bytes, sizeof bytes);
}

void setup()
{
    Serial.begin(115200);  // Coreboard's CH343 USB-to-UART, separate from USART2.
    stm.begin(115200, SERIAL_8N1, 18, 17);
    send(RC_STOP);
    BLEDevice::init("TreeServoBench");
    BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);
    passkey = 100000u + esp_random() % 900000u;
    security.setStaticPIN(passkey);
    security.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
    security.setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    security.setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    BLEDevice::setSecurityCallbacks(&pairing_callbacks);
    Serial.printf("TreeServoBench pairing PIN: %06lu\n", (unsigned long)passkey);

    BLEServer *server = BLEDevice::createServer();
    server->setCallbacks(&connection_callbacks);
    BLEService *service = server->createService(service_uuid);
    BLECharacteristic *commands = service->createCharacteristic(
        command_uuid, BLECharacteristic::PROPERTY_WRITE);
    commands->setAccessPermissions(ESP_GATT_PERM_WRITE_ENC_MITM);
    commands->setCallbacks(&command_callbacks);
    status_characteristic = service->createCharacteristic(
        status_uuid, BLECharacteristic::PROPERTY_READ);
    status_characteristic->setValue("WAIT STM32");
    service->start();
    BLEDevice::getAdvertising()->addServiceUUID(service_uuid);
    BLEDevice::getAdvertising()->setScanResponse(true);
    BLEDevice::startAdvertising();
}

void loop()
{
    static uint32_t last_ping, last_status;
    static bool seen_status;
    static int16_t reported_width;
    static int16_t last_display = -32768;
    const uint32_t now = millis();

    while (stm.available()) {
        rc_frame_t frame;
        if (rc_feed(&parser, (uint8_t)stm.read(), &frame) &&
            frame.type == SERVO_STATUS && frame.right == SERVO_BENCH_TAG) {
            last_status = now;
            seen_status = true;
            reported_width = frame.left;
        }
    }

    portENTER_CRITICAL(&mailbox_lock);
    const bool active = connected;
    const bool restart = disconnected;
    const bool stop = stop_requested;
    const int16_t width = requested_us;
    disconnected = stop_requested = false;
    requested_us = 0;
    portEXIT_CRITICAL(&mailbox_lock);

    if (stop) send(RC_STOP);
    if (restart) {
        seen_status = false;
        BLEDevice::startAdvertising();
    }
    if (active && now - last_ping >= 100u) {
        last_ping = now;
        send(SERVO_PING);
    }

    const bool online = active && seen_status && now - last_status < 280u;
    if (!stop && width && online) send(SERVO_JOG, width);
    const int16_t display = online ? reported_width : -2;
    if (display != last_display) {
        last_display = display;
        if (display == -2) status_characteristic->setValue("WAIT STM32");
        else if (display == -1) status_characteristic->setValue("STOP INPUT");
        else if (display == 0) status_characteristic->setValue("READY");
        else status_characteristic->setValue(String(display).c_str());
    }
    delay(2);
}
