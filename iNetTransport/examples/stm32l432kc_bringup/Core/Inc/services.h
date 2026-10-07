/*
 * services.h — the classic test services, on any xNetInterface, for the
 * PC-side bring-up script (iNetTransport/tools/net_bringup.py):
 *
 *   echo     port 7   everything received is sent back      (RFC 862)
 *   discard  port 9   everything received is counted, dropped (RFC 863)
 *   chargen  port 19  sends a known pattern until the peer closes (RFC 864)
 *
 * Each is one thread serving one connection at a time.
 */
#ifndef BRINGUP_SERVICES_H
#define BRINGUP_SERVICES_H

#include <stdint.h>
#include "xNetInterface.h"

struct ServiceStats {
	volatile uint32_t connections;
	volatile uint32_t bytesIn;
	volatile uint32_t bytesOut;
};

enum : uint8_t { SERVICE_ECHO = 1, SERVICE_DISCARD = 2, SERVICE_CHARGEN = 4, SERVICE_ALL = 7 };

// Starts a thread on net for each service in mask. tag names them in
// the log. An ESP-AT module listens on one port only: give it
// SERVICE_ECHO alone.
void services_start(xNetInterface &net, const char *tag, ServiceStats &stats, uint8_t mask);

// The chargen stream: line k (0-based) is 72 printable characters
// starting at ' ' + (k % 95), wrapping within ' '..'~', then "\r\n".
// The script regenerates it to check every byte.
uint8_t chargen_byte(uint32_t offset);

#endif /* BRINGUP_SERVICES_H */
