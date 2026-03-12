#pragma once

#include <Arduino.h>

#ifdef ESP_PLATFORM

#include <WiFiClient.h>

extern WiFiClient *g_client_telnet;

#define CONSOLE_WRITE(str) console_write_both(Serial, g_client_telnet, str);
#define CONSOLE_WRITE_F(F, ...) console_write_both_formated(Serial, g_client_telnet, F, ##__VA_ARGS__);

#else


#define CONSOLE_WRITE(str) console_write_both(Serial, nullptr, str);
#define CONSOLE_WRITE_F(F, ...) console_write_both_formated(Serial, nullptr, F, ##__VA_ARGS__);

#endif

extern void console_write_both(USBCDC &serial, Stream *client, const char *out);
extern void console_write_both_formated(USBCDC &serial, Stream *client, const char *out, ...);
