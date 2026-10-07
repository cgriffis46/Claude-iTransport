/*
 * config.h — what the bring-up firmware brings up. Every setting can
 * also be given to CMake, e.g. -DBRINGUP_WIFI=1 -DWIFI_SSID=\"home\".
 */
#ifndef BRINGUP_CONFIG_H
#define BRINGUP_CONFIG_H

/* W5500 Ethernet on SPI1. */
#ifndef BRINGUP_ETH
#define BRINGUP_ETH 1
#endif

/* ESP-AT Wi-Fi on USART1. */
#ifndef BRINGUP_WIFI
#define BRINGUP_WIFI 0
#endif
#ifndef WIFI_SSID
#define WIFI_SSID "your-ssid"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS "your-passphrase"
#endif
#ifndef ESP_BAUD
#define ESP_BAUD 115200
#endif

/* Ethernet address: DHCP, or this static one. */
#ifndef ETH_DHCP
#define ETH_DHCP 1
#endif
#ifndef ETH_STATIC_IP
#define ETH_STATIC_IP 192, 168, 1, 50
#endif
#ifndef ETH_STATIC_GW
#define ETH_STATIC_GW 192, 168, 1, 1
#endif
#ifndef ETH_STATIC_MASK
#define ETH_STATIC_MASK 255, 255, 255, 0
#endif

/* Fastest SPI1 prescaler to try (80 MHz / 4 = 20 MHz). The probe steps
 * down from here until the W5500 reads back cleanly. */
#ifndef SPI_FASTEST_PRESCALER
#define SPI_FASTEST_PRESCALER SPI_BAUDRATEPRESCALER_4
#endif

/* How often the statistics line is logged, ms. */
#ifndef STATS_PERIOD_MS
#define STATS_PERIOD_MS 10000
#endif

#endif /* BRINGUP_CONFIG_H */
