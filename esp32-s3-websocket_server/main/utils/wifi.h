#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>

/**
 * @brief Initializes the Wi-Fi station, connects to the configured AP,
 * and waits until the network connection is ready (IP and DNS both up).
 */
void wifi_init_sta(void);

/**
 * @brief Checks if the Wi-Fi is connected and DNS is working.
 *
 * @return true if the network is ready, false otherwise.
 */
bool wifi_is_connected(void);

#endif // WIFI_H