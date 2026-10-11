/*
 * WincProtocol.h
 *
 *  The ATWINC1500's host interface, as numbers: the SPI commands, the
 *  registers the host uses, the HIF (host interface) message header, and
 *  the layouts of the Wi-Fi and socket messages.
 *
 *  Source: Microchip's host driver 19.5.2 (BSD-3, Atmel 2016-2017) as
 *  shipped in Arduino's WiFi101 library: nmspi.c, nmasic.c/h, nmdrv.c,
 *  m2m_hif.c/h, m2m_wifi.c, m2m_types.h, socket.c,
 *  m2m_socket_host_if.h. There is no public document of this protocol;
 *  that driver is the reference. Opcodes, sizes and offsets were printed
 *  from its headers for the 32-bit ARM ABI by test/ref/winc_numbers.c
 *  (not counted by hand), and test/WincRef_test.cpp static_asserts them
 *  against those headers again. Nothing here was checked on a module.
 *
 *  Byte order: the messages are little-endian (the firmware and the
 *  driver are both little-endian ARM), except the socket address, whose
 *  port and IPv4 address are in network order (big-endian), as in BSD's
 *  sockaddr_in. A register value written with CMD_SINGLE_WRITE goes out
 *  most significant byte first inside the command; a register read
 *  returns it least significant byte first.
 */

#ifndef WINC_PROTOCOL_H_
#define WINC_PROTOCOL_H_

#include <stddef.h>
#include <stdint.h>

namespace WINC {

// ---- SPI commands (nmspi.c) ----
const uint8_t cmd_dma_ext_write	= 0xC7;	// address (3), size (3): a block of memory
const uint8_t cmd_dma_ext_read	= 0xC8;
const uint8_t cmd_single_write	= 0xC9;	// address (3), value (4, MSB first)
const uint8_t cmd_single_read	= 0xCA;	// address (3)
const uint8_t cmd_reset			= 0xCF;	// FF FF FF: resets the SPI state machine

// A data packet (a block write's data) starts with 0xF0 | order; one
// packet holds the whole block (order 3) up to the packet size set in
// spi_protocol_config, 8 KB here. A read's data comes after a byte whose
// high nibble is 0xF.
const uint8_t data_token_single	= 0xF3;
const uint8_t data_write_ack	= 0xC3;	// after a block write: C3 00

// Each response byte is polled for up to this many single-byte reads.
const uint8_t resp_polls		= 10;
// A failed access is retried after a reset command, this many times.
const uint8_t spi_tries			= 10;

// spi_protocol_config (0xE824): bits 2..3 enable the CRC (on after
// reset; the driver turns it off), bits 4..6 the data packet size
// (5: 8 KB).
const uint32_t reg_spi_protocol_config	= 0xE824;
const uint32_t spi_cfg_crc_bits			= 0x0C;
const uint32_t spi_cfg_pkt_mask			= 0x70;
const uint32_t spi_cfg_pkt_8k			= 5u << 4;

// The CRC-7 on commands (polynomial x^7 + x^3 + 1, from 0x7F, sent
// shifted left by one). Equal to nmspi.c's syndrome table.
inline uint8_t crc7(uint8_t crc, const uint8_t* p, size_t n) {
	while (n--) {
		const uint8_t d = *p++;
		for (int bit = 7; bit >= 0; --bit) {
			const uint8_t in = static_cast<uint8_t>(((d >> bit) & 1u) ^ ((crc >> 6) & 1u));
			crc = static_cast<uint8_t>((crc << 1) & 0x7F);
			if (in) crc ^= 0x09;
		}
	}
	return crc;
}

// ---- chip registers (nmasic.c/h, m2m_hif.c) ----
const uint32_t reg_chip_id			= 0x1000;	// raw: 0x1002B0 (rev B0), 0x1003A0 (rev 3A0)
const uint32_t reg_glb_reset		= 0x1400;	// write 0: global reset (no reset pin)
const uint32_t reg_pin_mux_0		= 0x1408;	// bit 8: the IRQN pin
const uint32_t reg_intr_enable		= 0x1A00;	// bit 16: interrupts to the host
const uint32_t reg_efuse_done		= 0x1014;	// bit 31: efuses loaded
const uint32_t reg_nmi_state		= 0x108C;	// boot handshake, then a HIF request's header
const uint32_t reg_gp_1				= 0x14A0;	// start-up configuration bits
const uint32_t reg_gp_2				= 0xC0008;	// address of tstrGpRegs (19.4 and later; 0 before)
const uint32_t reg_bootrom			= 0xC000C;
const uint32_t reg_wait_for_host	= 0x207BC;	// bit 0: the ROM won't wait for the host
const uint32_t data_mem_base		= 0x30000;	// tstrGpRegs, tstrM2mRev, the MAC

const uint32_t finish_boot_rom		= 0x10ADD09E;	// reg_bootrom: the ROM is waiting
const uint32_t start_firmware		= 0xEF522F61;	// reg_bootrom: go
const uint32_t finish_init_state	= 0x02532636;	// reg_nmi_state: the firmware is up
const uint32_t gp1_use_pmu			= 1u << 1;		// rHAVE_USE_PMU_BIT, rev 3A0 and later
const uint32_t gp1_reserved1		= 1u << 8;		// rHAVE_RESERVED1_BIT, always set
const uint16_t rev_3a0				= 0x3A0;
const uint16_t rev_b0				= 0x2B0;

// ---- the HIF (m2m_hif.c) ----
const uint32_t reg_rcv_ctrl_0	= 0x1070;	// chip -> host: bit 0 a message, bits 2..13 its size; bit 1 "RX done"
const uint32_t reg_rcv_ctrl_1	= 0x1084;	// chip -> host: the message's address
const uint32_t reg_rcv_ctrl_2	= 0x1078;	// host -> chip: write 2 to ask for a buffer; bit 1 clears when there is one
const uint32_t reg_rcv_ctrl_3	= 0x106C;	// host -> chip: (address << 2) | 2: the message is there
const uint32_t reg_rcv_ctrl_4	= 0x150400;	// host -> chip: the buffer's address

// A message: gid (1), opcode (1), length (2, LE: the whole message,
// header included), 4 bytes unused; then the control structure; and for
// a data request (opcode | req_data_pkt) the data at a fixed offset after
// the header.
const uint16_t hif_header_bytes	= 8;	// M2M_HIF_HDR_OFFSET
const uint16_t hif_max_bytes	= 1596;	// M2M_HIF_MAX_PACKET_SIZE
const uint8_t  req_data_pkt		= 0x80;

const uint8_t group_wifi	= 1;
const uint8_t group_ip		= 2;
const uint8_t group_hif		= 3;

// The version the host reports at boot (M2M_MAKE_VERSION_INFO(19,5,2,
// 19,5,2)): the firmware may shape messages by it, so we claim the
// version whose messages we were written against.
const uint32_t host_version_info	= 0x13521352;
inline uint16_t makeVersion(uint8_t major, uint8_t minor, uint8_t patch) {
	return static_cast<uint16_t>((major << 8) | ((minor & 0x0F) << 4) | (patch & 0x0F));
}
const uint16_t host_driver_version	= 0x1352;	// 19.5.2
const uint16_t min_firmware_version	= 0x1350;	// 19.5.0: tstrM2MIPConfig with the lease time

// tstrGpRegs (8): u32Mac_efuse_mib @0, u32Firmware_Ota_rev @4.
// tstrM2mRev (40): u32Chipid @0, firmware major/minor/patch @4/5/6,
// the least driver version it needs @7/8/9, build date @10 (12), time
// @22 (9).
const uint8_t rev_bytes = 40;

// ---- Wi-Fi messages (group_wifi; m2m_types.h) ----
const uint8_t wifi_req_current_rssi		= 3;	// no payload
const uint8_t wifi_resp_current_rssi	= 4;	// sint8 rssi, 3 bytes padding
const uint8_t wifi_req_enable_sntp		= 12;	// no payload
const uint8_t wifi_req_get_sys_time		= 26;	// no payload
const uint8_t wifi_resp_get_sys_time	= 27;	// tstrSystemTime
const uint8_t wifi_req_connect			= 40;	// tstrM2mWifiConnect
const uint8_t wifi_req_disconnect		= 43;	// no payload
const uint8_t wifi_resp_con_state		= 44;	// tstrM2mWifiStateChanged
const uint8_t wifi_req_dhcp_conf		= 50;	// (the firmware's message) tstrM2MIPConfig
const uint8_t wifi_resp_ip_conflict		= 52;	// uint32 address

// tstrM2mWifiConnect (108): the key (tstrM2MWifiSecInfo: the PSK, 65
// bytes, NUL-terminated, @0; u8SecType @65), u16Ch @68, au8SSID @70 (33,
// NUL-terminated), u8NoSaveCred @103.
const uint8_t  connect_bytes		= 108;
const uint8_t  connect_psk			= 0;
const uint8_t  connect_sec_type		= 65;
const uint8_t  connect_channel		= 68;
const uint8_t  connect_ssid			= 70;
const uint8_t  connect_no_save		= 103;
const uint8_t  max_ssid_len			= 32;	// M2M_MAX_SSID_LEN 33, with the NUL
const uint8_t  max_psk_len			= 64;	// M2M_MAX_PSK_LEN 65: a passphrase of 8..63, or 64 hex digits
const uint8_t  sec_open				= 1;
const uint8_t  sec_wpa_psk			= 2;
const uint16_t channel_all			= 255;

// tstrM2mWifiStateChanged (4): u8CurrState @0 (0 disconnected, 1
// connected), u8ErrCode @1.
const uint8_t wifi_disconnected		= 0;
const uint8_t wifi_connected		= 1;
const uint8_t err_scan_fail			= 1;	// no such network
const uint8_t err_join_fail			= 2;
const uint8_t err_auth_fail			= 3;	// wrong passphrase
const uint8_t err_assoc_fail		= 4;
const uint8_t err_conn_inprogress	= 5;

// tstrM2MIPConfig (20 from 19.5; 16 before): address @0, gateway @4,
// DNS @8, subnet @12, lease time @16 (LE, seconds). The addresses are
// in network order.
const uint8_t ip_config_bytes		= 20;

// tstrSystemTime (8): u16Year @0 (0: not set yet), month @2 (1-12), day
// @3, hour @4, minute @5, second @6. UTC.
const uint8_t sys_time_bytes		= 8;

// ---- IP and socket messages (group_ip; m2m_types.h, m2m_socket_host_if.h) ----
const uint8_t ip_req_static_ip_conf	= 10;	// tstrM2MIPConfig
const uint8_t ip_req_enable_dhcp	= 11;	// no payload
const uint8_t ip_req_disable_dhcp	= 12;	// no payload

const uint8_t sock_cmd_bind			= 0x41;
const uint8_t sock_cmd_listen		= 0x42;
const uint8_t sock_cmd_accept		= 0x43;	// (the firmware's message)
const uint8_t sock_cmd_connect		= 0x44;
const uint8_t sock_cmd_send			= 0x45;	// | req_data_pkt
const uint8_t sock_cmd_recv			= 0x46;
const uint8_t sock_cmd_close		= 0x49;	// no reply
const uint8_t sock_cmd_dns_resolve	= 0x4A;

// TCP sockets are 0..6, UDP 7..10. The host numbers the sockets it opens
// itself; the firmware numbers the connections a listening socket
// accepts.
const uint8_t tcp_sockets			= 7;	// TCP_SOCK_MAX
const uint16_t socket_max_send		= 1400;	// SOCKET_BUFFER_MAX_LENGTH
const uint8_t hostname_max			= 64;	// HOSTNAME_MAX_SIZE, with the NUL
const uint16_t af_inet				= 2;

// tstrSockAddr (8): u16Family @0 (LE), u16Port @2 and u32IPAddr @4 in
// network order.
const uint8_t sockaddr_bytes		= 8;

// Requests. tstrBindCmd and tstrConnectCmd (12): address @0, sock @8,
// u8Void/u8SslFlags @9, u16SessionID @10. tstrListenCmd (4): sock @0,
// backlog @1, session @2. tstrCloseCmd (4): sock @0, session @2.
// tstrRecvCmd (8): u32Timeoutmsec @0 (0xFFFFFFFF: none), sock @4,
// session @6. tstrSendCmd (16): sock @0, u16DataSize @2, address @4,
// session @12; the data at send_data_offset after the HIF header.
const uint8_t  bind_bytes			= 12;
const uint8_t  connect_cmd_bytes	= 12;
const uint8_t  listen_bytes			= 4;
const uint8_t  close_bytes			= 4;
const uint8_t  recv_cmd_bytes		= 8;
const uint8_t  send_cmd_bytes		= 16;
// TCP_TX_PACKET_OFFSET: 14 + 34 - 8 + 40. Room for the firmware's own
// headers in front of the data, so it needn't copy it.
const uint16_t send_data_offset		= 80;

// Replies. tstrBindReply and tstrListenReply (4): sock @0, s8Status @1,
// session @2. tstrAcceptReply (12): peer address @0, sListenSock @8,
// sConnectedSock @9 (negative: none), u16AppDataOffset @10.
// tstrConnectReply (4): sock @0, s8Error @1, u16AppDataOffset @2.
// tstrSendReply (8): sock @0, s16SentBytes @2, session @4.
// tstrRecvReply (16): peer address @0, s16RecvStatus @8 (bytes; 0 or
// negative: closed or an error), u16DataOffset @10 (where the data
// starts, from the reply's start), sock @12, session @14.
// tstrDnsReply (68): the name @0 (64), u32HostIP @64 (network order; 0:
// not found).
const uint8_t bind_reply_bytes		= 4;
const uint8_t accept_reply_bytes	= 12;
const uint8_t connect_reply_bytes	= 4;
const uint8_t send_reply_bytes		= 8;
const uint8_t recv_reply_bytes		= 16;
const uint8_t dns_reply_bytes		= 68;

const int8_t sock_err_none			= 0;
const int8_t sock_err_addr_in_use	= -2;
const int8_t sock_err_conn_aborted	= -12;	// the peer closed or reset
const int8_t sock_err_timeout		= -13;

// The largest control structure any reply carries (tstrDnsReply): the
// driver reads the header and this much in one go.
const uint8_t max_reply_bytes		= dns_reply_bytes;

// ---- little-endian helpers ----
inline uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t* p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
	       (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline void put16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
inline void put32(uint8_t* p, uint32_t v) {
	p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8);
	p[2] = static_cast<uint8_t>(v >> 16); p[3] = static_cast<uint8_t>(v >> 24);
}

}  // namespace WINC

#endif /* WINC_PROTOCOL_H_ */
