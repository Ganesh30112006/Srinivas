/*
 * net_task: Wi-Fi (several saved networks, auto-reconnect), the server link (AWS IoT Core over
 * MQTT, or the WebSocket gateway), `hello` handshake, live-first sending of un-ACKed chunks,
 * selective-ACK processing, exponential backoff with jitter (contracts §1).
 */
#pragma once

void net_start(void);

/* The Wi-Fi network currently being tried or used (for the display). */
const char *net_wifi_ssid(void);

/* Why the Wi-Fi isn't connected, in a few words (for the display). */
const char *net_wifi_problem(void);
