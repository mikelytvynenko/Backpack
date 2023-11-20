#include <Arduino.h>

#if defined(PLATFORM_ESP8266)
  #include <espnow.h>
  #include <ESP8266WiFi.h>
#elif defined(PLATFORM_ESP32)
  #include <esp_now.h>
  #include <esp_wifi.h>
  #include <WiFi.h>
#endif

#include "msp.h"
#include "msptypes.h"
#include "logging.h"
#include "config.h"
#include "common.h"
#include "options.h"
#include "helpers.h"

#include "device.h"
#include "devWIFI.h"
#include "devButton.h"
#include "devLED.h"

/////////// DEFINES ///////////

#define BINDING_TIMEOUT     30000
#define NO_BINDING_TIMEOUT  120000
#define BINDING_LED_PAUSE   1000

/////////// GLOBALS ///////////

uint8_t sendAddress[6];

const uint8_t version[] = {LATEST_VERSION};

connectionState_e connectionState = starting;
unsigned long bindingStart = 0;
unsigned long rebootTime = 0;

bool cacheFull = false;
bool sendCached = false;
bool tempUID = false;
bool isBinding = false;

device_t *ui_devices[] = {
#ifdef PIN_LED
  &LED_device,
#endif
#ifdef PIN_BUTTON
  &Button_device,
#endif
  &WIFI_device,
};

/////////// CLASS OBJECTS ///////////

MSP msp;
ELRS_EEPROM eeprom;
TimerBackpackConfig config;
mspPacket_t cachedVTXPacket;
mspPacket_t cachedHTPacket;

/////////// FUNCTION DEFS ///////////

int sendMSPViaEspnow(mspPacket_t *packet);
void resetBootCounter();

/////////////////////////////////////

#if defined(PLATFORM_ESP32)
// This seems to need to be global, as per this page,
// otherwise we get errors about invalid peer:
// https://rntlab.com/question/espnow-peer-interface-is-invalid/
esp_now_peer_info_t peerInfo;
#endif

void RebootIntoWifi()
{
  DBGLN("Rebooting into wifi update mode...");
  config.SetStartWiFiOnBoot(true);
  config.Commit();
  rebootTime = millis();
}

void ProcessMSPPacketFromPeer(mspPacket_t *packet)
{
  if (connectionState == binding)
  {
    DBGLN("Processing Binding Packet...");
    if (packet->function == MSP_ELRS_BIND)
    {
      config.SetGroupAddress(packet->payload);
      DBGLN("MSP_ELRS_BIND MAC = ");
      for (int i = 0; i < 6; i++)
      {
        DBG("%x", packet->payload[i]); // Debug prints
        DBG(",");
      }
      DBG(""); // Extra line for serial output readability
      resetBootCounter();
      connectionState = running;
      rebootTime = millis() + 200; // Add 200ms to allow for any response message(s) to be sent back to device
    }
    return;
  }

  switch (packet->function) {
    case MSP_ELRS_BACKPACK_SET_RECORDING_STATE: {
        mspPacket_t out;
        out.reset();
        out.makeResponse();
        out.function = MSP_ELRS_BACKPACK_SET_RECORDING_STATE;
        msp.sendPacket(packet, &Serial);
    }
  }
}

// espnow on-receive callback
#if defined(PLATFORM_ESP8266)
void OnDataRecv(uint8_t * mac_addr, uint8_t *data, uint8_t data_len)
#elif defined(PLATFORM_ESP32)
void OnDataRecv(const uint8_t * mac_addr, const uint8_t *data, int data_len)
#endif
{
  DBGLN("ESP NOW DATA:");
  for(int i = 0; i < data_len; i++)
  {
    if (msp.processReceivedByte(data[i]))
    {
      // Finished processing a complete packet
      // Only process packets from a bound MAC address
      if (connectionState == binding ||
            (
            firmwareOptions.uid[0] == mac_addr[0] &&
            firmwareOptions.uid[1] == mac_addr[1] &&
            firmwareOptions.uid[2] == mac_addr[2] &&
            firmwareOptions.uid[3] == mac_addr[3] &&
            firmwareOptions.uid[4] == mac_addr[4] &&
            firmwareOptions.uid[5] == mac_addr[5]
            )
          )
      {
        ProcessMSPPacketFromPeer(msp.getReceivedPacket());
      }
      msp.markPacketReceived();
    }
  }
  blinkLED();
}

void SendVersionResponse()
{
  mspPacket_t out;
  out.reset();
  out.makeResponse();
  out.function = MSP_ELRS_BACKPACK_SET_MODE;
  for (size_t i = 0 ; i < sizeof(version) ; i++)
  {
    out.addByte(version[i]);
  }
  msp.sendPacket(&out, &Serial);
}

void SendInProgressResponse()
{
  mspPacket_t out;
    const uint8_t *response = (const uint8_t *)"P";
    out.reset();
    out.makeResponse();
    out.function = MSP_ELRS_GET_BACKPACK_VERSION;
    for (uint32_t i = 0 ; i < 1 ; i++)
    {
        out.addByte(response[i]);
    }
    msp.sendPacket(&out, &Serial);
}

void ProcessMSPPacketFromTimer(mspPacket_t *packet, uint32_t now)
{
  if (connectionState == binding)
  {
    DBGLN("Processing Binding Packet...");
    if (packet->function == MSP_ELRS_BIND)
    {
      config.SetGroupAddress(packet->payload);
      DBGLN("MSP_ELRS_BIND MAC = ");
      for (int i = 0; i < 6; i++)
      {
        DBG("%x", packet->payload[i]); // Debug prints
        DBG(",");
      }
      DBG(""); // Extra line for serial output readability
      resetBootCounter();
      connectionState = running;
      rebootTime = millis() + 200; // Add 200ms to allow for any response message(s) to be sent back to device
    }
    return;
  }

  switch (packet->function)
  {
  case MSP_ELRS_BACKPACK_SET_MODE:
    {
      if (packet->payloadSize == 1)
      {
        if (packet->payload[0] == 'B')
        {
            DBGLN("Enter binding mode...");
            bindingStart =  now;
            connectionState = binding;
            isBinding = true;
        }
        else if (packet->payload[0] == 'W')
        {
            DBGLN("Enter WIFI mode...");
            connectionState = wifiUpdate;
            devicesTriggerEvent();
        }
        SendInProgressResponse();
      }
    }
  case MSP_ELRS_GET_BACKPACK_VERSION:
    DBGLN("Processing MSP_ELRS_GET_BACKPACK_VERSION...");
    SendVersionResponse();
    break;
  case MSP_ELRS_SET_SEND_UID:
  DBGLN("Processing MSP_ELRS_SET_SEND_UID...");
    {
      uint8_t function = packet->readByte();

      // Set target Send address
      if (function == 0x0001)
      {
        uint8_t receivedAddress[6];
        receivedAddress[0] = packet->readByte();
        receivedAddress[1] = packet->readByte();
        receivedAddress[2] = packet->readByte();
        receivedAddress[3] = packet->readByte();
        receivedAddress[4] = packet->readByte();
        receivedAddress[5] = packet->readByte();

        // Register peer address
        memset(&peerInfo, 0, sizeof(peerInfo));
        memcpy(peerInfo.peer_addr, receivedAddress, 6);
        peerInfo.channel = 0;
        peerInfo.encrypt = false;
        esp_now_add_peer(&peerInfo);

        // Set Send address for new target
        memset(&sendAddress, 0, sizeof(sendAddress));
        memcpy(sendAddress, receivedAddress, 6);
      }

      // Return to normal for recieving messages
      else
      {
        // Unregister Peer
        esp_now_del_peer(sendAddress);

        // Set Send address for normal target
        memset(&sendAddress, 0, sizeof(sendAddress));
        memcpy(sendAddress, firmwareOptions.uid, 6);
      }

      // Configure MAC address
      #if defined(PLATFORM_ESP8266)
        wifi_set_macaddr(STATION_IF, sendAddress);
      #elif defined(PLATFORM_ESP32)
        esp_wifi_set_mac(WIFI_IF_STA, sendAddress);
      #endif
      break;
    }
  default:
    // transparently forward MSP packets via espnow to any subscribers
    sendMSPViaEspnow(packet);
    break;
  }
}

int sendMSPViaEspnow(mspPacket_t *packet)
{
  int esp_err = -1;
  uint8_t packetSize = msp.getTotalPacketSize(packet);
  uint8_t nowDataOutput[packetSize];

  uint8_t result = msp.convertToByteArray(packet, nowDataOutput);

  if (!result)
  {
    // packet could not be converted to array, bail out
    return esp_err;
  }

  esp_err = esp_now_send(sendAddress, (uint8_t *) &nowDataOutput, packetSize);

  blinkLED();
  return esp_err;
}

void SetSoftMACAddress()
{
  if (!firmwareOptions.hasUID)
  {
    memcpy(firmwareOptions.uid, config.GetGroupAddress(), 6);
  }
  DBG("EEPROM MAC = ");
  for (int i = 0; i < 6; i++)
  {
    DBG("%x", firmwareOptions.uid[i]); // Debug prints
    DBG(",");
  }
  DBGLN(""); // Extra line for serial output readability

  // MAC address can only be set with unicast, so first byte must be even, not odd
  firmwareOptions.uid[0] = firmwareOptions.uid[0] & ~0x01;

  WiFi.mode(WIFI_STA);
  WiFi.begin("network-name", "pass-to-network", 1);
  WiFi.disconnect();

  // Soft-set the MAC address to the passphrase UID for binding
  #if defined(PLATFORM_ESP8266)
    wifi_set_macaddr(STATION_IF, firmwareOptions.uid);
  #elif defined(PLATFORM_ESP32)
    esp_wifi_set_mac(WIFI_IF_STA, firmwareOptions.uid);
  #endif
}

void resetBootCounter()
{
  config.SetBootCount(0);
  config.Commit();
}

bool BindingExpired(uint32_t now)
{
  return (connectionState == binding) && ((now - bindingStart) > NO_BINDING_TIMEOUT);
}

#if defined(PLATFORM_ESP8266)
// Called from core's user_rf_pre_init() function (which is called by SDK) before setup()
RF_PRE_INIT()
{
    // Set whether the chip will do RF calibration or not when power up.
    // I believe the Arduino core fakes this (byte 114 of phy_init_data.bin)
    // to be 1, but the TX power calibration can pull over 300mA which can
    // lock up receivers built with a underspeced LDO (such as the EP2 "SDG")
    // Option 2 is just VDD33 measurement
    #if defined(RF_CAL_MODE)
    system_phy_set_powerup_option(RF_CAL_MODE);
    #else
    system_phy_set_powerup_option(2);
    #endif
}
#endif

void setup()
{
  #ifdef DEBUG_LOG
    Serial1.begin(115200);
    Serial1.setDebugOutput(true);
  #endif
  Serial.begin(460800);

  options_init();

  eeprom.Begin();
  config.SetStorageProvider(&eeprom);
  config.Load();

  devicesInit(ui_devices, ARRAY_SIZE(ui_devices));

  #ifdef DEBUG_ELRS_WIFI
    config.SetStartWiFiOnBoot(true);
  #endif

  if (config.GetStartWiFiOnBoot())
  {
    config.SetStartWiFiOnBoot(false);
    config.Commit();
    connectionState = wifiUpdate;
    devicesTriggerEvent();
  }
  else
  {
    SetSoftMACAddress();

    if (esp_now_init() != 0)
    {
      DBGLN("Error initializing ESP-NOW");
      rebootTime = millis();
    }

    esp_now_register_recv_cb(OnDataRecv);
  }

  devicesStart();
  if (connectionState == starting)
  {
    connectionState = running;
  }
  DBGLN("Setup completed");
}

void loop()
{
  uint32_t now = millis();

  devicesUpdate(now);

  if (BindingExpired(now))
  {
      connectionState = running;
      isBinding = false;
      DBGLN("Bind timeout");
  }
  if (isBinding && connectionState == running)
  {
      DBGLN("Bind completed");
      isBinding = false;
  }

  #if defined(PLATFORM_ESP8266) || defined(PLATFORM_ESP32)
    // If the reboot time is set and the current time is past the reboot time then reboot.
    if (rebootTime != 0 && now > rebootTime) {
      ESP.restart();
    }
  #endif

  if (connectionState == wifiUpdate)
  {
    return;
  }

  if (BindingExpired(now))
  {
    connectionState = running;
  }

  if (Serial.available())
  {
    uint8_t c = Serial.read();
    if (msp.processReceivedByte(c))
    {
      // Finished processing a complete packet
      ProcessMSPPacketFromTimer(msp.getReceivedPacket(), now);
      msp.markPacketReceived();
    }
  }

}
