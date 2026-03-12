#pragma once

#include "Mesh.h"
#include "ConsoleEarly.h"

#ifdef ESP_PLATFORM

#include <WiFiClient.h>

extern WiFiServer g_server_telnet;
extern WiFiClient *g_client_telnet;

extern void loop_telnet_server(mesh::Mesh &the_mesh);

#endif

extern bool loop_serial_console(mesh::Mesh &the_mesh);
