#include <Arduino.h>
//#include "TxtDataHelpers.h"
//#include "AdvertDataHelpers.h"


#ifdef ESP_PLATFORM

#include <WiFi.h>
#include <esp_wifi.h>
#include <WiFiServer.h>

enum TELNET_CLIENT_STATE {
  TCS_NONE = 0,
  TCS_AUTH = 1,
  TCS_NORMAL = 2,
  TCS_IAC_CODE_GET = 3,
  TCS_IAC_CODE_GET_DO = 4,
  TCS_IAC_CODE_GET_WONT = 5,
};

WiFiServer g_server_telnet(23);
WiFiClient *g_client_telnet = nullptr;
static uint32_t gl_client_telnet_last = 0;
static TELNET_CLIENT_STATE gl_client_telnet_state = TCS_NONE;
static int gl_client_telnet_password_attempts = 0;

#endif

#include "Console.h"

extern char g_serial_command[];
extern const int g_serial_command_size;
int g_serial_command_len = 0;

static const char gl_serial_command_history_size = 5;
static char gl_serial_command_history[gl_serial_command_history_size + 1][160];
static int gl_serial_command_history_length = 0;
static int gl_serial_command_history_selector = 0;

enum SERIAL_COMMAND_STATE {
  SCS_NORMAL = 1,
  SCS_ESCAPE_CODE_ONE = 2,
  SCS_ESCAPE_CODE_TWO = 3,
  SCS_IAC_CODE = 4
};
static SERIAL_COMMAND_STATE gl_serial_command_state = SCS_NORMAL;
static char gl_serial_command_code[2];

void console_write_both(Stream &serial, Stream *client, const char *out) {
  if (serial.availableForWrite()) serial.print(out);
#ifdef ESP_PLATFORM
  // check for both no client AND that it's authenticated //
  if (client != nullptr && TCS_NORMAL == gl_client_telnet_state) {
    static char out_tmp[100];
    int out_tmp_len = 0;
    while (*out != '\0') {
      if (*out == '\n') {
        // we need to print marker to current position and handle missing \r //
        if (out_tmp_len > 0) {
          // check if preceding character is not \r //
          if (*(out - 1) != '\r') {
            out_tmp[out_tmp_len] = '\r';
            out_tmp_len++;
          }
        } else {
          // implicit \r //
          out_tmp[out_tmp_len] = '\r';
          out_tmp_len++;
        }

        // add \n and \0 and print //
        out_tmp[out_tmp_len] = '\n';
        out_tmp_len++;
console_write_both_write_tmp_out:
        out_tmp[out_tmp_len] = '\0';
        client->print(out_tmp);
        out_tmp_len = 0;
      } else if (out_tmp_len == sizeof(out_tmp) - 2) {
        goto console_write_both_write_tmp_out;
      } else {
        out_tmp[out_tmp_len] = *out;
        out_tmp_len++;
      }
      out++;
    }

    // handle leftvoer //
    if (out_tmp_len > 0) {
      out_tmp[out_tmp_len] = '\0';
      client->print(out_tmp);
    }
  }
#endif
}

void console_write_both_formated(Stream &serial, Stream *client, const char *out, ...) {
    va_list args;
    va_start(args, out);

    static char buffer[256];
    vsnprintf(buffer, sizeof(buffer), out, args);

    if (*buffer)
      console_write_both(serial, client, buffer);

    va_end(args);
}

#ifdef ESP_PLATFORM

static void telnet_send_no_local_echo(WiFiClient *client) {
  static uint8_t cmd[] = {0xFF, 0xFB, 0x01}; // IAC WILL ECHO
  client->write(cmd, sizeof(cmd));
}

static void telnet_send_local_echo(WiFiClient *client) {
  static uint8_t cmd[] = {0xFF, 0xFD, 0x01}; // IAC DO ECHO
  client->write(cmd, sizeof(cmd));
}

static bool telnet_handle_iac(WiFiClient *client, const char c) {
  if (TCS_IAC_CODE_GET == gl_client_telnet_state) {
    if (0xFB == c) {
      gl_client_telnet_state = TCS_IAC_CODE_GET_WONT;
    } else if (0xFD == c) {
      gl_client_telnet_state = TCS_IAC_CODE_GET_DO;
    } else {
      gl_client_telnet_state = TCS_NORMAL;
      return false;
    }
  } else if (TCS_IAC_CODE_GET_WONT == gl_client_telnet_state) {
    gl_client_telnet_state = TCS_NORMAL;
    return false;
  } else if (TCS_IAC_CODE_GET_DO == gl_client_telnet_state) {
    if (0x03 == c) {
      // send no echo //
      telnet_send_no_local_echo(client);
    }
    gl_client_telnet_state = TCS_NORMAL;
    return false;
  }
  return true;
}

#endif

static bool loop_serial_console_internal(mesh::Mesh &the_mesh, uint32_t client_now, Stream &serial, Stream *client) {
  // serial processing //
  char c;
  if (!serial.available()) {
#ifdef ESP_PLATFORM
    if (nullptr == client || TCS_NORMAL != gl_client_telnet_state || !client->available()) return true;
    c = client->read();
    if (-1 == c) {
      // disconnected //
      return false;
    }
    if (SCS_NORMAL == gl_serial_command_state) {
      if (0 == c) {
        // skip null byte? //
        return true;
      } else if (255 == c) {
        // telnet IAC code
        gl_serial_command_state = SCS_IAC_CODE;
      }
    }
#else
    return true;
#endif
  } else {
    c = serial.read();
  }

  // if CTRL+C is pressed then reset the prompt //
  bool disconnect_client = false;
  if (0x03 == c) {
    console_write_both(serial, client, "\n# ");

loop_serial_console_reset:

    // reset for another command //
    g_serial_command_len = 0;
    *g_serial_command = 0;
    gl_serial_command_state = SCS_NORMAL;

    // selector should now be latest //
    gl_serial_command_history_selector = gl_serial_command_history_length;

    // send next prompt (don't send to the telnet client if it's being disconnected) //
    console_write_both(serial, !disconnect_client ? client : nullptr, "# ");

    return !disconnect_client;
  }

  // command state //
  if (SCS_ESCAPE_CODE_ONE == gl_serial_command_state) {
      gl_serial_command_code[0] = c;
      gl_serial_command_state = SCS_ESCAPE_CODE_TWO;
  } else if (SCS_ESCAPE_CODE_TWO == gl_serial_command_state) {
    gl_serial_command_code[1] = c;
    if (0x5B == gl_serial_command_code[0] && 0x41 == gl_serial_command_code[1]) {
      // up arrow //
      if (0 == gl_serial_command_history_selector) {
        // don't do anything - selector is at the top //
      } else {
        if (gl_serial_command_history_selector == gl_serial_command_history_length) {
          // copy current command into history if selector is moving away from active command //
          strcpy(gl_serial_command_history[gl_serial_command_history_length], g_serial_command);
        }

        // set previous selector //
        gl_serial_command_history_selector--;

loop_serial_console_display_selected:

        // remove current command //
        const int backspace_size = g_serial_command_size / 3;
        for (int i=g_serial_command_len; i>0; i-=backspace_size) {
          const int backspace_len = i > backspace_size ? backspace_size : i;
          for (int c=0; c < backspace_len; c++) {
            g_serial_command_backspaces[c * 3] = '\b';
            g_serial_command_backspaces[(c * 3) + 1] = ' ';
            g_serial_command_backspaces[(c * 3) + 2] = '\b';
          }
          g_serial_command_backspaces[backspace_len * 3] = '\0';
          console_write_both(serial, client, g_serial_command_backspaces);
        }

        // set current to selected //
        g_serial_command_len = strlen(gl_serial_command_history[gl_serial_command_history_selector]);
        strncpy(g_serial_command, gl_serial_command_history[gl_serial_command_history_selector], g_serial_command_len);
        g_serial_command[g_serial_command_len] = '\0';

        // and display //
        console_write_both(serial, client, g_serial_command);
      }
    } else if (0x5B == gl_serial_command_code[0] && 0x42 == gl_serial_command_code[1]) {
      // down arrow //
      if (gl_serial_command_history_selector < gl_serial_command_history_length) {
        // set next selector and display //
        gl_serial_command_history_selector++;

        goto loop_serial_console_display_selected;
      }
    } else {
      MESH_DEBUG_PRINTLN("unhandled terminal command sequence: %2.2x %2.2x",
        gl_serial_command_code[0], gl_serial_command_code[1]);
    }

    // go back to normal state //
    gl_serial_command_state = SCS_NORMAL;
    return true;
#ifdef ESP_PLATFORM
  } else if (SCS_IAC_CODE == gl_serial_command_state) {
    if (nullptr != client) {
      if (!telnet_handle_iac(g_client_telnet, c)) {
        gl_serial_command_state = SCS_NORMAL;
      }
    }
    return true;
#endif
  }

  if ('\b' == c || 0x7F == c) {
    // backspace //
    if (g_serial_command_len > 0) {
        g_serial_command_len--;
        g_serial_command[g_serial_command_len] = '\0';
        console_write_both(serial, client, "\b \b");
    }
  } else if ('\r' == c || '\n' == c) {
    // carriage return //
    g_serial_command[g_serial_command_len] = 0;
    console_write_both(serial, client, "\n");
    if (g_serial_command_len > 0) {
      // handle the command //
      char reply[160];
      *reply = '\0';
#ifdef ESP_PLATFORM
      if (nullptr != client) {
        if (strcmp(g_serial_command, "exit") == 0) {
          // disconnect the client //
          disconnect_client = true;
          goto loop_serial_console_reset;
        } else if (client_now > 0) {
            // update the client's last only when a command was issued //
            gl_client_telnet_last = client_now;
        }
      }
#endif
      the_mesh.handleCommand(0, g_serial_command, reply);  // NOTE: there is no sender_timestamp via serial!
      if (*reply != '\0') {
        console_write_both(serial, client, "  -> ");
        console_write_both(serial, client, reply);
        console_write_both(serial, client, "\n");
      }

      // do console reset and not add the exit command //
      if (disconnect_client) goto loop_serial_console_reset;

      // update command history //
      if (gl_serial_command_history_length > 0 && (strcmp(gl_serial_command_history[gl_serial_command_history_length - 1], g_serial_command) == 0)) {
        // don't do anything if the command was the same as the last command! //
      } else {
        if (gl_serial_command_history_length == gl_serial_command_history_size) {
          // shift! //
          for (int i=0; i<gl_serial_command_history_size - 1; i++) {
            strcpy(gl_serial_command_history[i], gl_serial_command_history[i + 1]);
          }
        } else {
          gl_serial_command_history_length++;
        }
        strcpy(gl_serial_command_history[gl_serial_command_history_length - 1], g_serial_command);
      }

      // history selector goes to the last command always //
      gl_serial_command_history_selector = gl_serial_command_history_length;
      // reset and go for another! //
      goto loop_serial_console_reset;
    }
    console_write_both(serial, client, "# ");
  } else if (0x1B == c) {
      // start a command //
      gl_serial_command_state = SCS_ESCAPE_CODE_ONE;
#ifdef ESP_PLATFORM
  } else if (0xFF == c) {
    gl_serial_command_state = SCS_IAC_CODE;
    gl_client_telnet_state = TCS_IAC_CODE_GET;
#endif
  } else {
    // add chars as typing //
    if (g_serial_command_len < g_serial_command_size - 1) {
      g_serial_command[g_serial_command_len++] = c;
      g_serial_command[g_serial_command_len] = '\0';
      console_write_both(serial, client, &g_serial_command[g_serial_command_len - 1]);
    }
  }

  return true;
}

#ifdef ESP_PLATFORM

bool loop_serial_console(mesh::Mesh &the_mesh) {
  return loop_serial_console_internal(the_mesh, 0, Serial, g_client_telnet);
}

#else

bool loop_serial_console(mesh::Mesh &the_mesh) {
  return loop_serial_console_internal(the_mesh, 0, Serial, nullptr);
}

#endif

void loop_telnet_server(mesh::Mesh &the_mesh) {
#ifdef ESP_PLATFORM
  if (g_server_telnet.hasClient()) {
    auto client = g_server_telnet.available();
    if (!client) return;

    if (nullptr != g_client_telnet) {
loop_telnet_server_reject:
      MESH_DEBUG_PRINTLN("Rejecting telnet session from: %s",
        g_client_telnet->remoteIP().toString().c_str());
      client.println("Too many connections!");
      client.flush();
      client.stop();
      return;
    }

    const char *password = the_mesh.getNodePrefPassword();
    if (nullptr == password || '\0' == *password) {
      goto loop_telnet_server_reject;
    }

    g_client_telnet = new WiFiClient(client);
    gl_client_telnet_last = the_mesh.getRTCClock()->getCurrentTime();
    MESH_DEBUG_PRINTLN("New telnet session from: %s",
      g_client_telnet->remoteIP().toString().c_str());

    telnet_send_no_local_echo(g_client_telnet);

    g_client_telnet->print("Node: ");
    g_client_telnet->println(the_mesh.getNodeName());
    g_client_telnet->print("Password: ");
    gl_client_telnet_state = TCS_AUTH;
    gl_client_telnet_password_attempts = 0;
  }

  if (nullptr == g_client_telnet) return;
  const uint32_t now = the_mesh.getRTCClock()->getCurrentTime();
  if (now - gl_client_telnet_last >= the_mesh.getPrefWifiTelnetTimeout()) {
    MESH_DEBUG_PRINTLN("Telnet client timed out!");
    goto loop_telnet_server_disconnect;
  } else if (!g_client_telnet->connected()) {
    MESH_DEBUG_PRINTLN("Telnet client disconnected!");

loop_telnet_server_disconnect:
    // disconnect //
    g_client_telnet->flush();
    g_client_telnet->stop();
    delete g_client_telnet;
    g_client_telnet = nullptr;
    gl_client_telnet_state = TCS_NONE;
    return;
  }
  if (!g_client_telnet->available()) return;

  if (TCS_AUTH == gl_client_telnet_state) {
    if (now - gl_client_telnet_last >= 30) {
      MESH_DEBUG_PRINTLN("Telnet client timed out before authorization!");
      goto loop_telnet_server_disconnect;
    }
    static char password_input[16];
    const char c = g_client_telnet->read();
    if (0 == c) {
      return;
    } else if (-1 == c) {
      MESH_DEBUG_PRINTLN("Telnet session ended!");
      goto loop_telnet_server_disconnect;
    } else if (0xFF == c) {
      gl_client_telnet_state = TCS_IAC_CODE_GET;
    } else if ('\b' == c || 0x7F == c) {
      // backspace //
      int password_input_len = strlen(password_input);
      if (password_input_len > 0) {
        password_input[password_input_len - 1] = '\0';
      }
    } else if ('\r' == c || '\n' == c) {
      // carriage return //
      const char *password = the_mesh.getNodePrefPassword();
      const bool password_empty = !*password_input;
      const bool password_valid = strcmp(password, password_input) == 0;
      // burn password //
      memset(password_input, 0, sizeof(password_input));
      if (password_empty) {
        // ignore empty //
      } else if (password_valid) {
        gl_client_telnet_state = TCS_NORMAL;

        g_client_telnet->print("\r\nLogged in!\r\n\r\n# ");

        // send active prompt //
        if (!*g_serial_command)
          g_client_telnet->print(g_serial_command);
      } else {
        g_client_telnet->print("\r\nERROR: Invalid password!\r\n");
        gl_client_telnet_password_attempts++;
        if (gl_client_telnet_password_attempts >= 3) {
          MESH_DEBUG_PRINTLN("Disconnected telnet session after too many wrong passwords!");
          goto loop_telnet_server_disconnect;
        }
        g_client_telnet->print("\r\nPassword:");
      }
    } else {
      int password_input_len = strlen(password_input);
      if (strlen(password_input) < sizeof(password_input) - 1) {
        password_input[password_input_len] = c;
        password_input_len++;
        password_input[password_input_len] = '\0';
      }
    }
  } else if (TCS_NORMAL == gl_client_telnet_state) {
    if (!loop_serial_console_internal(the_mesh, now, Serial, g_client_telnet)) {
      MESH_DEBUG_PRINTLN("Telnet session ending.");
      goto loop_telnet_server_disconnect;
    }
  } else if (TCS_IAC_CODE_GET == gl_client_telnet_state || TCS_IAC_CODE_GET_DO == gl_client_telnet_state
    || TCS_IAC_CODE_GET_WONT == gl_client_telnet_state) {
    const char c = g_client_telnet->read();
    if (!telnet_handle_iac(g_client_telnet, c)) {
      gl_client_telnet_state = TCS_AUTH;
    }
  }
#endif
}
