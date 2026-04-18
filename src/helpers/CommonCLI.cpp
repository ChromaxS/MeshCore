#include <Arduino.h>
#include <ArduinoJson.h>
#include "CommonCLI.h"
#include "TxtDataHelpers.h"
#include "AdvertDataHelpers.h"
#include <RTClib.h>
#include <malloc.h>
#ifdef ESP_PLATFORM
#include <WiFi.h>
#include <esp_wifi.h>
#endif

#define REPLY_SILENT_RUNNING_ON   "Silent Running (ignores discovery requests and sends no advertisements)"
#define REPLY_SILENT_RUNNING_OFF  "Regular Running"

static int gl_allow_protected_over_remote = 0;

// Helper function to calculate total size of MQTT fields for file format compatibility
// Uses NodePrefs struct to get accurate field sizes
static size_t getMQTTFieldsSize(const NodePrefs* prefs) {
  return sizeof(prefs->mqtt_origin) + sizeof(prefs->mqtt_iata) +
         sizeof(prefs->mqtt_status_enabled) + sizeof(prefs->mqtt_packets_enabled) +
         sizeof(prefs->mqtt_raw_enabled) + sizeof(prefs->mqtt_tx_enabled) +
         sizeof(prefs->mqtt_status_interval) + sizeof(prefs->wifi_ssid) +
         sizeof(prefs->wifi_password) + sizeof(prefs->timezone_string) +
         sizeof(prefs->timezone_offset) + sizeof(prefs->mqtt_server) +
         sizeof(prefs->mqtt_port) + sizeof(prefs->mqtt_username) +
         sizeof(prefs->mqtt_password) + sizeof(prefs->mqtt_analyzer_us_enabled) +
         sizeof(prefs->mqtt_analyzer_eu_enabled) + sizeof(prefs->mqtt_owner_public_key) +
         sizeof(prefs->mqtt_email);
}

#ifdef WITH_MQTT_BRIDGE
#include "bridges/MQTTBridge.h"

#endif

// Believe it or not, this std C function is busted on some platforms!
static uint32_t _atoi(const char* sp) {
  uint32_t n = 0;
  while (*sp && *sp >= '0' && *sp <= '9') {
    n *= 10;
    n += (*sp++ - '0');
  }
  return n;
}

uint32_t getAllocatedHeap() {
  struct mallinfo mi = mallinfo();
  return (uint32_t)mi.uordblks;
}

#ifdef ESP_PLATFORM

uint32_t getFreeHeap() {
  return ESP.getFreeHeap();
}

#else

uint32_t getFreeHeap() {
  struct mallinfo mi = mallinfo();
  return (uint32_t)mi.fordblks;
}

#endif

// Validate public key hex string (PUB_KEY_SIZE * 2 hex characters = PUB_KEY_SIZE bytes)
static bool isValidPublicKeyHex(const char* key_hex) {
  if (!key_hex) {
    return false;
  }
  int key_len = strlen(key_hex);
  if (key_len != PUB_KEY_SIZE * 2) {
    return false;
  }
  // Validate hex characters
  for (int i = 0; i < key_len; i++) {
    if (!((key_hex[i] >= '0' && key_hex[i] <= '9') ||
          (key_hex[i] >= 'A' && key_hex[i] <= 'F') ||
          (key_hex[i] >= 'a' && key_hex[i] <= 'f'))) {
      return false;
    }
  }
  return true;
}

static bool isValidName(const char *n) {
  while (*n) {
    if (*n == '[' || *n == ']' || *n == '\\' || *n == ':' || *n == ',' || *n == '?' || *n == '*') return false;
    n++;
  }
  return true;
}

void CommonCLI::loadPrefs(FILESYSTEM* fs) {
  bool loaded_from_json = false;
  bool save_config = false;
  if (fs->exists("/prefs.json")) {
    loadPrefsJson(fs);
    loaded_from_json = true;
  }
  if (fs->exists("/com_prefs")) {
    if (!loaded_from_json) {
        MESH_DEBUG_PRINTLN("Migrating from configuration file, /com_prefs, to: /prefs.json");

        // new filename //
        loadPrefsInt(fs, "/com_prefs");
        save_config = true;
    } else {
        MESH_DEBUG_PRINTLN("Deleting old configuration file: /com_prefs");
    }
    // remove old //
    fs->remove("/com_prefs");
  }
  if (fs->exists("/node_prefs")) {
    if (!loaded_from_json) {
        MESH_DEBUG_PRINTLN("Migrating from configuration file, /node_prefs, to: /prefs.json");

        // old filename //
        loadPrefsInt(fs, "/node_prefs");
        save_config = true;
    } else {
        MESH_DEBUG_PRINTLN("Deleting old configuration file: /node_prefs");
    }
    // remove old //
    fs->remove("/node_prefs");
  }
  if (!loaded_from_json and !save_config) {
    // file doesn't exist //
    MESH_DEBUG_PRINTLN("No configuration file... setting defaults.");
    // set default settings //
    setPrefsDefaults();
#ifdef WITH_MQTT_BRIDGE
    setMQTTPrefsDefaults();
#endif
    // set default bridge settings for fresh installs //
    _prefs->bridge_pkt_src = 1;  // Default to RX (logRx) for new installs
    save_config = true;
  }
#ifdef WITH_MQTT_BRIDGE
  if (!loaded_from_json) {
    // Load MQTT preferences from separate file
    loadMQTTPrefs(fs);
  }

  // remove old
  if (fs->exists("/mqtt_prefs.json")) {
    fs->remove("/mqtt_prefs.json");
    save_config = !loaded_from_json;
  }
  if (fs->exists("/mqtt_prefs")) {
    fs->remove("/mqtt_prefs");
    save_config = !loaded_from_json;
  }
  if (fs->exists("/com_prefs")) {
    fs->remove("/com_prefs");
    save_config = !loaded_from_json;
  }
#endif

  sanitizePrefs();

  if (save_config) {
    MESH_DEBUG_PRINTLN("Finishing migration to: /prefs.json");
    savePrefs(fs);  // save to new filename
  }
}

void CommonCLI::loadPrefsInt(FILESYSTEM* fs, const char* filename) {
#if defined(RP2040_PLATFORM)
  File file = fs->open(filename, "r");
#else
  File file = fs->open(filename);
#endif
  if (file) {
    uint8_t pad[8];

    file.read((uint8_t *)&_prefs->airtime_factor, sizeof(_prefs->airtime_factor));    // 0
    file.read((uint8_t *)&_prefs->node_name, sizeof(_prefs->node_name));              // 4
    file.read(pad, 4);                                                                // 36
    file.read((uint8_t *)&_prefs->node_lat, sizeof(_prefs->node_lat));                // 40
    file.read((uint8_t *)&_prefs->node_lon, sizeof(_prefs->node_lon));                // 48
    file.read((uint8_t *)&_prefs->password[0], sizeof(_prefs->password));             // 56
    file.read((uint8_t *)&_prefs->freq, sizeof(_prefs->freq));                        // 72
    file.read((uint8_t *)&_prefs->tx_power_dbm, sizeof(_prefs->tx_power_dbm));        // 76
    file.read((uint8_t *)&_prefs->disable_fwd, sizeof(_prefs->disable_fwd));          // 77
    file.read((uint8_t *)&_prefs->advert_interval, sizeof(_prefs->advert_interval));  // 78
    file.read((uint8_t *)&_prefs->rx_boosted_gain, sizeof(_prefs->rx_boosted_gain));  // 79
    file.read((uint8_t *)&_prefs->rx_delay_base, sizeof(_prefs->rx_delay_base));      // 80
    file.read((uint8_t *)&_prefs->tx_delay_factor, sizeof(_prefs->tx_delay_factor));  // 84
    file.read((uint8_t *)&_prefs->guest_password[0], sizeof(_prefs->guest_password)); // 88
    file.read((uint8_t *)&_prefs->direct_tx_delay_factor, sizeof(_prefs->direct_tx_delay_factor)); // 104
    file.read(pad, 4); // 108 : 4 bytes unused
    file.read((uint8_t *)&_prefs->sf, sizeof(_prefs->sf));                                         // 112
    file.read((uint8_t *)&_prefs->cr, sizeof(_prefs->cr));                                         // 113
    file.read((uint8_t *)&_prefs->allow_read_only, sizeof(_prefs->allow_read_only));               // 114
    file.read((uint8_t *)&_prefs->multi_acks, sizeof(_prefs->multi_acks));                         // 115
    file.read((uint8_t *)&_prefs->bw, sizeof(_prefs->bw));                                         // 116
    file.read((uint8_t *)&_prefs->agc_reset_interval, sizeof(_prefs->agc_reset_interval));         // 120
    file.read((uint8_t *)&_prefs->path_hash_mode, sizeof(_prefs->path_hash_mode));                 // 121
    //file.read((uint8_t *)&_prefs->sx126x_rx_boosted_gain, sizeof(_prefs->sx126x_rx_boosted_gain)); // 121 BUG
    file.read((uint8_t *)&_prefs->loop_detect, sizeof(_prefs->loop_detect));                       // 122
    file.read(pad, 1);                                                                             // 123
    file.read((uint8_t *)&_prefs->flood_max, sizeof(_prefs->flood_max));                           // 124
    file.read((uint8_t *)&_prefs->flood_advert_interval, sizeof(_prefs->flood_advert_interval));   // 125
    file.read((uint8_t *)&_prefs->interference_threshold, sizeof(_prefs->interference_threshold)); // 126
    file.read((uint8_t *)&_prefs->bridge_enabled, sizeof(_prefs->bridge_enabled));                 // 127
    file.read((uint8_t *)&_prefs->bridge_delay, sizeof(_prefs->bridge_delay));                     // 128
    file.read((uint8_t *)&_prefs->bridge_pkt_src, sizeof(_prefs->bridge_pkt_src));                 // 130
    file.read((uint8_t *)&_prefs->bridge_baud, sizeof(_prefs->bridge_baud));                       // 131
    file.read((uint8_t *)&_prefs->bridge_channel, sizeof(_prefs->bridge_channel));                 // 135
    file.read((uint8_t *)&_prefs->bridge_secret, sizeof(_prefs->bridge_secret));                   // 136
    file.read((uint8_t *)&_prefs->powersaving_enabled, sizeof(_prefs->powersaving_enabled));       // 152
    file.read((uint8_t *)&_prefs->gps_enabled, sizeof(_prefs->gps_enabled));                       // 156
    file.read((uint8_t *)&_prefs->gps_interval, sizeof(_prefs->gps_interval));                     // 157
    file.read((uint8_t *)&_prefs->advert_loc_policy, sizeof (_prefs->advert_loc_policy));          // 161
    file.read((uint8_t *)&_prefs->discovery_mod_timestamp, sizeof(_prefs->discovery_mod_timestamp)); // 162
    file.read((uint8_t *)&_prefs->adc_multiplier, sizeof(_prefs->adc_multiplier));                 // 166
    file.read((uint8_t *)_prefs->owner_info, sizeof(_prefs->owner_info));                          // 170
    // next: 290

    file.close();
  }
}

void CommonCLI::loadPrefsJson(FILESYSTEM *fs) {
    MESH_DEBUG_PRINTLN("Loading preferences from: /prefs.json");

    // Initialize with defaults first
    memset(_prefs, 0, sizeof(_prefs));
    setPrefsDefaults();
#ifdef WITH_MQTT_BRIDGE
    setMQTTPrefsDefaults();
#endif

    bool json_existed = fs->exists("/prefs.json");
    if (!json_existed) {
        MESH_DEBUG_PRINTLN("No configuration file: /prefs.json");
        return;
    }

#if defined(RP2040_PLATFORM)
    File file = fs->open("/prefs.json", "r");
#else
    File file = fs->open("/prefs.json");
#endif
    if (!file) {
        MESH_DEBUG_PRINTLN("Could not open /prefs.json!");
        return;
    }

    // read in and parse the file //
    String content = file.readString();
    DynamicJsonDocument config_doc(content.length());
    deserializeJson(config_doc, content);

    if (config_doc.containsKey("general")) {
        if (config_doc["general"].containsKey("node_name")) {
            String str = config_doc["general"]["node_name"].as<String>();
            str.toCharArray(_prefs->node_name, sizeof(_prefs->node_name));
        }
        if (config_doc["general"].containsKey("node_lat")) {
            _prefs->node_lat = config_doc["general"]["node_lat"].as<double>();
        }
        if (config_doc["general"].containsKey("node_lon")) {
            _prefs->node_lon = config_doc["general"]["node_lon"].as<double>();
        }
        if (config_doc["general"].containsKey("password")) {
            String str = config_doc["general"]["password"].as<String>();
            str.toCharArray(_prefs->password, sizeof(_prefs->password));
        }
        if (config_doc["general"].containsKey("password_protected")) {
            String str = config_doc["general"]["password_protected"].as<String>();
            str.toCharArray(_prefs->password_protected, sizeof(_prefs->password_protected));
        }
        if (config_doc["general"].containsKey("guest_password")) {
            String str = config_doc["general"]["guest_password"].as<String>();
            str.toCharArray(_prefs->guest_password, sizeof(_prefs->guest_password));
        }
        if (config_doc["general"].containsKey("powersaving_enabled")) {
            _prefs->powersaving_enabled = config_doc["general"]["powersaving_enabled"].as<bool>() ? 1 : 0;
        }
        if (config_doc["general"].containsKey("owner_info")) {
            String str = config_doc["general"]["owner_info"].as<String>();
            str.toCharArray(_prefs->owner_info, sizeof(_prefs->owner_info));
        }
    }

    if (config_doc.containsKey("protocol")) {
        if (config_doc["protocol"].containsKey("airtime_factor")) {
            _prefs->airtime_factor = config_doc["protocol"]["airtime_factor"].as<float>();
        }
        if (config_doc["protocol"].containsKey("disable_fwd")) {
            _prefs->disable_fwd = config_doc["protocol"]["disable_fwd"].as<bool>() ? 1 : 0;
        }
        if (config_doc["protocol"].containsKey("silent_running")) {
            _prefs->silent_running = config_doc["protocol"]["silent_running"].as<bool>();
        }
        if (config_doc["protocol"].containsKey("advert_interval")) {
            _prefs->advert_interval = config_doc["protocol"]["advert_interval"].as<uint8_t>();
        }
        if (config_doc["protocol"].containsKey("tx_delay_factor")) {
            _prefs->tx_delay_factor = config_doc["protocol"]["tx_delay_factor"].as<float>();
        }
        if (config_doc["protocol"].containsKey("direct_tx_delay_factor")) {
            _prefs->direct_tx_delay_factor = config_doc["protocol"]["direct_tx_delay_factor"].as<float>();
        }
        if (config_doc["protocol"].containsKey("allow_read_only")) {
            _prefs->allow_read_only = config_doc["protocol"]["allow_read_only"].as<bool>() ? 1 : 0;
        }
        if (config_doc["protocol"].containsKey("multi_acks")) {
            _prefs->multi_acks = config_doc["protocol"]["multi_acks"].as<bool>() ? 1 : 0;
        }
        if (config_doc["protocol"].containsKey("agc_reset_interval")) {
            _prefs->agc_reset_interval = config_doc["protocol"]["agc_reset_interval"].as<uint8_t>();
        }
        if (config_doc["protocol"].containsKey("path_hash_mode")) {
            _prefs->path_hash_mode = config_doc["protocol"]["path_hash_mode"].as<uint8_t>();
        }
        if (config_doc["protocol"].containsKey("loop_detect")) {
            String loop_detect = config_doc["protocol"]["path_hash_mode"].as<String>();
            if (loop_detect == "OFF") {
                _prefs->loop_detect = LOOP_DETECT_OFF;
            } else if (loop_detect == "MINIMAL") {
                _prefs->loop_detect = LOOP_DETECT_MINIMAL;
            } else if (loop_detect == "MODERATE") {
                _prefs->loop_detect = LOOP_DETECT_MODERATE;
            } else if (loop_detect == "STRICT") {
                _prefs->loop_detect = LOOP_DETECT_STRICT;
            }
        }
        if (config_doc["protocol"].containsKey("flood_max")) {
            _prefs->flood_max = config_doc["protocol"]["flood_max"].as<uint8_t>();
        }
        if (config_doc["protocol"].containsKey("flood_advert_interval")) {
            _prefs->flood_advert_interval = config_doc["protocol"]["flood_advert_interval"].as<uint8_t>();
        }
        if (config_doc["protocol"].containsKey("interference_threshold")) {
            _prefs->interference_threshold = config_doc["protocol"]["interference_threshold"].as<uint8_t>();
        }
        if (config_doc["protocol"].containsKey("discovery_mod_timestamp")) {
            _prefs->discovery_mod_timestamp = config_doc["protocol"]["discovery_mod_timestamp"].as<uint32_t>();
        }
    }

    if (config_doc.containsKey("location")) {
        if (config_doc["location"].containsKey("enabled")) {
            _prefs->gps_enabled = config_doc["location"]["enabled"].as<bool>() ? 1 : 0;
        }
        if (config_doc["location"].containsKey("interval")) {
            _prefs->gps_interval = config_doc["location"]["interval"].as<uint32_t>();
        }
        if (config_doc["location"].containsKey("advert_loc_policy")) {
            String policy = config_doc["location"]["advert_loc_policy"].as<String>();
            if (policy == "NONE") {
                _prefs->advert_loc_policy = ADVERT_LOC_NONE;
            } else if (policy == "SHARE") {
                _prefs->advert_loc_policy = ADVERT_LOC_SHARE;
            } else if (policy == "PREFS") {
                _prefs->advert_loc_policy = ADVERT_LOC_PREFS;
            }
        }
    }

    if (config_doc.containsKey("radio")) {
        if (config_doc["radio"].containsKey("freq")) {
            _prefs->freq = config_doc["radio"]["freq"].as<float>();
        }
        if (config_doc["radio"].containsKey("tx_power_dbm")) {
            _prefs->tx_power_dbm = config_doc["radio"]["tx_power_dbm"].as<uint8_t>();
        }
        if (config_doc["radio"].containsKey("rx_boosted_gain")) {
            _prefs->rx_boosted_gain = config_doc["radio"]["rx_boosted_gain"].as<uint8_t>();
        }
        if (config_doc["radio"].containsKey("rx_delay_base")) {
            _prefs->rx_delay_base = config_doc["radio"]["rx_delay_base"].as<float>();
        }
        if (config_doc["radio"].containsKey("spread_factor")) {
            _prefs->sf = config_doc["radio"]["spread_factor"].as<uint8_t>();
        }
        if (config_doc["radio"].containsKey("coding_rate")) {
            _prefs->cr = config_doc["radio"]["coding_rate"].as<uint8_t>();
        }
        if (config_doc["radio"].containsKey("bandwidth")) {
            _prefs->bw = config_doc["radio"]["bandwidth"].as<float>();
        }
        if (config_doc["radio"].containsKey("adc_multiplier")) {
            _prefs->adc_multiplier = config_doc["radio"]["adc_multiplier"].as<float>();
        }
    }

    if (config_doc.containsKey("bridge")) {
        if (config_doc["bridge"].containsKey("enabled")) {
            _prefs->bridge_enabled = config_doc["bridge"]["enabled"].as<bool>() ? 1 : 0;
        }
        if (config_doc["bridge"].containsKey("delay")) {
            _prefs->bridge_delay = config_doc["bridge"]["delay"].as<uint16_t>();
        }
        if (config_doc["bridge"].containsKey("pkt_src")) {
            _prefs->bridge_pkt_src = config_doc["bridge"]["pkt_src"].as<uint8_t>();
        }
        if (config_doc["bridge"].containsKey("baud")) {
            _prefs->bridge_baud = config_doc["bridge"]["baud"].as<uint32_t>();
        }
        if (config_doc["bridge"].containsKey("channel")) {
            _prefs->bridge_channel = config_doc["bridge"]["channel"].as<uint8_t>();
        }
        if (config_doc["bridge"].containsKey("secret")) {
            String str = config_doc["bridge"]["secret"].as<String>();
            str.toCharArray(_prefs->bridge_secret, sizeof(_prefs->bridge_secret));
        }
    }

    if (config_doc.containsKey("mqtt")) {
        if (config_doc["mqtt"].containsKey("admin_public_key")) {
            String str = config_doc["mqtt"]["admin_public_key"].as<String>();
            str.toCharArray(_prefs->mqtt_admin_public_key, sizeof(_prefs->mqtt_admin_public_key));
        }
        if (config_doc["mqtt"].containsKey("analyzer_us_enabled")) {
            _prefs->mqtt_analyzer_us_enabled = config_doc["mqtt"]["analyzer_us_enabled"].as<bool>() ? 1 : 0;
        }
        if (config_doc["mqtt"].containsKey("analyzer_eu_enabled")) {
            _prefs->mqtt_analyzer_eu_enabled = config_doc["mqtt"]["analyzer_eu_enabled"].as<bool>() ? 1 : 0;
        }
        if (config_doc["mqtt"].containsKey("email")) {
            String str = config_doc["mqtt"]["email"].as<String>();
            str.toCharArray(_prefs->mqtt_email, sizeof(_prefs->mqtt_email));
        }
        if (config_doc["mqtt"].containsKey("iata")) {
            String str = config_doc["mqtt"]["iata"].as<String>();
            str.toCharArray(_prefs->mqtt_iata, sizeof(_prefs->mqtt_iata));
        }
        if (config_doc["mqtt"].containsKey("packets_enabled")) {
            _prefs->mqtt_packets_enabled = config_doc["mqtt"]["packets_enabled"].as<bool>() ? 1 : 0;
        }
        if (config_doc["mqtt"].containsKey("origin")) {
            String str = config_doc["mqtt"]["origin"].as<String>();
            str.toCharArray(_prefs->mqtt_origin, sizeof(_prefs->mqtt_origin));
        }
        if (config_doc["mqtt"].containsKey("owner_public_key")) {
            String str = config_doc["mqtt"]["owner_public_key"].as<String>();
            str.toCharArray(_prefs->mqtt_owner_public_key, sizeof(_prefs->mqtt_owner_public_key));
        }
        if (config_doc["mqtt"].containsKey("password")) {
            String str = config_doc["mqtt"]["password"].as<String>();
            str.toCharArray(_prefs->mqtt_password, sizeof(_prefs->mqtt_password));
        }
        if (config_doc["mqtt"].containsKey("port")) {
            _prefs->mqtt_port = config_doc["mqtt"]["port"].as<int>();
        }
        if (config_doc["mqtt"].containsKey("raw_enabled")) {
            _prefs->mqtt_raw_enabled = config_doc["mqtt"]["raw_enabled"].as<bool>() ? 1 : 0;
        }
        if (config_doc["mqtt"].containsKey("remote_enabled")) {
            _prefs->mqtt_remote_enabled = config_doc["mqtt"]["remote_enabled"].as<bool>() ? 1 : 0;
        }
        if (config_doc["mqtt"].containsKey("status_enabled")) {
            _prefs->mqtt_status_enabled = config_doc["mqtt"]["status_enabled"].as<bool>() ? 1 : 0;
        }
        if (config_doc["mqtt"].containsKey("status_interval")) {
            _prefs->mqtt_status_interval = config_doc["mqtt"]["status_interval"].as<int>();
        }
        if (config_doc["mqtt"].containsKey("server")) {
            String str = config_doc["mqtt"]["server"].as<String>();
            str.toCharArray(_prefs->mqtt_server, sizeof(_prefs->mqtt_server));
        }
        if (config_doc["mqtt"].containsKey("tx_enabled")) {
            _prefs->mqtt_tx_enabled = config_doc["mqtt"]["tx_enabled"].as<bool>() ? 1 : 0;
        }
        if (config_doc["mqtt"].containsKey("use_acl")) {
            _prefs->mqtt_use_acl = config_doc["mqtt"]["use_acl"].as<bool>() ? 1 : 0;
        }
        if (config_doc["mqtt"].containsKey("username")) {
            String str = config_doc["mqtt"]["username"].as<String>();
            str.toCharArray(_prefs->mqtt_username, sizeof(_prefs->mqtt_username));
        }
    }

    if (config_doc.containsKey("timezone")) {
        if (config_doc["timezone"].containsKey("offset")) {
            _prefs->timezone_offset = config_doc["mqtt"]["offset"].as<int>();
        }
        if (config_doc["timezone"].containsKey("string")) {
            String str = config_doc["timezone"]["string"].as<String>();
            str.toCharArray(_prefs->timezone_string, sizeof(_prefs->timezone_string));
        }
    }

    if (config_doc.containsKey("wifi")) {
        if (config_doc["wifi"].containsKey("ntp_enabled")) {
            _prefs->wifi_ntp_enabled = config_doc["wifi"]["ntp_enabled"].as<bool>() ? 1 : 0;
        }
        if (config_doc["wifi"].containsKey("ntp_server")) {
            String str = config_doc["wifi"]["ntp_server"].as<String>();
            str.toCharArray(_prefs->wifi_ntp_server, sizeof(_prefs->wifi_ntp_server));
        }
        if (config_doc["wifi"].containsKey("password")) {
            String str = config_doc["wifi"]["password"].as<String>();
            str.toCharArray(_prefs->wifi_password, sizeof(_prefs->wifi_password));
        }
        if (config_doc["wifi"].containsKey("power_save")) {
            String str = config_doc["wifi"]["power_save"].as<String>();
            if (str == "min") {
                _prefs->wifi_power_save = 0;
            }else if (str == "none") {
                _prefs->wifi_power_save = 1;
            }else if (str == "max") {
                _prefs->wifi_power_save = 2;
            }
        }
        if (config_doc["wifi"].containsKey("ssid")) {
            String str = config_doc["wifi"]["ssid"].as<String>();
            str.toCharArray(_prefs->wifi_ssid, sizeof(_prefs->wifi_ssid));
        }
        if (config_doc["wifi"].containsKey("telnet_enabled")) {
            _prefs->wifi_telnet_enabled = config_doc["wifi"]["telnet_enabled"].as<bool>() ? 1 : 0;
        }
        if (config_doc["wifi"].containsKey("telnet_timeout")) {
            _prefs->wifi_telnet_timeout = config_doc["wifi"]["telnet_timeout"].as<uint16_t>();
        }
    }

    file.close();
}

void CommonCLI::sanitizePrefs() {
    // sanitise bad pref values
    _prefs->rx_delay_base = constrain(_prefs->rx_delay_base, 0, 20.0f);
    _prefs->tx_delay_factor = constrain(_prefs->tx_delay_factor, 0, 2.0f);
    _prefs->direct_tx_delay_factor = constrain(_prefs->direct_tx_delay_factor, 0, 2.0f);
    _prefs->airtime_factor = constrain(_prefs->airtime_factor, 0, 9.0f);
    _prefs->freq = constrain(_prefs->freq, 400.0f, 2500.0f);
    _prefs->bw = constrain(_prefs->bw, 7.8f, 500.0f);
    _prefs->sf = constrain(_prefs->sf, 5, 12);
    _prefs->cr = constrain(_prefs->cr, 5, 8);
    _prefs->tx_power_dbm = constrain(_prefs->tx_power_dbm, -9, 30);
    _prefs->multi_acks = constrain(_prefs->multi_acks, 0, 1);
    _prefs->adc_multiplier = constrain(_prefs->adc_multiplier, 0.0f, 10.0f);
    _prefs->path_hash_mode = constrain(_prefs->path_hash_mode, 0, 2);   // NOTE: mode 3 reserved for future

    // sanitise bad bridge pref values
    _prefs->bridge_enabled = constrain(_prefs->bridge_enabled, 0, 1);
    _prefs->bridge_delay = constrain(_prefs->bridge_delay, 0, 10000);
    _prefs->bridge_pkt_src = constrain(_prefs->bridge_pkt_src, 0, 1);
    _prefs->bridge_baud = constrain(_prefs->bridge_baud, 9600, 115200);
    _prefs->bridge_channel = constrain(_prefs->bridge_channel, 0, 14);

    _prefs->powersaving_enabled = constrain(_prefs->powersaving_enabled, 0, 1);

    _prefs->gps_enabled = constrain(_prefs->gps_enabled, 0, 1);
    _prefs->advert_loc_policy = constrain(_prefs->advert_loc_policy, 0, 2);

    // sanitise power settings
    _prefs->rx_boosted_gain = constrain(_prefs->rx_boosted_gain, 0, 1); // boolean
}

void CommonCLI::savePrefs(FILESYSTEM* fs) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  fs->remove("/com_prefs");
  fs->remove("/prefs.json");
  File config_file = fs->open("/prefs.json", FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  File config_file = fs->open("/prefs.json", "w");
#else
  File config_file = fs->open("/prefs.json", "w", true);
#endif
  if (!config_file) {
      MESH_DEBUG_PRINTLN("Failed to write configuration file: /prefs.json");
      return;
  }

  // setup json
  DynamicJsonDocument config_doc(1024);

  JsonObject general = config_doc.createNestedObject("general");
  general["node_name"] = _prefs->node_name;
  general["node_lat"] = _prefs->node_lat;
  general["node_lon"] = _prefs->node_lon;
  general["password"] = _prefs->password;
  general["password_protected"] = _prefs->password_protected;
  general["guest_password"] = _prefs->guest_password;
  general["powersaving_enabled"] = _prefs->powersaving_enabled ? true : false;
  general["owner_info"] = _prefs->owner_info;

  JsonObject protocol = config_doc.createNestedObject("protocol");
  protocol["airtime_factor"] = _prefs->airtime_factor;
  protocol["disable_fwd"] = _prefs->disable_fwd ? true : false;
  protocol["silent_running"] = _prefs->silent_running ? true : false;
  protocol["advert_interval"] = _prefs->advert_interval;
  protocol["rx_delay_base"] = _prefs->rx_delay_base;
  protocol["tx_delay_factor"] = _prefs->tx_delay_factor;
  protocol["direct_tx_delay_factor"] = _prefs->direct_tx_delay_factor;
  protocol["allow_read_only"] = _prefs->allow_read_only ? true : false;
  protocol["multi_acks"] = _prefs->multi_acks ? true : false;
  protocol["agc_reset_interval"] = _prefs->agc_reset_interval;
  protocol["path_hash_mode"] = _prefs->path_hash_mode;
  if (LOOP_DETECT_OFF == _prefs->loop_detect) {
    protocol["loop_detect"] = "OFF";
  } else if (LOOP_DETECT_MINIMAL == _prefs->loop_detect) {
    protocol["loop_detect"] = "MINIMAL";
  } else if (LOOP_DETECT_MODERATE == _prefs->loop_detect) {
    protocol["loop_detect"] = "MODERATE";
  } else if (LOOP_DETECT_STRICT == _prefs->loop_detect) {
    protocol["loop_detect"] = "STRICT";
  }
  protocol["flood_max"] = _prefs->flood_max;
  protocol["flood_advert_interval"] = _prefs->flood_advert_interval;
  protocol["interference_threshold"] = _prefs->interference_threshold;
  protocol["discovery_mod_timestamp"] = _prefs->discovery_mod_timestamp;

  JsonObject location = config_doc.createNestedObject("location");
  location["enabled"] = _prefs->gps_enabled ? true : false;
  location["interval"] = _prefs->gps_interval;
  if (ADVERT_LOC_NONE == _prefs->advert_loc_policy) {
    location["advert_loc_policy"] = "NONE";
  } else if (ADVERT_LOC_SHARE == _prefs->advert_loc_policy) {
    location["advert_loc_policy"] = "SHARE";
  } else if (ADVERT_LOC_PREFS == _prefs->advert_loc_policy) {
    location["advert_loc_policy"] = "PREFS";
  }

  JsonObject radio = config_doc.createNestedObject("radio");
  radio["freq"] = _prefs->freq;
  radio["tx_power_dbm"] = _prefs->tx_power_dbm;
  radio["rx_boosted_gain"] = _prefs->rx_boosted_gain;
  radio["rx_delay_base"] = _prefs->rx_delay_base;
  radio["spread_factor"] = _prefs->sf;
  radio["coding_rate"] = _prefs->cr;
  radio["bandwidth"] = _prefs->bw;
  radio["adc_multiplier"] = _prefs->adc_multiplier;

  JsonObject bridge = config_doc.createNestedObject("bridge");
  bridge["enabled"] = _prefs->bridge_enabled ? true : false;
  bridge["delay"] = _prefs->bridge_delay;
  bridge["pkt_src"] = _prefs->bridge_pkt_src;
  bridge["baud"] = _prefs->bridge_baud;
  bridge["channel"] = _prefs->bridge_channel;
  bridge["secret"] = _prefs->bridge_secret;

#ifdef WITH_MQTT_BRIDGE
  JsonObject mqtt = config_doc.createNestedObject("mqtt");
  mqtt["admin_public_key"] = _prefs->mqtt_admin_public_key;
  mqtt["analyzer_us_enabled"] = _prefs->mqtt_analyzer_us_enabled ? true : false;
  mqtt["analyzer_eu_enabled"] = _prefs->mqtt_analyzer_eu_enabled ? true : false;
  mqtt["email"] = _prefs->mqtt_email;
  mqtt["iata"] = _prefs->mqtt_iata;
  mqtt["packets_enabled"] = _prefs->mqtt_packets_enabled ? true : false;
  mqtt["origin"] = _prefs->mqtt_origin;
  mqtt["owner_public_key"] = _prefs->mqtt_owner_public_key;
  mqtt["password"] = _prefs->mqtt_password;
  mqtt["port"] = _prefs->mqtt_port;
  mqtt["raw_enabled"] = _prefs->mqtt_raw_enabled ? true : false;
  mqtt["remote_enabled"] = _prefs->mqtt_remote_enabled ? true : false;
  mqtt["status_enabled"] = _prefs->mqtt_status_enabled ? true : false;
  mqtt["status_interval"] = _prefs->mqtt_status_interval;
  mqtt["server"] = _prefs->mqtt_server;
  mqtt["tx_enabled"] = _prefs->mqtt_tx_enabled ? true : false;
  mqtt["use_acl"] = _prefs->mqtt_use_acl ? true : false;
  mqtt["username"] = _prefs->mqtt_username;

  JsonObject timezone = config_doc.createNestedObject("timezone");
  timezone["offset"] = _prefs->timezone_offset;
  timezone["string"] = _prefs->timezone_string;

  JsonObject wifi = config_doc.createNestedObject("wifi");
  wifi["ntp_enabled"] = _prefs->wifi_ntp_enabled ? true : false;
  wifi["ntp_server"] = _prefs->wifi_ntp_server;
  wifi["password"] = _prefs->wifi_password;
  if (0 == _prefs->wifi_power_save) {
      wifi["power_save"] = "min";
  }else if (1 == _prefs->wifi_power_save) {
      wifi["power_save"] = "none";
  }else if (2 == _prefs->wifi_power_save) {
      wifi["power_save"] = "max";
  }
  wifi["ssid"] = _prefs->wifi_ssid;
  wifi["telnet_enabled"] = _prefs->wifi_telnet_enabled ? true : false;
  wifi["telnet_timeout"] = _prefs->wifi_telnet_timeout;
#endif

  // write out //
  serializeJson(config_doc, config_file);
  config_file.close();
}

void CommonCLI::setPrefsDefaults() {
  // Set sensible defaults
  _prefs->airtime_factor = 1.0;   // one half
  _prefs->rx_delay_base = 0.0f;   // turn off by default, was 10.0;
  _prefs->tx_delay_factor = 0.5f; // was 0.25f
  _prefs->direct_tx_delay_factor = 0.3f; // was 0.2
  *_prefs->node_name = '\0';
  _prefs->node_lat = 0.0;
  _prefs->node_lon = 0.0;
  *_prefs->password = '\0';
  _prefs->freq = 869.525;         // it's documented that the EU radio settings are used by default
  _prefs->sf = 250.0;
  _prefs->bw = 11;
  _prefs->cr = 5;
  _prefs->tx_power_dbm = 0;
  _prefs->advert_interval = 1;    // default to 2 minutes for NEW installs
  _prefs->flood_advert_interval = 12; // 12 hours
  _prefs->flood_max = 64;
  _prefs->interference_threshold = 0; // disabled

  // bridge defaults
  _prefs->bridge_enabled = 1;    // enabled
  _prefs->bridge_delay   = 500;  // milliseconds
  _prefs->bridge_pkt_src = 1;    // logRx (RX packets)
  _prefs->bridge_baud = 115200;  // baud rate
  _prefs->bridge_channel = 1;    // channel 1

  *_prefs->bridge_secret = '\0';

  // GPS defaults
  _prefs->gps_enabled = 0;
  _prefs->gps_interval = 0;
  _prefs->advert_loc_policy = ADVERT_LOC_PREFS;

  _prefs->adc_multiplier = 0.0f; // 0.0f means use default board multiplier

  // String fields are already zero-initialized by memset
}

#ifdef WITH_MQTT_BRIDGE
// Set default values for MQTT preferences (used when file doesn't exist or is corrupted)
void CommonCLI::setMQTTPrefsDefaults() {
  // Set sensible defaults matching MQTTBridge expectations
  _prefs->mqtt_status_enabled = 1;    // enabled by default
  _prefs->mqtt_packets_enabled = 1;   // enabled by default
  _prefs->mqtt_raw_enabled = 0;       // disabled by default
  _prefs->mqtt_tx_enabled = 0;        // disabled by default (RX only)
  _prefs->mqtt_status_interval = 300000; // 5 minutes default
  _prefs->mqtt_analyzer_us_enabled = 1; // enabled by default
  _prefs->mqtt_analyzer_eu_enabled = 1; // enabled by default
  _prefs->wifi_power_save = 0; // Default to WIFI_PS_MIN_MODEM (0=min)
  _prefs->mqtt_remote_enabled = 0;    // Off by default
  _prefs->mqtt_use_acl = 1;           // Use ACL by default
  _prefs->mqtt_admin_public_key[0] = '\0'; // Empty by default
  _prefs->wifi_ntp_enabled = 1;       // enabled by default
  strncpy(_prefs->wifi_ntp_server, "pool.ntp.org", sizeof(_prefs->wifi_ntp_server));
  _prefs->wifi_telnet_enabled = 0;    // telnet disabled by default
  _prefs->wifi_telnet_timeout = 300;  // telnet timeout is 300 seconds by default

  // String fields are already zero-initialized by memset
}

void CommonCLI::loadMQTTPrefs(FILESYSTEM* fs) {
  MQTTPrefs mqtt_prefs;
  memset(&mqtt_prefs, 0, sizeof(mqtt_prefs));

  // attempt json read first //
  bool json_existed = fs->exists("/mqtt_prefs.json");
  if (json_existed) {
#if defined(RP2040_PLATFORM)
    File file = fs->open("/mqtt_prefs.json", "r");
#else
    File file = fs->open("/mqtt_prefs.json");
#endif
        if (file) {
            // read in and parse the file //
            String content = file.readString();
            DynamicJsonDocument config_doc(content.length());
            deserializeJson(config_doc, content);

            if (config_doc.containsKey("config")) {
                if (config_doc["config"].containsKey("admin_public_key")) {
                    String str = config_doc["config"]["admin_public_key"].as<String>();
                    str.toCharArray(mqtt_prefs.mqtt_admin_public_key, sizeof(mqtt_prefs.mqtt_admin_public_key));
                }
                if (config_doc["config"].containsKey("email")) {
                    String str = config_doc["config"]["email"].as<String>();
                    str.toCharArray(mqtt_prefs.mqtt_email, sizeof(mqtt_prefs.mqtt_email));
                }
                if (config_doc["config"].containsKey("owner_public_key")) {
                    String str = config_doc["config"]["owner_public_key"].as<String>();
                    str.toCharArray(mqtt_prefs.mqtt_owner_public_key, sizeof(mqtt_prefs.mqtt_owner_public_key));
                }
            }

            if (config_doc.containsKey("mqtt")) {
                if (config_doc["mqtt"].containsKey("analyzer_us_enabled")) {
                    mqtt_prefs.mqtt_analyzer_us_enabled = config_doc["mqtt"]["analyzer_us_enabled"].as<bool>() ? 1 : 0;
                }
                if (config_doc["mqtt"].containsKey("analyzer_eu_enabled")) {
                    mqtt_prefs.mqtt_analyzer_eu_enabled = config_doc["mqtt"]["analyzer_eu_enabled"].as<bool>() ? 1 : 0;
                }
                if (config_doc["mqtt"].containsKey("iata")) {
                    String str = config_doc["mqtt"]["iata"].as<String>();
                    str.toCharArray(mqtt_prefs.mqtt_iata, sizeof(mqtt_prefs.mqtt_iata));
                }
                if (config_doc["mqtt"].containsKey("packets_enabled")) {
                    mqtt_prefs.mqtt_packets_enabled = config_doc["mqtt"]["packets_enabled"].as<bool>() ? 1 : 0;
                }
                if (config_doc["mqtt"].containsKey("origin")) {
                    String str = config_doc["mqtt"]["origin"].as<String>();
                    str.toCharArray(mqtt_prefs.mqtt_origin, sizeof(mqtt_prefs.mqtt_origin));
                }
                if (config_doc["mqtt"].containsKey("password")) {
                    String str = config_doc["mqtt"]["password"].as<String>();
                    str.toCharArray(mqtt_prefs.mqtt_password, sizeof(mqtt_prefs.mqtt_password));
                }
                if (config_doc["mqtt"].containsKey("port")) {
                    mqtt_prefs.mqtt_port = config_doc["mqtt"]["port"].as<int>();
                }
                if (config_doc["mqtt"].containsKey("raw_enabled")) {
                    mqtt_prefs.mqtt_raw_enabled = config_doc["mqtt"]["raw_enabled"].as<bool>() ? 1 : 0;
                }
                if (config_doc["mqtt"].containsKey("remote_enabled")) {
                    mqtt_prefs.mqtt_remote_enabled = config_doc["mqtt"]["remote_enabled"].as<bool>() ? 1 : 0;
                }
                if (config_doc["mqtt"].containsKey("status_enabled")) {
                    mqtt_prefs.mqtt_status_enabled = config_doc["mqtt"]["status_enabled"].as<bool>() ? 1 : 0;
                }
                if (config_doc["mqtt"].containsKey("status_interval")) {
                    mqtt_prefs.mqtt_status_interval = config_doc["mqtt"]["status_interval"].as<int>();
                }
                if (config_doc["mqtt"].containsKey("server")) {
                    String str = config_doc["mqtt"]["server"].as<String>();
                    str.toCharArray(mqtt_prefs.mqtt_server, sizeof(mqtt_prefs.mqtt_server));
                }
                if (config_doc["mqtt"].containsKey("tx_enabled")) {
                    mqtt_prefs.mqtt_tx_enabled = config_doc["mqtt"]["tx_enabled"].as<bool>() ? 1 : 0;
                }
                if (config_doc["mqtt"].containsKey("use_acl")) {
                    mqtt_prefs.mqtt_use_acl = config_doc["mqtt"]["use_acl"].as<bool>() ? 1 : 0;
                }
                if (config_doc["mqtt"].containsKey("username")) {
                    String str = config_doc["mqtt"]["username"].as<String>();
                    str.toCharArray(mqtt_prefs.mqtt_username, sizeof(mqtt_prefs.mqtt_username));
                }
            }

            if (config_doc.containsKey("timezone")) {
                if (config_doc["timezone"].containsKey("ntp_server")) {
                    String str = config_doc["timezone"]["ntp_server"].as<String>();
                    str.toCharArray(mqtt_prefs.timezone_ntp_server, sizeof(mqtt_prefs.timezone_ntp_server));
                }
                if (config_doc["timezone"].containsKey("offset")) {
                    mqtt_prefs.timezone_offset = config_doc["mqtt"]["offset"].as<int>();
                }
                if (config_doc["timezone"].containsKey("string")) {
                    String str = config_doc["timezone"]["string"].as<String>();
                    str.toCharArray(mqtt_prefs.timezone_string, sizeof(mqtt_prefs.timezone_string));
                }
            }

            if (config_doc.containsKey("wifi")) {
                if (config_doc["wifi"].containsKey("password")) {
                    String str = config_doc["wifi"]["password"].as<String>();
                    str.toCharArray(mqtt_prefs.wifi_password, sizeof(mqtt_prefs.wifi_password));
                }
                if (config_doc["wifi"].containsKey("power_save")) {
                    String str = config_doc["wifi"]["power_save"].as<String>();
                    if (str == "min") {
                        mqtt_prefs.wifi_power_save = 0;
                    }else if (str == "none") {
                        mqtt_prefs.wifi_power_save = 1;
                    }else if (str == "max") {
                        mqtt_prefs.wifi_power_save = 2;
                    }
                }
                if (config_doc["wifi"].containsKey("ssid")) {
                    String str = config_doc["wifi"]["ssid"].as<String>();
                    str.toCharArray(mqtt_prefs.wifi_ssid, sizeof(mqtt_prefs.wifi_ssid));
                }
            }

            file.close();

            syncMQTTPrefsToNodePrefs(&mqtt_prefs);
            return;
        }
  }

  bool file_existed = fs->exists("/mqtt_prefs");
  if (file_existed) {
    MESH_DEBUG_PRINTLN("Loading configuration from /mqtt_prefs (will migrate to /mqtt_prefs.json)...");

    // Load from separate MQTT prefs file
#if defined(RP2040_PLATFORM)
    File file = fs->open("/mqtt_prefs", "r");
#else
    File file = fs->open("/mqtt_prefs");
#endif
    if (file) {
      // Verify file size is correct before reading
      if (file.size() >= sizeof(mqtt_prefs)) {
        size_t bytes_read = file.read((uint8_t *)&mqtt_prefs, sizeof(mqtt_prefs));
        if (bytes_read != sizeof(mqtt_prefs)) {
          // File read incomplete - reinitialize to defaults
          MESH_DEBUG_PRINTLN("Configuration read size unexpected in /mqtt_prefs -- initializing to defaults!");
        }else
        {
            syncMQTTPrefsToNodePrefs(&mqtt_prefs);
        }
      } else {
        // File too small - reinitialize to defaults
        MESH_DEBUG_PRINTLN("Configuration file /mqtt_prefs is of unexpected size -- initializing to defaults!");
      }
      file.close();
      fs->remove("/mqtt_prefs");

      return;
    }
  } else {
    MESH_DEBUG_PRINTLN("Loading configuration from older /com_prefs (will migrate to /mqtt_prefs.json)...");

    // Migration: Try to read from old /com_prefs file if it exists
    // This handles the case where MQTT settings were previously stored in /com_prefs
    if (fs->exists("/com_prefs")) {
#if defined(RP2040_PLATFORM)
      File file = fs->open("/com_prefs", "r");
#else
      File file = fs->open("/com_prefs");
#endif
      if (file) {
        // Skip to MQTT section (after advert_loc_policy at offset 161)
        // Calculate offset: we need to skip everything up to and including advert_loc_policy
        size_t offset_to_mqtt = 
          sizeof(_prefs->airtime_factor) + sizeof(_prefs->node_name) + 4 + // pad
          sizeof(_prefs->node_lat) + sizeof(_prefs->node_lon) +
          sizeof(_prefs->password) + sizeof(_prefs->freq) +
          sizeof(_prefs->tx_power_dbm) + sizeof(_prefs->disable_fwd) +
          sizeof(_prefs->advert_interval) + 1 + // pad
          sizeof(_prefs->rx_delay_base) + sizeof(_prefs->tx_delay_factor) +
          sizeof(_prefs->guest_password) + sizeof(_prefs->direct_tx_delay_factor) + 4 + // pad
          sizeof(_prefs->sf) + sizeof(_prefs->cr) +
          sizeof(_prefs->allow_read_only) + sizeof(_prefs->multi_acks) +
          sizeof(_prefs->bw) + sizeof(_prefs->agc_reset_interval) + 3 + // pad
          sizeof(_prefs->flood_max) + sizeof(_prefs->flood_advert_interval) +
          sizeof(_prefs->interference_threshold) + sizeof(_prefs->bridge_enabled) +
          sizeof(_prefs->bridge_delay) + sizeof(_prefs->bridge_pkt_src) +
          sizeof(_prefs->bridge_baud) + sizeof(_prefs->bridge_channel) +
          sizeof(_prefs->bridge_secret) + 4 + // pad
          sizeof(_prefs->gps_enabled) + sizeof(_prefs->gps_interval) +
          sizeof(_prefs->advert_loc_policy);
        
        // Check if file is large enough and seek succeeded
        if (file.size() >= offset_to_mqtt + sizeof(mqtt_prefs)) {
          if (file.seek(offset_to_mqtt)) {
            size_t bytes_read = file.read((uint8_t *)&mqtt_prefs, sizeof(mqtt_prefs));
            if (bytes_read == sizeof(mqtt_prefs)) {
              // Successfully migrated - save to new location for future use
              file.close();
              fs->remove("/com_prefs");

              syncMQTTPrefsToNodePrefs(&mqtt_prefs);
              return; // Migration successful
            }
          }
        }
        file.close();
        // Migration failed - defaults already set, just return
        MESH_DEBUG_PRINTLN("Configuration failed to load from /com_prefs -- using defaults.");
        return;
      }
    }
    // No file exists and migration didn't happen - defaults already set
  }
}

void CommonCLI::syncMQTTPrefsToNodePrefs(MQTTPrefs *mqtt_prefs) {
  // Copy MQTT prefs to NodePrefs so existing code can access them
  // Use StrHelper::strncpy to ensure proper null termination
  StrHelper::strncpy(_prefs->mqtt_admin_public_key, mqtt_prefs->mqtt_admin_public_key, sizeof(_prefs->mqtt_admin_public_key));
  StrHelper::strncpy(_prefs->mqtt_email, mqtt_prefs->mqtt_email, sizeof(_prefs->mqtt_email));
  StrHelper::strncpy(_prefs->mqtt_owner_public_key, mqtt_prefs->mqtt_owner_public_key, sizeof(_prefs->mqtt_owner_public_key));

  _prefs->mqtt_analyzer_us_enabled = mqtt_prefs->mqtt_analyzer_us_enabled;
  _prefs->mqtt_analyzer_eu_enabled = mqtt_prefs->mqtt_analyzer_eu_enabled;
  StrHelper::strncpy(_prefs->mqtt_iata, mqtt_prefs->mqtt_iata, sizeof(_prefs->mqtt_iata));
  StrHelper::strncpy(_prefs->mqtt_origin, mqtt_prefs->mqtt_origin, sizeof(_prefs->mqtt_origin));
  _prefs->mqtt_packets_enabled = mqtt_prefs->mqtt_packets_enabled;
  StrHelper::strncpy(_prefs->mqtt_password, mqtt_prefs->mqtt_password, sizeof(_prefs->mqtt_password));
  _prefs->mqtt_port = mqtt_prefs->mqtt_port;
  _prefs->mqtt_raw_enabled = mqtt_prefs->mqtt_raw_enabled;
  _prefs->mqtt_remote_enabled = mqtt_prefs->mqtt_remote_enabled;
  _prefs->mqtt_status_enabled = mqtt_prefs->mqtt_status_enabled;
  _prefs->mqtt_status_interval = mqtt_prefs->mqtt_status_interval;
  StrHelper::strncpy(_prefs->mqtt_server, mqtt_prefs->mqtt_server, sizeof(_prefs->mqtt_server));
  _prefs->mqtt_tx_enabled = mqtt_prefs->mqtt_tx_enabled;
  _prefs->mqtt_use_acl = mqtt_prefs->mqtt_use_acl;
  StrHelper::strncpy(_prefs->mqtt_username, mqtt_prefs->mqtt_username, sizeof(_prefs->mqtt_username));

  StrHelper::strncpy(_prefs->wifi_ntp_server, mqtt_prefs->timezone_ntp_server, sizeof(_prefs->wifi_ntp_server));
  _prefs->timezone_offset = mqtt_prefs->timezone_offset;
  StrHelper::strncpy(_prefs->timezone_string, mqtt_prefs->timezone_string, sizeof(_prefs->timezone_string));

  StrHelper::strncpy(_prefs->wifi_password, mqtt_prefs->wifi_password, sizeof(_prefs->wifi_password));
  _prefs->wifi_power_save = mqtt_prefs->wifi_power_save;
  StrHelper::strncpy(_prefs->wifi_ssid, mqtt_prefs->wifi_ssid, sizeof(_prefs->wifi_ssid));
}

#endif

#define MIN_LOCAL_ADVERT_INTERVAL   60

void CommonCLI::savePrefs() {
  if (_prefs->advert_interval * 2 < MIN_LOCAL_ADVERT_INTERVAL) {
    _prefs->advert_interval = 0;  // turn it off, now that device has been manually configured
  }
  _callbacks->savePrefs();
}

uint8_t CommonCLI::buildAdvertData(uint8_t node_type, uint8_t* app_data) {
  if (_prefs->advert_loc_policy == ADVERT_LOC_NONE) {
    AdvertDataBuilder builder(node_type, _prefs->node_name);
    return builder.encodeTo(app_data);
  } else if (_prefs->advert_loc_policy == ADVERT_LOC_SHARE) {
    AdvertDataBuilder builder(node_type, _prefs->node_name, _sensors->node_lat, _sensors->node_lon);
    return builder.encodeTo(app_data);
  } else {
    AdvertDataBuilder builder(node_type, _prefs->node_name, _prefs->node_lat, _prefs->node_lon);
    return builder.encodeTo(app_data);
  }
}

bool CommonCLI::allowProtectedCommand(uint32_t sender_timestamp) {
    // check if serial //
    if (0 == sender_timestamp) return true;
    // protected not configured //
    if (0 == *_prefs->password_protected) return true;
    // check if allowed protected over remote //
    if (getRTCClock()->getCurrentTime() - gl_allow_protected_over_remote < PROTECTED_TIME_DURATION) return true;
    return false;
}

void CommonCLI::handleCommand(uint32_t sender_timestamp, const char* command, char* reply) {
    if (strcmp(command, "help") == 0) {
handleCommandHelpSections:
      strcpy(reply, "Help sections: admin, clear, debug, disable, general, get\n"
        "  gps, log, sensor, set, stats");
    } else if (memcmp(command, "help ", 5) == 0) {
        const char* section = &command[5];
        if (strcmp(section, "admin") == 0) {
          strcpy(reply, "Possible: board, clkreboot, clock sync, erase,\n"
            "  guest.password, memory, password[.protected],\n"
            "  powersaving, [temp]radio[.txgain], reboot, start ota, time, ver\n"
          );
        } else if (strcmp(section, "clear") == 0) {
          goto handleCommandHelpClear;
        } else if (strcmp(section, "debug") == 0) {
          goto handleCommandHelpDebug;
        } else if (strcmp(section, "disable") == 0) {
          goto handleCommandHelpDisable;
        } else if (strcmp(section, "general") == 0) {
          strcpy(reply, "Possible: advert[.zerohop], clock, neighbor[s|.remove],\n"
            "  neighbor[s|.remove], ,\n"
            "  tempradio, time\n"
          );
        } else if (strcmp(section, "gps") == 0) {
          strcpy(reply, "Possible: gps, on, sync, setloc, advert");
        } else if (strcmp(section, "log") == 0) {
          strcpy(reply, "Possible: log, start, stop, erase");
        } else if (strcmp(section, "sensor") == 0) {
          strcpy(reply, "Possible: get, list");
        } else if (strcmp(section, "stats") == 0) {
          strcpy(reply, "Possible: stats-packets, stats-radio, stats-core");
        } else {
            goto handleCommandHelpSections;
        }
    } else if (memcmp(command, "poweroff", 8) == 0 || memcmp(command, "shutdown", 8) == 0) {
      if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
      _board->powerOff();  // doesn't return
    } else if (strcmp(command, "reboot") == 0) {
      if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
      _board->reboot();  // doesn't return
    } else if (strcmp(command, "clkreboot") == 0) {
      if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
      // Reset clock
      getRTCClock()->setCurrentTime(1715770351);  // 15 May 2024, 8:50pm
      _board->reboot();  // doesn't return
    } else if (strcmp(command, "advert") == 0) {
      // send flood advert
      _callbacks->sendSelfAdvertisement(1500, true);  // longer delay, give CLI response time to be sent first
      strcpy(reply, "OK - Advert sent");
    } else if (strcmp(command, "advert.zerohop") == 0) {
     // send zerohop advert
     _callbacks->sendSelfAdvertisement(1500, false);  // longer delay, give CLI response time to be sent first
     strcpy(reply, "OK - zerohop advert sent");
    } else if (strcmp(command, "clock sync") == 0) {
      if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
      uint32_t curr = getRTCClock()->getCurrentTime();
      if (sender_timestamp > curr) {
        getRTCClock()->setCurrentTime(sender_timestamp + 1);
        uint32_t now = getRTCClock()->getCurrentTime();
        DateTime dt = DateTime(now);
        sprintf(reply, "OK - clock set: %02d:%02d - %d/%d/%d UTC", dt.hour(), dt.minute(), dt.day(), dt.month(), dt.year());
      } else {
        strcpy(reply, "ERR: clock cannot go backwards");
      }
    } else if (strcmp(command, "memory") == 0) {
      sprintf(reply, "Used: %d, Free: %d, Queue: %d",
        getAllocatedHeap(), getFreeHeap(),
        _callbacks->getQueueSize());
    } else if (strcmp(command, "start ota") == 0) {
      if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
      if (!_board->startOTAUpdate(_prefs->node_name, reply)) {
        strcpy(reply, "Error");
      }
    } else if (strcmp(command, "clock") == 0) {
      uint32_t now = getRTCClock()->getCurrentTime();
      DateTime dt = DateTime(now);
      sprintf(reply, "%02d:%02d - %d/%d/%d UTC", dt.hour(), dt.minute(), dt.day(), dt.month(), dt.year());
    } else if (memcmp(command, "time ", 5) == 0) {  // set time (to epoch seconds)
      if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
      uint32_t secs = _atoi(&command[5]);
      uint32_t curr = getRTCClock()->getCurrentTime();
      if (secs > curr) {
        getRTCClock()->setCurrentTime(secs);
        uint32_t now = getRTCClock()->getCurrentTime();
        DateTime dt = DateTime(now);
        sprintf(reply, "OK - clock set: %02d:%02d - %d/%d/%d UTC", dt.hour(), dt.minute(), dt.day(), dt.month(), dt.year());
      } else {
        strcpy(reply, "(ERR: clock cannot go backwards)");
      }
    } else if (strcmp(command, "neighbors") == 0) {
      _callbacks->formatNeighborsReply(reply);
    } else if (memcmp(command, "neighbor.remove ", 16) == 0) {
      const char* hex = &command[16];
      uint8_t pubkey[PUB_KEY_SIZE];
      int hex_len = min((int)strlen(hex), PUB_KEY_SIZE*2);
      int pubkey_len = hex_len / 2;
      if (mesh::Utils::fromHex(pubkey, pubkey_len, hex)) {
        _callbacks->removeNeighbor(pubkey, pubkey_len);
        strcpy(reply, "OK");
      } else {
        strcpy(reply, "ERR: bad pubkey");
      }
    } else if (memcmp(command, "tempradio ", 10) == 0) {
      if (0 != sender_timestamp) goto handleCommandDenied;
      strcpy(tmp, &command[10]);
      const char *parts[5];
      int num = mesh::Utils::parseTextParts(tmp, parts, 5);
      float freq  = num > 0 ? strtof(parts[0], nullptr) : 0.0f;
      float bw    = num > 1 ? strtof(parts[1], nullptr) : 0.0f;
      uint8_t sf  = num > 2 ? atoi(parts[2]) : 0;
      uint8_t cr  = num > 3 ? atoi(parts[3]) : 0;
      int temp_timeout_mins  = num > 4 ? atoi(parts[4]) : 0;
      if (freq >= 300.0f && freq <= 2500.0f && sf >= 5 && sf <= 12 && cr >= 5 && cr <= 8 && bw >= 7.0f && bw <= 500.0f && temp_timeout_mins > 0) {
        _callbacks->applyTempRadioParams(freq, bw, sf, cr, temp_timeout_mins);
        sprintf(reply, "OK - temp params for %d mins", temp_timeout_mins);
      } else {
        strcpy(reply, "Error, invalid params");
      }
    } else if (strcmp(command, "password.protected") == 0) {
      if (0 != sender_timestamp) goto handleCommandDenied;
      memset(_prefs->password_protected, 0, sizeof(_prefs->password_protected));
      savePrefs();
      goto handleCommand_protected_cleared;
    } else if (memcmp(command, "password.protected ", 19) == 0) {
      if (0 != sender_timestamp) goto handleCommandDenied;
      // change protected password
      StrHelper::strncpy(_prefs->password_protected, &command[19], sizeof(_prefs->password_protected));
      savePrefs();
      if (_prefs->password_protected[0]) {
          strcpy(reply, "protected password set");
      } else {
handleCommand_protected_cleared:
          strcpy(reply, "protected password cleared -- lots of commands only allowed via serial cli");
      }
    } else if (memcmp(command, "password ", 9) == 0) {
      if (0 != sender_timestamp) goto handleCommandDenied;
      // change admin password
      StrHelper::strncpy(_prefs->password, &command[9], sizeof(_prefs->password));
      savePrefs();
      sprintf(reply, "password now: %s", _prefs->password);   // echo back just to let admin know for sure!!
    /*
     * GET commands
     */
    } else if (memcmp(command, "get ", 4) == 0) {
      const char* config = &command[4];
      if (strcmp(config, "af") == 0) {
        sprintf(reply, "> %s", StrHelper::ftoa(_prefs->airtime_factor));
      } else if (strcmp(config, "int.thresh") == 0) {
        sprintf(reply, "> %d", (uint32_t) _prefs->interference_threshold);
      } else if (strcmp(config, "agc.reset.interval") == 0) {
        sprintf(reply, "> %d", ((uint32_t) _prefs->agc_reset_interval) * 4);
      } else if (strcmp(config, "multi.acks") == 0) {
        sprintf(reply, "> %d", (uint32_t) _prefs->multi_acks);
      } else if (strcmp(config, "allow.read.only") == 0) {
        sprintf(reply, "> %s", _prefs->allow_read_only ? "on" : "off");
      } else if (strcmp(config, "flood.advert.interval") == 0) {
        sprintf(reply, "> %d", ((uint32_t) _prefs->flood_advert_interval));
      } else if (strcmp(config, "silent") == 0) {
          if (_prefs->silent_running) {
              strcpy(reply, "> ");
              strcpy(reply, REPLY_SILENT_RUNNING_ON);
          } else {
              strcpy(reply, "> ");
              strcat(reply, REPLY_SILENT_RUNNING_OFF);
          }
      } else if (strcmp(config, "advert.interval") == 0) {
        sprintf(reply, "> %d", ((uint32_t) _prefs->advert_interval) * 2);
      } else if (strcmp(config, "guest.password") == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        sprintf(reply, "> %s", _prefs->guest_password);
      } else if (strcmp(config, "prv.key") == 0) {  // from serial command line only
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        uint8_t prv_key[PRV_KEY_SIZE];
        int len = _callbacks->getSelfId().writeTo(prv_key, PRV_KEY_SIZE);
        mesh::Utils::toHex(tmp, prv_key, len);
        sprintf(reply, "> %s", tmp);
      } else if (strcmp(config, "name") == 0) {
        sprintf(reply, "> %s", _prefs->node_name);
      } else if (strcmp(config, "repeat") == 0) {
        sprintf(reply, "> %s", _prefs->disable_fwd ? "off" : "on");
      } else if (strcmp(config, "lat") == 0) {
        sprintf(reply, "> %s", StrHelper::ftoa(_prefs->node_lat));
      } else if (strcmp(config, "lon") == 0) {
        sprintf(reply, "> %s", StrHelper::ftoa(_prefs->node_lon));
      } else if (strcmp(config, "radio") == 0) {
        char freq[16], bw[16];
        strcpy(freq, StrHelper::ftoa(_prefs->freq));
        strcpy(bw, StrHelper::ftoa3(_prefs->bw));
        sprintf(reply, "> %s,%s,%d,%d", freq, bw, (uint32_t)_prefs->sf, (uint32_t)_prefs->cr);
#if defined(USE_SX1262) || defined(USE_SX1268)
      } else if (strcmp(config, "radio.rxgain") == 0) {
        sprintf(reply, "> %s", _prefs->rx_boosted_gain ? "on" : "off");
#endif
      } else if (strcmp(config, "rxdelay") == 0) {
        sprintf(reply, "> %s", StrHelper::ftoa(_prefs->rx_delay_base));
      } else if (strcmp(config, "txdelay") == 0) {
        sprintf(reply, "> %s", StrHelper::ftoa(_prefs->tx_delay_factor));
      } else if (strcmp(config, "flood.max") == 0) {
        sprintf(reply, "> %d", (uint32_t)_prefs->flood_max);
      } else if (strcmp(config, "direct.txdelay") == 0) {
        sprintf(reply, "> %s", StrHelper::ftoa(_prefs->direct_tx_delay_factor));
      } else if (strcmp(config, "owner.info") == 0) {
        *reply++ = '>';
        *reply++ = ' ';
        const char* sp = _prefs->owner_info;
        while (*sp) {
          *reply++ = (*sp == '\n') ? '|' : *sp;    // translate newline back to orig '|'
          sp++;
        }
        *reply = 0;  // set null terminator
      } else if (strcmp(config, "path.hash.mode") == 0) {
        sprintf(reply, "> %d", (uint32_t)_prefs->path_hash_mode);
      } else if (strcmp(config, "loop.detect") == 0) {
        if (_prefs->loop_detect == LOOP_DETECT_OFF) {
          strcpy(reply, "> off");
        } else if (_prefs->loop_detect == LOOP_DETECT_MINIMAL) {
          strcpy(reply, "> minimal");
        } else if (_prefs->loop_detect == LOOP_DETECT_MODERATE) {
          strcpy(reply, "> moderate");
        } else {
          strcpy(reply, "> strict");
        }
      } else if (strcmp(config, "tx") == 0 && (config[2] == 0 || config[2] == ' ')) {
        sprintf(reply, "> %d", (int32_t) _prefs->tx_power_dbm);
      } else if (strcmp(config, "freq") == 0) {
        sprintf(reply, "> %s", StrHelper::ftoa(_prefs->freq));
      } else if (strcmp(config, "public.key") == 0) {
        strcpy(reply, "> ");
        mesh::Utils::toHex(&reply[2], _callbacks->getSelfId().pub_key, PUB_KEY_SIZE);
      } else if (strcmp(config, "role") == 0) {
        sprintf(reply, "> %s", _callbacks->getRole());
      } else if (strcmp(config, "bridge.type") == 0) {
        sprintf(reply, "> %s",
#ifdef WITH_RS232_BRIDGE
                "rs232"
#elif WITH_ESPNOW_BRIDGE
                "espnow"
#else
                "none"
#endif
        );
#ifdef WITH_BRIDGE
      } else if (strcmp(config, "bridge.enabled") == 0) {
        sprintf(reply, "> %s", _prefs->bridge_enabled ? "on" : "off");
      } else if (strcmp(config, "bridge.delay") == 0) {
        sprintf(reply, "> %d", (uint32_t)_prefs->bridge_delay);
      } else if (strcmp(config, "bridge.source") == 0) {
        sprintf(reply, "> %s", _prefs->bridge_pkt_src ? "logRx" : "logTx");
#endif
#ifdef WITH_RS232_BRIDGE
      } else if (strcmp(config, "bridge.baud") == 0) {
        sprintf(reply, "> %d", (uint32_t)_prefs->bridge_baud);
#endif
#ifdef WITH_ESPNOW_BRIDGE
      } else if (strcmp(config, "bridge.channel") == 0) {
        sprintf(reply, "> %d", (uint32_t)_prefs->bridge_channel);
      } else if (strcmp(config, "bridge.secret") == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        sprintf(reply, "> %s", _prefs->bridge_secret);
#endif
#ifdef WITH_MQTT_BRIDGE
      } else if (strcmp(config, "mqtt.origin") == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        sprintf(reply, "> %s", _prefs->mqtt_origin);
      } else if (strcmp(config, "mqtt.iata") == 0) {
        sprintf(reply, "> %s", _prefs->mqtt_iata);
      } else if (strcmp(config, "mqtt.status") == 0) {
        sprintf(reply, "> %s", _prefs->mqtt_status_enabled ? "on" : "off");
      } else if (strcmp(config, "mqtt.packets") == 0) {
        sprintf(reply, "> %s", _prefs->mqtt_packets_enabled ? "on" : "off");
              } else if (strcmp(config, "mqtt.raw") == 0) {
                sprintf(reply, "> %s", _prefs->mqtt_raw_enabled ? "on" : "off");
              } else if (strcmp(config, "mqtt.tx") == 0) {
                sprintf(reply, "> %s", _prefs->mqtt_tx_enabled ? "on" : "off");
              } else if (strcmp(config, "mqtt.interval") == 0) {
                // Display interval in minutes (rounded)
                uint32_t minutes = (_prefs->mqtt_status_interval + 29999) / 60000; // Round up
                sprintf(reply, "> %u minutes (%lu ms)", minutes, _prefs->mqtt_status_interval);
              } else if (strcmp(config, "mqtt.server") == 0) {
                if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
                sprintf(reply, "> %s", _prefs->mqtt_server);
              } else if (strcmp(config, "mqtt.port") == 0) {
                if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
                sprintf(reply, "> %d", _prefs->mqtt_port);
              } else if (strcmp(config, "mqtt.username") == 0) {
                if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
                sprintf(reply, "> %s", _prefs->mqtt_username);
              } else if (strcmp(config, "mqtt.password") == 0) {
                if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
                sprintf(reply, "> %s", _prefs->mqtt_password);
              } else if (strcmp(config, "wifi.ntp.server") == 0) {
                if (0 != sender_timestamp) goto handleCommandDenied;
                sprintf(reply, "> %s", _prefs->wifi_ntp_server);
              } else if (strcmp(config, "wifi.ntp.enabled") == 0) {
                sprintf(reply, "> %s", _prefs->wifi_ntp_enabled ? "on" : "off");
              } else if (strcmp(config, "wifi.ssid") == 0) {
                if (0 != sender_timestamp) goto handleCommandDenied;
                sprintf(reply, "> %s", _prefs->wifi_ssid);
              } else if (strcmp(config, "wifi.pwd") == 0) {
                if (0 != sender_timestamp) goto handleCommandDenied;
                sprintf(reply, "> %s", _prefs->wifi_password);
              } else if (strcmp(config, "wifi.status") == 0) {
                wl_status_t status = WiFi.status();
                const char* status_str;
                switch(status) {
                  case WL_CONNECTED: status_str = "connected"; break;
                  case WL_NO_SSID_AVAIL: status_str = "no_ssid"; break;
                  case WL_CONNECT_FAILED: status_str = "connect_failed"; break;
                  case WL_CONNECTION_LOST: status_str = "connection_lost"; break;
                  case WL_DISCONNECTED: status_str = "disconnected"; break;
                  default: status_str = "unknown"; break;
                }
                if (status == WL_CONNECTED) {
                  if (!allowProtectedCommand(sender_timestamp)) {
                      sprintf(reply, "> %s, RSSI: %d dBm", status_str, WiFi.RSSI());
                  } else {
                      sprintf(reply, "> %s, IP: %s, RSSI: %d dBm", status_str, WiFi.localIP().toString().c_str(), WiFi.RSSI());
                  }
                } else {
                  sprintf(reply, "> %s (code: %d)", status_str, status);
                }
              } else if (strcmp(config, "wifi.powersave") == 0) {
                uint8_t ps = _prefs->wifi_power_save;
                const char* ps_name = (ps == 1) ? "none" : (ps == 2) ? "max" : "min";
                sprintf(reply, "> %s", ps_name);
              } else if (strcmp(config, "wifi.telnet.enabled") == 0) {
                sprintf(reply, "> %s", _prefs->wifi_telnet_enabled ? "on" : "off");
              } else if (strcmp(config, "wifi.telnet.timeout") == 0) {
                sprintf(reply, "> %d second(s)", _prefs->wifi_telnet_timeout);
              } else if (strcmp(config, "timezone.offset") == 0) {
                sprintf(reply, "> %d", _prefs->timezone_offset);
              } else if (strcmp(config, "timezone.string") == 0) {
                sprintf(reply, "> %s", _prefs->timezone_string);
              } else if (strcmp(config, "mqtt.analyzer.us") == 0) {
                sprintf(reply, "> %s", _prefs->mqtt_analyzer_us_enabled ? "on" : "off");
              } else if (strcmp(config, "mqtt.analyzer.eu") == 0) {
                sprintf(reply, "> %s", _prefs->mqtt_analyzer_eu_enabled ? "on" : "off");
              } else if (strcmp(config, "mqtt.owner") == 0) {  // from serial command line only
                if (_prefs->mqtt_owner_public_key[0] != '\0') {
                  sprintf(reply, "> %s", _prefs->mqtt_owner_public_key);
                } else {
                  strcpy(reply, "> (not set)");
                }
              } else if (strcmp(config, "mqtt.email") == 0) {  // from serial command line only
                if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;

                if (_prefs->mqtt_email[0] != '\0') {
                  sprintf(reply, "> %s", _prefs->mqtt_email);
                } else {
                  strcpy(reply, "> (not set)");
                }
              } else if (strcmp(config, "mqtt.config.valid") == 0) {
                bool valid = MQTTBridge::isConfigValid(_prefs);
                sprintf(reply, "> %s", valid ? "valid" : "invalid");
              } else if (strcmp(config, "mqtt.remote") == 0) {
                sprintf(reply, "> %s", _prefs->mqtt_remote_enabled ? "on" : "off");
              } else if (strcmp(config, "mqtt.useacl") == 0) {
                sprintf(reply, "> %s", _prefs->mqtt_use_acl ? "on" : "off");
              } else if (strcmp(config, "mqtt.admin") == 0) {
                if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;

                // Only from serial command line (not remote)
                if (_prefs->mqtt_admin_public_key[0] != '\0') {
                  sprintf(reply, "> %s", _prefs->mqtt_admin_public_key);
                } else {
                  strcpy(reply, "> (not set)");
                }
#endif
      } else if (strcmp(config, "bootloader.ver") == 0) {
#ifdef NRF52_PLATFORM
          char ver[32];
          if (_board->getBootloaderVersion(ver, sizeof(ver))) {
              sprintf(reply, "> %s", ver);
          } else {
              strcpy(reply, "> unknown");
          }
#else
          strcpy(reply, "ERROR: unsupported");
#endif
      } else if (strcmp(config, "adc.multiplier") == 0) {
        float adc_mult = _board->getAdcMultiplier();
        if (adc_mult == 0.0f) {
          strcpy(reply, "Error: unsupported by this board");
        } else {
          sprintf(reply, "> %.3f", adc_mult);
        }
      // Power management commands
      } else if (strcmp(config, "pwrmgt.support") == 0) {
#ifdef NRF52_POWER_MANAGEMENT
        strcpy(reply, "> supported");
#else
        strcpy(reply, "> unsupported");
#endif
      } else if (strcmp(config, "pwrmgt.source") == 0) {
#ifdef NRF52_POWER_MANAGEMENT
        strcpy(reply, _board->isExternalPowered() ? "> external" : "> battery");
#else
        strcpy(reply, "ERROR: Power management not supported");
#endif
      } else if (strcmp(config, "pwrmgt.bootreason") == 0) {
#ifdef NRF52_POWER_MANAGEMENT
        sprintf(reply, "> Reset: %s; Shutdown: %s",
          _board->getResetReasonString(_board->getResetReason()),
          _board->getShutdownReasonString(_board->getShutdownReason()));
#else
        strcpy(reply, "ERROR: Power management not supported");
#endif
      } else if (strcmp(config, "pwrmgt.bootmv") == 0) {
#ifdef NRF52_POWER_MANAGEMENT
        sprintf(reply, "> %u mV", _board->getBootVoltage());
#else
        strcpy(reply, "ERROR: Power management not supported");
#endif
      } else {
        sprintf(reply, "??: %s", config);
      }
    /*
    * CLEAR commands
    */
    } else if (strcmp(command, "clear") == 0) {
handleCommandHelpClear:
      // this is near 160 characters DO NOT ADD MORE //
      strcpy(reply, "Possible: allow.{protected|read.only}, guest.password, owner.info,\n"
#ifdef WITH_MQTT_BRIDGE
        "mqtt: email, iata, origin, owner, password, server, username\n"
        "wifi: ntp.server, pwd, ssid"
#endif
      );
    } else if (memcmp(command, "clear ", 6) == 0) {
      const char* config = &command[6];
      if (strcmp(config, "allow.protected") == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        gl_allow_protected_over_remote = 0;
        sprintf(reply, "OK: disabled protected mode");
      } else if (strcmp(config, "allow.read.only") == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        _prefs->allow_read_only = 0;
        goto handleCommandClearedAndSave;
      } else if (strcmp(config, "guest.password") == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        *_prefs->guest_password = 0;
        goto handleCommandClearedAndSave;
      } else if (strcmp(config, "owner.info") == 0) {
        if (0 != sender_timestamp) goto handleCommandDenied;
        *_prefs->owner_info = 0;
        goto handleCommandClearedAndSave;
      } else if (strcmp(config, "stats") == 0) {
        _callbacks->clearStats();
        strcpy(reply, "(OK - stats reset)");
#ifdef WITH_MQTT_BRIDGE
      } else if (strcmp(config, "mqtt") == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        *_prefs->mqtt_email = 0;
        *_prefs->mqtt_iata = 0;
        *_prefs->mqtt_origin = 0;
        *_prefs->mqtt_owner_public_key = 0;
        *_prefs->mqtt_password = 0;
        *_prefs->mqtt_server = 0;
        *_prefs->mqtt_username = 0;
        goto handleCommandClearedAndSave;
      } else if (strcmp(config, "mqtt.email") == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        *_prefs->mqtt_email = 0;
        goto handleCommandClearedAndSave;
      } else if (strcmp(config, "mqtt.iata") == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        *_prefs->mqtt_iata = 0;
        goto handleCommandClearedAndSave;
      } else if (strcmp(config, "mqtt.origin") == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        *_prefs->mqtt_origin = 0;
        goto handleCommandClearedAndSave;
      } else if (strcmp(config, "mqtt.owner") == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        *_prefs->mqtt_owner_public_key = 0;
        goto handleCommandClearedAndSave;
      } else if (strcmp(config, "mqtt.password") == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        *_prefs->mqtt_password = 0;
        goto handleCommandClearedAndSave;
      } else if (strcmp(config, "mqtt.server") == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        *_prefs->mqtt_server = 0;
        goto handleCommandClearedAndSave;
      } else if (strcmp(config, "mqtt.username") == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        *_prefs->mqtt_username = 0;
        goto handleCommandClearedAndSave;
      } else if (strcmp(config, "timezone.string") == 0) {
        if (0 != sender_timestamp) goto handleCommandDenied;
        *_prefs->timezone_string = 0;
        goto handleCommandClearedAndSave;
      } else if (strcmp(config, "wifi") == 0) {
        if (0 != sender_timestamp) goto handleCommandDenied;
        *_prefs->wifi_ntp_server = 0;
        *_prefs->wifi_password = 0;
        *_prefs->wifi_ssid = 0;
        goto handleCommandClearedAndSave;
      } else if (strcmp(config, "wifi.ntp.server") == 0) {
        if (0 != sender_timestamp) goto handleCommandDenied;
        *_prefs->wifi_ntp_server = 0;
        goto handleCommandClearedAndSave;
      } else if (strcmp(config, "wifi.pwd") == 0) {
        if (0 != sender_timestamp) goto handleCommandDenied;
        *_prefs->wifi_password = 0;
        goto handleCommandClearedAndSave;
      } else if (strcmp(config, "wifi.ssid") == 0) {
        if (0 != sender_timestamp) goto handleCommandDenied;
        *_prefs->wifi_ssid = 0;
        goto handleCommandClearedAndSave;
#endif
      } else {
        strcpy(reply, "> unknown setting");
      }
    } else if (strcmp(command, "disable") == 0) {
handleCommandHelpDisable:
      strcpy(reply, "Possible: bridge, silent"
#ifdef WITH_MQTT_BRIDGE
        "mqtt: analyzer.eu, analyzer.us, packets, raw, status, tx,\n"
        "wifi: ntp, powersave"
#endif
      );
    } else if (memcmp(command, "disable ", 8) == 0) {
      const char* config = &command[8];
      if (strcmp(config, "bridge") == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        _prefs->bridge_enabled = 0;
        goto handleCommandDisabledAndSave;
    } else if (strcmp(config, "silent") == 0) {
      if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
      _prefs->silent_running = 0;
      goto handleCommandDisabledAndSave;
#ifdef WITH_MQTT_BRIDGE
      } else if (strcmp(config, "mqtt.analyzer.eu") == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        _prefs->mqtt_analyzer_eu_enabled = 0;
        goto handleCommandDisabledAndSave;
      } else if (strcmp(config, "mqtt.analyzer.us") == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        _prefs->mqtt_analyzer_us_enabled = 0;
        goto handleCommandDisabledAndSave;
      } else if (strcmp(config, "mqtt.packets") == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        _prefs->mqtt_packets_enabled = 0;
        goto handleCommandDisabledAndSave;
      } else if (strcmp(config, "mqtt.raw") == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        _prefs->mqtt_raw_enabled = 0;
        goto handleCommandDisabledAndSave;
      } else if (strcmp(config, "mqtt.status") == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        _prefs->mqtt_status_enabled = 0;
        goto handleCommandDisabledAndSave;
      } else if (strcmp(config, "mqtt.tx") == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        _prefs->mqtt_tx_enabled = 0;
        goto handleCommandDisabledAndSave;
      } else if (strcmp(config, "wifi.ntp") == 0) {
          if (0 != sender_timestamp) goto handleCommandDenied;
        _prefs->wifi_ntp_enabled = 0;
        goto handleCommandDisabledAndSave;
      } else if (strcmp(config, "wifi.powersave") == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        _prefs->wifi_power_save = 0;
        goto handleCommandDisabledAndSave;
      } else if (strcmp(config, "wifi.telnet") == 0) {
        if (0 != sender_timestamp) goto handleCommandDenied;
        _prefs->wifi_telnet_enabled = 0;
        goto handleCommandDisabledAndSaveNeedReboot;
#endif
      } else {
          strcpy(reply, "> unknown feature");
      }
    /*
     * SET commands
     */
    } else if (memcmp(command, "set ", 4) == 0) {
      const char* config = &command[4];
      if (memcmp(config, "allow.protected ", 16) == 0) {
        if (*_prefs->password_protected &&
            strcmp(&config[16], _prefs->password_protected) == 0)
        {
            gl_allow_protected_over_remote = getRTCClock()->getCurrentTime();
            sprintf(reply, "OK: protected mode enabled for %d second(s)", PROTECTED_TIME_DURATION);
        } else {
            strcpy(reply, "Invalid protected command!");
        }
      } else if (memcmp(config, "af ", 3) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        _prefs->airtime_factor = atof(&config[3]);
        savePrefs();
        strcpy(reply, "OK");
      } else if (memcmp(config, "int.thresh ", 11) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        _prefs->interference_threshold = atoi(&config[11]);
        savePrefs();
        strcpy(reply, "OK");
      } else if (memcmp(config, "agc.reset.interval ", 19) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        _prefs->agc_reset_interval = atoi(&config[19]) / 4;
        savePrefs();
        sprintf(reply, "OK - interval rounded to %d", ((uint32_t) _prefs->agc_reset_interval) * 4);
      } else if (memcmp(config, "multi.acks ", 11) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        _prefs->multi_acks = atoi(&config[11]);
        savePrefs();
        strcpy(reply, "OK");
      } else if (memcmp(config, "allow.read.only ", 16) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        _prefs->allow_read_only = memcmp(&config[16], "on", 2) == 0;
        savePrefs();
        strcpy(reply, "OK");
      } else if (memcmp(config, "flood.advert.interval ", 22) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        int hours = _atoi(&config[22]);
        if ((hours > 0 && hours < 3) || (hours > 168)) {
          strcpy(reply, "Error: interval range is 3-168 hours");
        } else {
          _prefs->flood_advert_interval = (uint8_t)(hours);
          _callbacks->updateFloodAdvertTimer();
          savePrefs();
          strcpy(reply, "OK");
        }
      } else if (memcmp(config, "silent ", 7) == 0) {
          if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
          _prefs->silent_running = strcmp(&config[7], "on") == 0;
          if (_prefs->silent_running) {
              strcpy(reply, "OK - Silent Running (ignores discovery requests and sends no advertisements)");
          } else {
handleCommandSilentCleared:
              strcpy(reply, "OK - Regular Running");
          }
      } else if (memcmp(config, "advert.interval ", 16) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        int mins = _atoi(&config[16]);
        if ((mins > 0 && mins < MIN_LOCAL_ADVERT_INTERVAL) || (mins > 240)) {
          sprintf(reply, "Error: interval range is %d-240 minutes", MIN_LOCAL_ADVERT_INTERVAL);
        } else {
          _prefs->advert_interval = (uint8_t)(mins / 2);
          _callbacks->updateAdvertTimer();
          savePrefs();
          strcpy(reply, "OK");
        }
      } else if (memcmp(config, "guest.password ", 15) == 0) {
        StrHelper::strncpy(_prefs->guest_password, &config[15], sizeof(_prefs->guest_password));
        savePrefs();
        strcpy(reply, "OK");
      } else if (memcmp(config, "prv.key ", 8) == 0) {
        if (0 != sender_timestamp) goto handleCommandDenied;

        uint8_t prv_key[PRV_KEY_SIZE];
        bool success = mesh::Utils::fromHex(prv_key, PRV_KEY_SIZE, &config[8]);
        // only allow rekey if key is valid
        if (success && mesh::LocalIdentity::validatePrivateKey(prv_key)) {
          mesh::LocalIdentity new_id;
          new_id.readFrom(prv_key, PRV_KEY_SIZE);
          _callbacks->saveIdentity(new_id);
          strcpy(reply, "OK, reboot to apply! New pubkey: ");
          mesh::Utils::toHex(&reply[33], new_id.pub_key, PUB_KEY_SIZE);
        } else {
          strcpy(reply, "Error, bad key");
        }
      } else if (memcmp(config, "name ", 5) == 0) {
        if (0 != sender_timestamp) goto handleCommandDenied;
        if (isValidName(&config[5])) {
          StrHelper::strncpy(_prefs->node_name, &config[5], sizeof(_prefs->node_name));
          savePrefs();
          strcpy(reply, "OK");
        } else {
          strcpy(reply, "Error, bad chars");
        }
      } else if (memcmp(config, "repeat ", 7) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        _prefs->disable_fwd = memcmp(&config[7], "off", 3) == 0;
        savePrefs();
        strcpy(reply, _prefs->disable_fwd ? "OK - repeat is now OFF" : "OK - repeat is now ON");
#if defined(USE_SX1262) || defined(USE_SX1268)
      } else if (memcmp(config, "radio.rxgain ", 13) == 0) {
        _prefs->rx_boosted_gain = memcmp(&config[13], "on", 2) == 0;
        strcpy(reply, "OK");
        savePrefs();
        _callbacks->setRxBoostedGain(_prefs->rx_boosted_gain);
#endif
      } else if (memcmp(config, "radio ", 6) == 0) {
        if (0 != sender_timestamp) goto handleCommandDenied;
        if (strcmp(&config[6], "au") == 0) {
          // set austrailia/new zealand radio settings //
          _prefs->freq = 915.8;
          _prefs->bw = 250.0;
          _prefs->sf = 10;
          _prefs->cr = 5;
handleCommandSetRadioSave:
          _callbacks->savePrefs();
          strcpy(reply, "OK - reboot to apply");
        } else if (strcmp(&config[6], "eu") == 0) {
          // set us radio settings //
          _prefs->freq = 869.525;
          _prefs->bw = 250.0;
          _prefs->sf = 11;
          _prefs->cr = 5;
          goto handleCommandSetRadioSave;
        } else if (strcmp(&config[6], "us") == 0) {
          // set united states radio settings //
          _prefs->freq = 910.525;
          _prefs->bw = 62.5;
          _prefs->sf = 7;
          _prefs->cr = 5;
          goto handleCommandSetRadioSave;
        } else {
          strcpy(tmp, &config[6]);
          const char *parts[4];
          int num = mesh::Utils::parseTextParts(tmp, parts, 4);
          float freq  = num > 0 ? strtof(parts[0], nullptr) : 0.0f;
          float bw    = num > 1 ? strtof(parts[1], nullptr) : 0.0f;
          uint8_t sf  = num > 2 ? atoi(parts[2]) : 0;
          uint8_t cr  = num > 3 ? atoi(parts[3]) : 0;
          if (freq >= 300.0f && freq <= 2500.0f && sf >= 5 && sf <= 12 && cr >= 5 && cr <= 8 && bw >= 7.0f && bw <= 500.0f) {
            _prefs->sf = sf;
            _prefs->cr = cr;
            _prefs->freq = freq;
            _prefs->bw = bw;
            goto handleCommandSetRadioSave;
          } else {
            strcpy(reply, "Error, invalid radio params: au, eu, us, or freq,bw,sf,cr");
          }
        }
      } else if (memcmp(config, "lat ", 4) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        _prefs->node_lat = atof(&config[4]);
        savePrefs();
        strcpy(reply, "OK");
      } else if (memcmp(config, "lon ", 4) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        _prefs->node_lon = atof(&config[4]);
        savePrefs();
        strcpy(reply, "OK");
      } else if (memcmp(config, "rxdelay ", 8) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        float db = atof(&config[8]);
        if (db >= 0) {
          _prefs->rx_delay_base = db;
          savePrefs();
          strcpy(reply, "OK");
        } else {
          strcpy(reply, "Error, cannot be negative");
        }
      } else if (strcmp(config, "silent") == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        _prefs->silent_running = 0;
        goto handleCommandDisabledAndSave;
        strcat(reply, "OK: ");
        strcat(reply, REPLY_SILENT_RUNNING_OFF);
      } else if (memcmp(config, "txdelay ", 8) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        float f = atof(&config[8]);
        if (f >= 0) {
          _prefs->tx_delay_factor = f;
          savePrefs();
          strcpy(reply, "OK");
        } else {
          strcpy(reply, "Error, cannot be negative");
        }
      } else if (memcmp(config, "flood.max ", 10) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        uint8_t m = atoi(&config[10]);
        if (m <= 64) {
          _prefs->flood_max = m;
          savePrefs();
          strcpy(reply, "OK");
        } else {
          strcpy(reply, "Error, max 64");
        }
      } else if (memcmp(config, "direct.txdelay ", 15) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        float f = atof(&config[15]);
        if (f >= 0) {
          _prefs->direct_tx_delay_factor = f;
          savePrefs();
          strcpy(reply, "OK");
        } else {
          strcpy(reply, "Error, cannot be negative");
        }
      } else if (memcmp(config, "owner.info ", 11) == 0) {
        if (0 != sender_timestamp) goto handleCommandDenied;

        config += 11;
        char *dp = _prefs->owner_info;
        while (*config && dp - _prefs->owner_info < sizeof(_prefs->owner_info)-1) {
          *dp++ = (*config == '|') ? '\n' : *config;    // translate '|' to newline chars
          config++;
        }
        *dp = 0;
        savePrefs();
        strcpy(reply, "OK");
      } else if (memcmp(config, "path.hash.mode ", 15) == 0) {
        config += 15;
        uint8_t mode = atoi(config);
        if (mode < 3) {
          _prefs->path_hash_mode = mode;
          savePrefs();
          strcpy(reply, "OK");
        } else {
          strcpy(reply, "Error, must be 0,1, or 2");
        }
      } else if (memcmp(config, "loop.detect ", 12) == 0) {
        config += 12;
        uint8_t mode;
        if (strcmp(config, "off") == 0) {
          mode = LOOP_DETECT_OFF;
        } else if (strcmp(config, "minimal") == 0) {
          mode = LOOP_DETECT_MINIMAL;
        } else if (strcmp(config, "moderate") == 0) {
          mode = LOOP_DETECT_MODERATE;
        } else if (strcmp(config, "strict") == 0) {
          mode = LOOP_DETECT_STRICT;
        } else {
          mode = 0xFF;
          strcpy(reply, "Error, must be: off, minimal, moderate, or strict");
        }
        if (mode != 0xFF) {
          _prefs->loop_detect = mode;
          savePrefs();
          strcpy(reply, "OK");
        }
      } else if (memcmp(config, "tx ", 3) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        _prefs->tx_power_dbm = atoi(&config[3]);
        savePrefs();
        _callbacks->setTxPower(_prefs->tx_power_dbm);
        strcpy(reply, "OK");
      } else if (memcmp(config, "freq ", 5) == 0) {
        if (0 != sender_timestamp) goto handleCommandDenied;

        _prefs->freq = atof(&config[5]);
        savePrefs();
        strcpy(reply, "OK - reboot to apply");
#ifdef WITH_BRIDGE
      } else if (memcmp(config, "bridge.enabled ", 15) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        _prefs->bridge_enabled = strcmp(&config[15], "on") == 0;
        _callbacks->setBridgeState(_prefs->bridge_enabled);
        savePrefs();
        strcpy(reply, "OK");
      } else if (memcmp(config, "bridge.delay ", 13) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        int delay = _atoi(&config[13]);
        if (delay >= 0 && delay <= 10000) {
          _prefs->bridge_delay = (uint16_t)delay;
          savePrefs();
          strcpy(reply, "OK");
        } else {
          strcpy(reply, "Error: delay must be between 0-10000 ms");
        }
      } else if (memcmp(config, "bridge.source ", 14) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        _prefs->bridge_pkt_src = strcmp(&config[14], "rx") == 0;
        savePrefs();
        strcpy(reply, "OK");
#endif
#ifdef WITH_RS232_BRIDGE
      } else if (memcmp(config, "bridge.baud ", 12) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        uint32_t baud = atoi(&config[12]);
        if (baud >= 9600 && baud <= 115200) {
          _prefs->bridge_baud = (uint32_t)baud;
          _callbacks->restartBridge();
          savePrefs();
          strcpy(reply, "OK");
        } else {
          strcpy(reply, "Error: baud rate must be between 9600-115200");
        }
#endif
#ifdef WITH_ESPNOW_BRIDGE
      } else if (memcmp(config, "bridge.channel ", 15) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        int ch = atoi(&config[15]);
        if (ch > 0 && ch < 15) {
          _prefs->bridge_channel = (uint8_t)ch;
          _callbacks->restartBridge();
          savePrefs();
          strcpy(reply, "OK");
        } else {
          strcpy(reply, "Error: channel must be between 1-14");
        }
      } else if (memcmp(config, "bridge.secret ", 14) == 0) {
        StrHelper::strncpy(_prefs->bridge_secret, &config[14], sizeof(_prefs->bridge_secret));
        _callbacks->restartBridge();
        savePrefs();
        strcpy(reply, "OK");
#endif
#ifdef WITH_MQTT_BRIDGE
      } else if (memcmp(config, "mqtt.origin ", 12) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        StrHelper::strncpy(_prefs->mqtt_origin, &config[12], sizeof(_prefs->mqtt_origin));
        savePrefs();
        strcpy(reply, "OK");
      } else if (memcmp(config, "mqtt.iata ", 10) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        StrHelper::strncpy(_prefs->mqtt_iata, &config[10], sizeof(_prefs->mqtt_iata));
        // Convert IATA code to uppercase (IATA codes are conventionally uppercase)
        for (int i = 0; _prefs->mqtt_iata[i]; i++) {
          _prefs->mqtt_iata[i] = toupper(_prefs->mqtt_iata[i]);
        }
        savePrefs();
        strcpy(reply, "OK");
      } else if (memcmp(config, "mqtt.status ", 12) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        _prefs->mqtt_status_enabled = memcmp(&config[12], "on", 2) == 0;
        if (!_prefs->mqtt_status_enabled) goto handleCommandDisabledAndSave;
        savePrefs();
        strcpy(reply, "OK");
      } else if (memcmp(config, "mqtt.packets ", 13) == 0) {
        if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
        _prefs->mqtt_packets_enabled = strcmp(&config[13], "on") == 0;
        if (!_prefs->mqtt_packets_enabled) goto handleCommandDisabledAndSave;
        savePrefs();
        strcpy(reply, "OK");
              } else if (memcmp(config, "mqtt.raw ", 9) == 0) {
                if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
                _prefs->mqtt_raw_enabled = strcmp(&config[9], "on") == 0;
                if (!_prefs->mqtt_raw_enabled) goto handleCommandDisabledAndSave;
                savePrefs();
                strcpy(reply, "OK");
              } else if (memcmp(config, "mqtt.tx ", 8) == 0) {
                if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
                _prefs->mqtt_tx_enabled = strcmp(&config[8], "on") == 0;
                if (!_prefs->mqtt_tx_enabled) goto handleCommandDisabledAndSave;
                savePrefs();
                strcpy(reply, "OK");
              } else if (memcmp(config, "mqtt.interval ", 14) == 0) {
                if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
                uint32_t minutes = _atoi(&config[14]);
                if (minutes >= 1 && minutes <= 60) { // 1 minute to 60 minutes
                  _prefs->mqtt_status_interval = minutes * 60000; // Convert minutes to milliseconds
                  savePrefs();
                  // Restart bridge to pick up new interval value
                  _callbacks->restartBridge();
                  sprintf(reply, "OK - interval set to %u minutes (%lu ms), bridge restarted", minutes, _prefs->mqtt_status_interval);
                } else {
                  strcpy(reply, "Error: interval must be between 1-60 minutes");
                }
              } else if (memcmp(config, "wifi.ntp.enabled ", 17) == 0) {
                if (0 != sender_timestamp) goto handleCommandDenied;
                _prefs->wifi_ntp_enabled = strcmp(&config[17], "on") == 0;
                if (!_prefs->wifi_ntp_enabled) goto handleCommandDisabledAndSave;
                savePrefs();
                strcpy(reply, "OK");
              } else if (memcmp(config, "wifi.ntp.server ", 16) == 0) {
                if (0 != sender_timestamp) goto handleCommandDenied;
                StrHelper::strncpy(_prefs->wifi_ntp_server, &config[16], sizeof(_prefs->wifi_ntp_server));
                savePrefs();
                strcpy(reply, "OK");
              } else if (memcmp(config, "wifi.ssid ", 10) == 0) {
                if (0 != sender_timestamp) goto handleCommandDenied;
                StrHelper::strncpy(_prefs->wifi_ssid, &config[10], sizeof(_prefs->wifi_ssid));
                savePrefs();
                strcpy(reply, "OK");
              } else if (memcmp(config, "wifi.pwd ", 9) == 0) {
                if (0 != sender_timestamp) goto handleCommandDenied;
                StrHelper::strncpy(_prefs->wifi_password, &config[9], sizeof(_prefs->wifi_password));
                savePrefs();
                strcpy(reply, "OK");
              } else if (memcmp(config, "wifi.powersave ", 15) == 0) {
                if (0 != sender_timestamp) goto handleCommandDenied;
                const char* value = &config[15];
                uint8_t ps_value;
                bool valid = false;
                if (strcmp(value, "min") == 0 && (value[3] == 0 || value[3] == ' ')) {
                  ps_value = 0;
                  valid = true;
                } else if (strcmp(value, "none") == 0 && (value[4] == 0 || value[4] == ' ')) {
                  ps_value = 1;
                  valid = true;
                } else if (strcmp(value, "max") == 0 && (value[3] == 0 || value[3] == ' ')) {
                  ps_value = 2;
                  valid = true;
                }
                
                if (!valid) {
                  strcpy(reply, "Error: must be none, min, or max");
                } else {
                  _prefs->wifi_power_save = ps_value;
                  if (1 == _prefs->wifi_power_save) goto handleCommandDisabledAndSave;
                  savePrefs();
                  
                  // Apply immediately if WiFi is connected
                  #ifdef ESP_PLATFORM
                  if (WiFi.status() == WL_CONNECTED) {
                    wifi_ps_type_t ps_mode = (ps_value == 1) ? WIFI_PS_NONE : 
                                            (ps_value == 2) ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM;
                    esp_err_t ps_result = esp_wifi_set_ps(ps_mode);
                    if (ps_result == ESP_OK) {
                      const char* ps_name = (ps_value == 1) ? "none" : (ps_value == 2) ? "max" : "min";
                      sprintf(reply, "OK - power save set to %s", ps_name);
                    } else {
                      sprintf(reply, "OK - saved, but failed to apply: %d", ps_result);
                    }
                  } else {
                    const char* ps_name = (ps_value == 1) ? "none" : (ps_value == 2) ? "max" : "min";
                    sprintf(reply, "OK - saved as %s (will apply on next WiFi connection)", ps_name);
                  }
                  #else
                  const char* ps_name = (ps_value == 1) ? "none" : (ps_value == 2) ? "max" : "min";
                  sprintf(reply, "OK - saved as %s", ps_name);
                  #endif
                }
              } else if (memcmp(config, "wifi.telnet.enabled ", 20) == 0) {
                if (0 != sender_timestamp) goto handleCommandDenied;
                _prefs->wifi_telnet_enabled = strcmp(&config[20], "on") == 0;
                if (!_prefs->wifi_telnet_enabled) goto handleCommandDisabledAndSaveNeedReboot;
                savePrefs();
                strcpy(reply, "OK - Enabled telnet. Reboot is required.");
              } else if (memcmp(config, "wifi.telnet.timeout ", 20) == 0) {
                if (0 != sender_timestamp) goto handleCommandDenied;
                _prefs->wifi_telnet_timeout = _atoi(&config[20]);
                savePrefs();
                sprintf(reply, "OK - %d second(s) timeout set", _prefs->wifi_telnet_timeout);
              } else if (memcmp(config, "timezone.string ", 16) == 0) {
                if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
                StrHelper::strncpy(_prefs->timezone_string, &config[9], sizeof(_prefs->timezone_string));
                savePrefs();
                strcpy(reply, "OK");
              } else if (memcmp(config, "timezone.offset ", 16) == 0) {
                if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
                int8_t offset = _atoi(&config[16]);
                if (offset >= -12 && offset <= 14) {
                  _prefs->timezone_offset = offset;
                  savePrefs();
                  strcpy(reply, "OK");
                } else {
                  strcpy(reply, "Error: timezone offset must be between -12 and +14");
                }
              } else if (memcmp(config, "mqtt.server ", 12) == 0) {
                if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
                StrHelper::strncpy(_prefs->mqtt_server, &config[12], sizeof(_prefs->mqtt_server));
                savePrefs();
                strcpy(reply, "OK");
              } else if (memcmp(config, "mqtt.port ", 10) == 0) {
                if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
                int port = atoi(&config[10]);
                if (port > 0 && port <= 65535) {
                  _prefs->mqtt_port = port;
                  savePrefs();
                  strcpy(reply, "OK");
                } else {
                  strcpy(reply, "Error: port must be between 1 and 65535");
                }
              } else if (memcmp(config, "mqtt.username ", 14) == 0) {
                if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
                StrHelper::strncpy(_prefs->mqtt_username, &config[14], sizeof(_prefs->mqtt_username));
                savePrefs();
                strcpy(reply, "OK");
              } else if (memcmp(config, "mqtt.password ", 14) == 0) {
                if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
                StrHelper::strncpy(_prefs->mqtt_password, &config[14], sizeof(_prefs->mqtt_password));
                savePrefs();
                strcpy(reply, "OK");
              } else if (memcmp(config, "mqtt.analyzer.us ", 17) == 0) {
                if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
                _prefs->mqtt_analyzer_us_enabled = strcmp(&config[17], "on") == 0;
                savePrefs();
                strcpy(reply, "OK");
              } else if (memcmp(config, "mqtt.analyzer.eu ", 17) == 0) {
                if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
                _prefs->mqtt_analyzer_eu_enabled = strcmp(&config[17], "on") == 0;
                savePrefs();
                strcpy(reply, "OK");
              } else if (memcmp(config, "mqtt.owner ", 11) == 0) {
                if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
                // Validate that it's a valid hex string of the correct length (PUB_KEY_SIZE * 2 hex chars = PUB_KEY_SIZE bytes)
                const char* owner_key = &config[11];
                if (isValidPublicKeyHex(owner_key)) {
                  StrHelper::strncpy(_prefs->mqtt_owner_public_key, owner_key, sizeof(_prefs->mqtt_owner_public_key));
                  savePrefs();
                  strcpy(reply, "OK");
                } else {
                  strcpy(reply, "Error: public key must be 64 hex characters (32 bytes)");
                }
              } else if (memcmp(config, "mqtt.email ", 11) == 0) {
                if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
                StrHelper::strncpy(_prefs->mqtt_email, &config[11], sizeof(_prefs->mqtt_email));
                savePrefs();
                strcpy(reply, "OK");
              } else if (memcmp(config, "mqtt.remote ", 12) == 0) {
                if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
                _prefs->mqtt_remote_enabled = memcmp(&config[12], "on", 2) == 0;
                savePrefs();
                strcpy(reply, "OK");
              } else if (memcmp(config, "mqtt.useacl ", 12) == 0) {
                if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
                _prefs->mqtt_use_acl = strcmp(&config[12], "on") == 0;
                savePrefs();
                strcpy(reply, "OK");
              } else if (memcmp(config, "mqtt.admin ", 11) == 0) {
                if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
                const char* admin_key = &config[11];
                if (strcmp(admin_key, "0") == 0) {
                  // Clear the admin key
                  _prefs->mqtt_admin_public_key[0] = '\0';
                  savePrefs();
                  strcpy(reply, "OK - admin key cleared");
                } else {
                  // Validate that it's a valid hex string of the correct length (PUB_KEY_SIZE * 2 hex chars = PUB_KEY_SIZE bytes)
                  if (isValidPublicKeyHex(admin_key)) {
                    StrHelper::strncpy(_prefs->mqtt_admin_public_key, admin_key, sizeof(_prefs->mqtt_admin_public_key));
                    savePrefs();
                    strcpy(reply, "OK");
                  } else {
                    strcpy(reply, "Error: public key must be 64 hex characters (32 bytes)");
                  }
                }
#endif
      } else {
        sprintf(reply, "unknown config: %s", config);
      }
    } else if (strcmp(command, "erase") == 0) {
      if (0 != sender_timestamp) goto handleCommandDenied;

      bool s = _callbacks->formatFileSystem();
      sprintf(reply, "File system erase: %s", s ? "OK" : "Err");
    } else if (strcmp(command, "ver") == 0) {
      sprintf(reply, "%s (Build: %s)", _callbacks->getFirmwareVer(), _callbacks->getBuildDate());
    } else if (strcmp(command, "board") == 0) {
      sprintf(reply, "%s", _board->getManufacturerName());
    } else if (memcmp(command, "sensor get ", 11) == 0) {
      const char* key = command + 11;
      const char* val = _sensors->getSettingByKey(key);
      if (val != NULL) {
        sprintf(reply, "> %s", val);
      } else {
        strcpy(reply, "null");
      }
    } else if (memcmp(command, "sensor set ", 11) == 0) {
      strcpy(tmp, &command[11]);
      const char *parts[2]; 
      int num = mesh::Utils::parseTextParts(tmp, parts, 2, ' ');
      const char *key = (num > 0) ? parts[0] : "";
      const char *value = (num > 1) ? parts[1] : "null";
      if (_sensors->setSettingValue(key, value)) {
        strcpy(reply, "ok");
      } else {
        strcpy(reply, "can't find custom var");
      }
    } else if (strcmp(command, "sensor list") == 0) {
      char* dp = reply;
      int start = 0;
      int end = _sensors->getNumSettings();
      if (strlen(command) > 11) {
        start = _atoi(command+12);
      }
      if (start >= end) {
        strcpy(reply, "no custom var");
      } else {
        sprintf(dp, "%d vars\n", end);
        dp = strchr(dp, 0);
        int i;
        for (i = start; i < end && (dp-reply < 134); i++) {
          sprintf(dp, "%s=%s\n", 
            _sensors->getSettingName(i),
            _sensors->getSettingValue(i));
          dp = strchr(dp, 0);
        }
        if (i < end) {
          sprintf(dp, "... next:%d", i);
        } else {
          *(dp-1) = 0; // remove last CR
        }
      }
#if ENV_INCLUDE_GPS == 1
    } else if (strcmp(command, "gps on") == 0) {
      if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
      if (_sensors->setSettingValue("gps", "1")) {
        _prefs->gps_enabled = 1;
        savePrefs();
        strcpy(reply, "ok");
      } else {
        strcpy(reply, "gps toggle not found");
      }
    } else if (strcmp(command, "gps off") == 0) {
      if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
      if (_sensors->setSettingValue("gps", "0")) {
        _prefs->gps_enabled = 0;
        savePrefs();
        strcpy(reply, "ok");
      } else {
        strcpy(reply, "gps toggle not found");
      }
    } else if (strcmp(command, "gps sync") == 0) {
      LocationProvider * l = _sensors->getLocationProvider();
      if (l != NULL) {
        l->syncTime();
        strcpy(reply, "ok");
      } else {
        strcpy(reply, "gps provider not found");
      }
    } else if (strcmp(command, "gps setloc") == 0) {
      if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
      _prefs->node_lat = _sensors->node_lat;
      _prefs->node_lon = _sensors->node_lon;
      savePrefs();
      strcpy(reply, "ok");
    } else if (strcmp(command, "gps advert") == 0) {
      switch (_prefs->advert_loc_policy) {
        case ADVERT_LOC_NONE:
          strcpy(reply, "> none");
          break;
        case ADVERT_LOC_PREFS:
          strcpy(reply, "> prefs");
          break;
        case ADVERT_LOC_SHARE:
          strcpy(reply, "> share");
          break;
        default:
          strcpy(reply, "error");
        }
    } else if (strcmp(command, "gps advert") == 0) {
      strcpy(reply, "Possible: none, share, prefs");
    } else if (memcmp(command, "gps advert ", 11) == 0) {
      if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
      if (strcmp(&command[11], "none") == 0) {
        _prefs->advert_loc_policy = ADVERT_LOC_NONE;
        savePrefs();
        strcpy(reply, "ok");
      } else if (strcmp(&command[11], "share") == 0) {
        _prefs->advert_loc_policy = ADVERT_LOC_SHARE;
        savePrefs();
        strcpy(reply, "ok");
      } else if (strcmp(&command[11], "prefs") == 0) {
        _prefs->advert_loc_policy = ADVERT_LOC_PREFS;
        savePrefs();
        strcpy(reply, "ok");
      } else {
        strcpy(reply, "error");
      }
    } else if (strcmp(command, "gps") == 0) {
      LocationProvider * l = _sensors->getLocationProvider();
      if (l != NULL) {
        bool enabled = l->isEnabled(); // is EN pin on ?
        bool fix = l->isValid();       // has fix ?
        int sats = l->satellitesCount();
        bool active = !strcmp(_sensors->getSettingByKey("gps"), "1");
        if (enabled) {
          sprintf(reply, "on, %s, %s, %d sats",
            active?"active":"deactivated", 
            fix?"fix":"no fix", 
            sats);
        } else {
          strcpy(reply, "off");
        }
      } else {
        strcpy(reply, "Can't find GPS");
      }
#endif
    } else if (strcmp(command, "debug") == 0) {
handleCommandHelpDebug:
      strcpy(reply, "Possible: noise_floor");
    } else if (memcmp(command, "debug ", 6) == 0) {
      if (0 != sender_timestamp) goto handleCommandDenied;
      if (strcmp(&command[6], "noise_floor") == 0) {
          g_debug_noise_floor = !g_debug_noise_floor;
          sprintf(reply, "ok: %s", g_debug_noise_floor ? "on" : "off");
      } else {
          strcpy(reply, "unknown debug");
      }
    } else if (memcmp(command, "powersaving ", 12) == 0) {
      if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
      _prefs->powersaving_enabled = strcmp(&command[12], "on") == 0;
      savePrefs();
      sprintf(reply, "ok: %s", _prefs->powersaving_enabled ? "on" : "off");
    } else if (strcmp(command, "powersaving") == 0) {
      if (_prefs->powersaving_enabled) {
        strcpy(reply, "on");
      } else {
        strcpy(reply, "off");
      }
    } else if (memcmp(command, "log ", 4) == 0) {
      if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
      if (strcmp(&command[4], "start") == 0) {
          _callbacks->setLoggingOn(true);
          strcpy(reply, "logging: on");
      } else if (strcmp(&command[4], "stop") == 0) {
          _callbacks->setLoggingOn(false);
          strcpy(reply, "logging: off");
      } else if (strcmp(&command[4], "erase") == 0) {
          _callbacks->eraseLogFile();
      }
    } else if (strcmp(command, "log") == 0) {
      if (!allowProtectedCommand(sender_timestamp)) goto handleCommandDenied;
      _callbacks->dumpLogFile();
      strcpy(reply, "   EOF");
    } else if (strcmp(command, "stats-packets") == 0) {
      _callbacks->formatPacketStatsReply(reply);
    } else if (strcmp(command, "stats-radio") == 0) {
      _callbacks->formatRadioStatsReply(reply);
    } else if (strcmp(command, "stats-core") == 0) {
      _callbacks->formatStatsReply(reply);
    } else {
      strcpy(reply, "Unknown command");
    }
    return;
handleCommandDenied:
    sprintf(reply, "Denied protected command");
    return;
handleCommandClearedAndSave:
    savePrefs();
    strcpy(reply, "OK: Cleared");
    return;
handleCommandDisabledAndSave:
    strcpy(reply, "OK: Disabled");
    savePrefs();
    return;
handleCommandDisabledAndSaveNeedReboot:
    strcpy(reply, "OK: Disabled - Reboot is required.");
    savePrefs();
    return;
}
