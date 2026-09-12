/*
 * wifi.h - minimal STA bring-up for the real transport build.
 */
#ifndef WIFI_H
#define WIFI_H

/* Joins CONFIG_SAF_WIFI_SSID and blocks until an IP is obtained. */
void wifi_connect_blocking(void);

#endif /* WIFI_H */
