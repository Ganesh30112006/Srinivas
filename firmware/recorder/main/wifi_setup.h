/*
 * Wi-Fi setup mode: the recorder becomes its own Wi-Fi access point and serves a small page
 * (http://192.168.4.1) where a new network can be chosen and saved. Entered when no network is
 * saved, when the main button is held while the board starts, or after holding the main button
 * for 5 s while not recording.
 */
#pragma once

#include <stdbool.h>

/* True if setup mode was requested for this boot (button held at start, or requested before a restart). */
bool wifi_setup_requested(void);

/* Restarts the board into setup mode. */
void wifi_setup_request_restart(void);

/* Runs setup mode. Never returns: restarts the board after saving, or after 10 minutes. */
void wifi_setup_run(void);

/* For the display while in setup mode. */
bool wifi_setup_active(void);
const char *wifi_setup_ap_name(void);
const char *wifi_setup_ap_pass(void);
