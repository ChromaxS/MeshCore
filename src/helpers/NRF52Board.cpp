#if defined(NRF52_PLATFORM)
#include "NRF52Board.h"

#include <bluefruit.h>

static BLEDfu bledfu;

static void connect_callback(uint16_t conn_handle) {
  (void)conn_handle;
  MESH_DEBUG_PRINTLN("BLE client connected");
}

static void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
  (void)conn_handle;
  (void)reason;

  MESH_DEBUG_PRINTLN("BLE client disconnected");
}

void NRF52Board::begin() {
  startup_reason = BD_STARTUP_NORMAL;
}

void NRF52BoardDCDC::begin() {
  NRF52Board::begin();

  // Enable DC/DC converter for improved power efficiency
  uint8_t sd_enabled = 0;
  sd_softdevice_is_enabled(&sd_enabled);
  if (sd_enabled) {
    sd_power_dcdc_mode_set(NRF_POWER_DCDC_ENABLE);
  } else {
    NRF_POWER->DCDCEN = 1;
  }
}

void NRF52Board::sleep(uint32_t secs) {
  // Clear FPU interrupt flags to avoid insomnia
  // see errata 87 for details https://docs.nordicsemi.com/bundle/errata_nRF52840_Rev3/page/ERR/nRF52840/Rev3/latest/anomaly_840_87.html
  #if (__FPU_USED == 1)
  __set_FPSCR(__get_FPSCR() & ~(0x0000009F)); 
  (void) __get_FPSCR();
  NVIC_ClearPendingIRQ(FPU_IRQn);
  #endif

  // On nRF52, we use event-driven sleep instead of timed sleep
  // The 'secs' parameter is ignored - we wake on any interrupt
  uint8_t sd_enabled = 0;
  sd_softdevice_is_enabled(&sd_enabled);

  if (sd_enabled) {
    // first call processes pending softdevice events, second call sleeps.
    sd_app_evt_wait();
    sd_app_evt_wait();
  } else {
    // softdevice is disabled, use raw WFE
    __SEV();
    __WFE();
    __WFE();
  }
}

// Temperature from NRF52 MCU
float NRF52Board::getMCUTemperature() {
  NRF_TEMP->TASKS_START = 1; // Start temperature measurement

  long startTime = millis();  
  while (NRF_TEMP->EVENTS_DATARDY == 0) { // Wait for completion. Should complete in 50us
    if(millis() - startTime > 5) {  // To wait 5ms just in case
      NRF_TEMP->TASKS_STOP = 1;
      return NAN;
    }
  }
  
  NRF_TEMP->EVENTS_DATARDY = 0; // Clear event flag

  int32_t temp = NRF_TEMP->TEMP; // In 0.25 *C units
  NRF_TEMP->TASKS_STOP = 1;

  return temp * 0.25f; // Convert to *C
}

bool NRF52Board::getBootloaderVersion(char* out, size_t max_len) {
    static const char BOOTLOADER_MARKER[] = "UF2 Bootloader ";
    const uint8_t* flash = (const uint8_t*)0x000FB000; // earliest known info.txt location is 0xFB90B, latest is 0xFCC4B

    for (uint32_t i = 0; i < 0x3000 - (sizeof(BOOTLOADER_MARKER) - 1); i++) {
        if (memcmp(&flash[i], BOOTLOADER_MARKER, sizeof(BOOTLOADER_MARKER) - 1) == 0) {
            const char* ver = (const char*)&flash[i + sizeof(BOOTLOADER_MARKER) - 1];
            size_t len = 0;
            while (len < max_len - 1 && ver[len] != '\0' && ver[len] != ' ' && ver[len] != '\n' && ver[len] != '\r') {
                out[len] = ver[len];
                len++;
            }
            out[len] = '\0';
            return len > 0; // bootloader string is non-empty
        }
    }
    return false;
}

bool NRF52Board::startOTAUpdate(const char *id, char reply[]) {
  // Config the peripheral connection with maximum bandwidth
  // more SRAM required by SoftDevice
  // Note: All config***() function must be called before begin()
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  Bluefruit.configPrphConn(92, BLE_GAP_EVENT_LENGTH_MIN, 16, 16);

  Bluefruit.begin(1, 0);
  // Set max power. Accepted values are: -40, -30, -20, -16, -12, -8, -4, 0, 4
  Bluefruit.setTxPower(4);
  // Set the BLE device name
  Bluefruit.setName(ota_name);

  Bluefruit.Periph.setConnectCallback(connect_callback);
  Bluefruit.Periph.setDisconnectCallback(disconnect_callback);

  // To be consistent OTA DFU should be added first if it exists
  bledfu.begin();

  // Set up and start advertising
  // Advertising packet
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addName();

  /* Start Advertising
    - Enable auto advertising if disconnected
    - Interval:  fast mode = 20 ms, slow mode = 152.5 ms
    - Timeout for fast mode is 30 seconds
    - Start(timeout) with timeout = 0 will advertise forever (until connected)

    For recommended advertising interval
    https://developer.apple.com/library/content/qa/qa1931/_index.html
  */
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244); // in unit of 0.625 ms
  Bluefruit.Advertising.setFastTimeout(30);   // number of seconds in fast mode
  Bluefruit.Advertising.start(0);             // 0 = Don't stop advertising after n seconds

  uint8_t mac_addr[6];
  memset(mac_addr, 0, sizeof(mac_addr));
  Bluefruit.getAddr(mac_addr);
  sprintf(reply, "OK - mac: %02X:%02X:%02X:%02X:%02X:%02X", mac_addr[5], mac_addr[4], mac_addr[3],
          mac_addr[2], mac_addr[1], mac_addr[0]);

  return true;
}
#endif
