/*
 * services.cpp — echo, discard and chargen; see services.h.
 */

#include "services.h"
#include "cmsis_os2.h"
#include "xClient.h"
#include "log.h"

namespace {

enum class Kind : uint8_t { Echo, Discard, Chargen };

struct Service {
	xNetInterface *net;
	const char    *tag;
	ServiceStats  *stats;
	Kind           kind;
	uint16_t       port;
	const char    *name;
	uint8_t        buf[256];
};

void serve(void *arg) {
	const Service &sv = *static_cast<Service *>(arg);
	sv.net->waitAddress(xNetInterface::kForever);

	xClient client(*sv.net);
	uint8_t *buf = static_cast<Service *>(arg)->buf;

	for (;;) {
		if (!client.listen(sv.port)) {
			osDelay(1000);
			continue;
		}
		if (!client.accept(xNetInterface::kForever)) continue;
		sv.stats->connections = sv.stats->connections + 1;
		log_printf("[%s] %s: connection", sv.tag, sv.name);
		uint32_t moved = 0;
		const uint32_t t0 = osKernelGetTickCount();

		switch (sv.kind) {
		case Kind::Echo:
			for (;;) {
				const int32_t n = client.read(buf, 256, 30000);
				if (n <= 0) break;	// closed, or idle 30 s
				sv.stats->bytesIn = sv.stats->bytesIn + n;
				if (client.write(buf, static_cast<size_t>(n), 5000) != n) break;
				sv.stats->bytesOut = sv.stats->bytesOut + n;
				moved += n;
			}
			break;
		case Kind::Discard:
			for (;;) {
				const int32_t n = client.read(buf, 256, 30000);
				if (n <= 0) break;
				sv.stats->bytesIn = sv.stats->bytesIn + n;
				moved += n;
			}
			break;
		case Kind::Chargen:
			for (uint32_t off = 0;;) {
				for (int i = 0; i < 256; ++i) buf[i] = chargen_byte(off + i);
				const int32_t n = client.write(buf, 256, 5000);
				if (n <= 0) break;	// the peer closed
				off += n;
				moved += n;
				sv.stats->bytesOut = sv.stats->bytesOut + n;
				if (n < 256) break;	// stuck for 5 s
			}
			break;
		}
		const uint32_t ms = osKernelGetTickCount() - t0;
		log_printf("[%s] %s: closed, %lu bytes in %lu ms", sv.tag, sv.name,
		           static_cast<unsigned long>(moved), static_cast<unsigned long>(ms));
		client.stop();
	}
}

} // namespace

uint8_t chargen_byte(uint32_t offset) {
	const uint32_t line = offset / 74, col = offset % 74;
	if (col == 72) return '\r';
	if (col == 73) return '\n';
	return static_cast<uint8_t>(' ' + (line + col) % 95);
}

void services_start(xNetInterface &net, const char *tag, ServiceStats &stats, uint8_t mask) {
	static Service sv[2][3];	// two interfaces at most
	static int used = 0;
	if (used >= 2) return;
	Service *s = sv[used++];
	const Kind kinds[3] = {Kind::Echo, Kind::Discard, Kind::Chargen};
	const uint16_t ports[3] = {7, 9, 19};
	const char *names[3] = {"echo", "discard", "chargen"};
	for (int i = 0; i < 3; ++i) {
		if (!(mask & (1u << i))) continue;
		s[i].net = &net;
		s[i].tag = tag;
		s[i].stats = &stats;
		s[i].kind = kinds[i];
		s[i].port = ports[i];
		s[i].name = names[i];
		osThreadAttr_t a = {};
		a.name = s[i].name;
		a.stack_size = 1024;
		a.priority = osPriorityNormal;
		osThreadNew(serve, &s[i], &a);
	}
}
