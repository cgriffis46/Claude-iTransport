/*
 * ref_app.c: Microchip's own host driver (19.5.2, from WINC_REF_DIR)
 * driven through its public API, for WincRef_test.cpp, which runs it
 * against test/sim/SimWinc1500.h. What it hears back goes to the ref_on_*
 * functions there.
 */
#include <string.h>
#include "driver/include/m2m_wifi.h"
#include "socket/include/socket.h"

void ref_on_wifi_state(int state, int err);
void ref_on_dhcp(const uint8 ip[4]);
void ref_on_rssi(int rssi);
void ref_on_time(int year, int month, int day, int h, int m, int s);
void ref_on_connect(int sock, int err);
void ref_on_send(int sock, int sent);
void ref_on_recv(int sock, const uint8 *data, int len);
void ref_on_bind(int sock, int status);
void ref_on_listen(int sock, int status);
void ref_on_accept(int listenSock, int sock);
void ref_on_resolve(const char *name, const uint8 ip[4]);

static void wifi_cb(uint8 type, void *msg) {
	if (type == M2M_WIFI_RESP_CON_STATE_CHANGED) {
		tstrM2mWifiStateChanged *s = (tstrM2mWifiStateChanged *)msg;
		ref_on_wifi_state(s->u8CurrState, s->u8ErrCode);
	} else if (type == M2M_WIFI_REQ_DHCP_CONF) {
		ref_on_dhcp((const uint8 *)msg);   /* u32StaticIP first, network order */
	} else if (type == M2M_WIFI_RESP_CURRENT_RSSI) {
		ref_on_rssi(*(sint8 *)msg);
	} else if (type == M2M_WIFI_RESP_GET_SYS_TIME) {
		tstrSystemTime *t = (tstrSystemTime *)msg;
		ref_on_time(t->u16Year, t->u8Month, t->u8Day, t->u8Hour, t->u8Minute, t->u8Second);
	}
}

static uint8 rxbuf[7][600];

static void sock_cb(SOCKET sock, uint8 msg, void *m) {
	if (msg == SOCKET_MSG_CONNECT) {
		tstrSocketConnectMsg *c = (tstrSocketConnectMsg *)m;
		ref_on_connect(c->sock, c->s8Error);
	} else if (msg == SOCKET_MSG_SEND) {
		ref_on_send(sock, *(sint16 *)m);
	} else if (msg == SOCKET_MSG_RECV) {
		tstrSocketRecvMsg *r = (tstrSocketRecvMsg *)m;
		ref_on_recv(sock, r->pu8Buffer, r->s16BufferSize);
		/* Asks again once the whole reply has been handed over in pieces. */
		if (r->s16BufferSize > 0 && r->u16RemainingSize == 0) recv(sock, rxbuf[sock], sizeof rxbuf[sock], 0);
	} else if (msg == SOCKET_MSG_BIND) {
		ref_on_bind(sock, ((tstrSocketBindMsg *)m)->status);
	} else if (msg == SOCKET_MSG_LISTEN) {
		ref_on_listen(sock, ((tstrSocketListenMsg *)m)->status);
	} else if (msg == SOCKET_MSG_ACCEPT) {
		ref_on_accept(sock, ((tstrSocketAcceptMsg *)m)->sock);
	}
}

static void resolve_cb(uint8 *name, uint32 ip) {
	uint8 b[4];
	memcpy(b, &ip, 4);
	ref_on_resolve((const char *)name, b);
}

int ref_init(void) {
	tstrWifiInitParam p;
	memset(&p, 0, sizeof p);
	p.pfAppWifiCb = wifi_cb;
	const sint8 r = m2m_wifi_init(&p);
	socketInit();
	registerSocketCallback(sock_cb, resolve_cb);
	return r;
}
int ref_firmware(uint8 *maj, uint8 *min, uint8 *patch) {
	tstrM2mRev rev;
	const sint8 r = m2m_wifi_get_firmware_version(&rev);
	*maj = rev.u8FirmwareMajor; *min = rev.u8FirmwareMinor; *patch = rev.u8FirmwarePatch;
	return r;
}
void ref_events(void) { m2m_wifi_handle_events(NULL); }
int ref_join(const char *ssid, const char *pass) {
	return m2m_wifi_connect((char *)ssid, (uint8)strlen(ssid), M2M_WIFI_SEC_WPA_PSK, (void *)pass, M2M_WIFI_CH_ALL);
}
int ref_rssi(void) { return m2m_wifi_req_curr_rssi(); }
int ref_time(void) { return m2m_wifi_get_sytem_time(); }
int ref_tcp_socket(void) { return socket(AF_INET, SOCK_STREAM, 0); }
int ref_connect(int s, const uint8 ip[4], uint16 port) {
	struct sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = _htons(port);
	memcpy(&a.sin_addr.s_addr, ip, 4);
	return connect((SOCKET)s, (struct sockaddr *)&a, sizeof a);
}
int ref_recv(int s) { return recv((SOCKET)s, rxbuf[s], sizeof rxbuf[s], 0); }
int ref_send(int s, const uint8 *d, uint16 n) { return send((SOCKET)s, (void *)d, n, 0); }
int ref_close(int s) { return close((SOCKET)s); }
int ref_bind(int s, uint16 port) {
	struct sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = _htons(port);
	return bind((SOCKET)s, (struct sockaddr *)&a, sizeof a);
}
int ref_listen(int s) { return listen((SOCKET)s, 1); }
int ref_resolve(const char *name) { return gethostbyname((uint8 *)name); }
