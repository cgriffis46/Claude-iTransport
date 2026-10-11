// The numbers in WincProtocol.h, checked against Microchip's own headers
// (WINC_REF_DIR, built with uint32 as a 32-bit type: see WincRef_test).
#include "driver/include/m2m_types.h"
#include "socket/include/m2m_socket_host_if.h"
#include "driver/source/m2m_hif.h"
#include "driver/source/nmasic.h"
#include "driver/source/nmdrv.h"
#include "WincProtocol.h"

using namespace WINC;
static_assert(group_wifi == M2M_REQ_GROUP_WIFI && group_ip == M2M_REQ_GROUP_IP && group_hif == M2M_REQ_GROUP_HIF, "groups");
static_assert(req_data_pkt == M2M_REQ_DATA_PKT && hif_header_bytes == M2M_HIF_HDR_OFFSET && hif_max_bytes == M2M_HIF_MAX_PACKET_SIZE, "HIF");
static_assert(reg_nmi_state == NMI_STATE_REG && reg_bootrom == BOOTROM_REG && reg_wait_for_host == M2M_WAIT_FOR_HOST_REG &&
              reg_gp_1 == rNMI_GP_REG_1 && reg_gp_2 == rNMI_GP_REG_2 && reg_chip_id == NMI_CHIPID, "registers");
static_assert(finish_boot_rom == M2M_FINISH_BOOT_ROM && start_firmware == M2M_START_FIRMWARE && finish_init_state == M2M_FINISH_INIT_STATE, "boot");
static_assert(gp1_use_pmu == rHAVE_USE_PMU_BIT && gp1_reserved1 == rHAVE_RESERVED1_BIT && rev_3a0 == REV_3A0 && rev_b0 == REV_B0, "conf");
static_assert(host_version_info == M2M_MAKE_VERSION_INFO(19, 5, 2, 19, 5, 2) && host_driver_version == M2M_MAKE_VERSION(19, 5, 2) &&
              min_firmware_version == M2M_MAKE_VERSION(19, 5, 0), "versions");
static_assert(M2M_RELEASE_VERSION_MAJOR_NO == 19 && M2M_RELEASE_VERSION_MINOR_NO == 5 && M2M_RELEASE_VERSION_PATCH_NO == 2, "the reference is 19.5.2");
static_assert(wifi_req_current_rssi == M2M_WIFI_REQ_CURRENT_RSSI && wifi_resp_current_rssi == M2M_WIFI_RESP_CURRENT_RSSI &&
              wifi_req_enable_sntp == M2M_WIFI_REQ_ENABLE_SNTP_CLIENT && wifi_req_get_sys_time == M2M_WIFI_REQ_GET_SYS_TIME &&
              wifi_resp_get_sys_time == M2M_WIFI_RESP_GET_SYS_TIME && wifi_req_connect == M2M_WIFI_REQ_CONNECT &&
              wifi_req_disconnect == M2M_WIFI_REQ_DISCONNECT && wifi_resp_con_state == M2M_WIFI_RESP_CON_STATE_CHANGED &&
              wifi_req_dhcp_conf == M2M_WIFI_REQ_DHCP_CONF && wifi_resp_ip_conflict == M2M_WIFI_RESP_IP_CONFLICT, "Wi-Fi opcodes");
static_assert(ip_req_static_ip_conf == M2M_IP_REQ_STATIC_IP_CONF && ip_req_enable_dhcp == M2M_IP_REQ_ENABLE_DHCP &&
              ip_req_disable_dhcp == M2M_IP_REQ_DISABLE_DHCP, "IP opcodes");
static_assert(sock_cmd_bind == SOCKET_CMD_BIND && sock_cmd_listen == SOCKET_CMD_LISTEN && sock_cmd_accept == SOCKET_CMD_ACCEPT &&
              sock_cmd_connect == SOCKET_CMD_CONNECT && sock_cmd_send == SOCKET_CMD_SEND && sock_cmd_recv == SOCKET_CMD_RECV &&
              sock_cmd_close == SOCKET_CMD_CLOSE && sock_cmd_dns_resolve == SOCKET_CMD_DNS_RESOLVE, "socket opcodes");
static_assert(tcp_sockets == TCP_SOCK_MAX && socket_max_send == SOCKET_BUFFER_MAX_LENGTH && hostname_max == HOSTNAME_MAX_SIZE && af_inet == AF_INET, "sockets");
static_assert(sec_open == M2M_WIFI_SEC_OPEN && sec_wpa_psk == M2M_WIFI_SEC_WPA_PSK && channel_all == M2M_WIFI_CH_ALL &&
              max_ssid_len + 1 == M2M_MAX_SSID_LEN && max_psk_len + 1 == M2M_MAX_PSK_LEN, "join");
static_assert(wifi_connected == M2M_WIFI_CONNECTED && wifi_disconnected == M2M_WIFI_DISCONNECTED && err_scan_fail == M2M_ERR_SCAN_FAIL &&
              err_join_fail == M2M_ERR_JOIN_FAIL && err_auth_fail == M2M_ERR_AUTH_FAIL && err_assoc_fail == M2M_ERR_ASSOC_FAIL, "states");
static_assert(sock_err_none == SOCK_ERR_NO_ERROR && sock_err_addr_in_use == SOCK_ERR_ADDR_ALREADY_IN_USE &&
              sock_err_conn_aborted == SOCK_ERR_CONN_ABORTED && sock_err_timeout == SOCK_ERR_TIMEOUT, "errors");
// Layouts (the build makes uint32 32 bits, as on the module and an STM32).
static_assert(sizeof(uint32) == 4, "uint32 must be 32 bits for these layouts");
static_assert(sizeof(tstrM2mWifiConnect) == connect_bytes && offsetof(tstrM2mWifiConnect, strSec) == connect_psk &&
              offsetof(tstrM2mWifiConnect, u16Ch) == connect_channel && offsetof(tstrM2mWifiConnect, au8SSID) == connect_ssid &&
              offsetof(tstrM2mWifiConnect, u8NoSaveCred) == connect_no_save &&
              offsetof(tstrM2mWifiConnect, strSec) + offsetof(tstrM2MWifiSecInfo, u8SecType) == connect_sec_type, "tstrM2mWifiConnect");
static_assert(sizeof(tstrM2MIPConfig) == ip_config_bytes && offsetof(tstrM2MIPConfig, u32Gateway) == 4 &&
              offsetof(tstrM2MIPConfig, u32DNS) == 8 && offsetof(tstrM2MIPConfig, u32SubnetMask) == 12, "tstrM2MIPConfig");
static_assert(sizeof(tstrSystemTime) == sys_time_bytes && sizeof(tstrM2mRev) == rev_bytes &&
              offsetof(tstrM2mRev, u8FirmwareMajor) == 4 && offsetof(tstrM2mRev, u8DriverMajor) == 7, "time, revision");
static_assert(sizeof(tstrGpRegs) == 8 && offsetof(tstrGpRegs, u32Firmware_Ota_rev) == 4, "tstrGpRegs");
static_assert(sizeof(tstrSockAddr) == sockaddr_bytes && sizeof(tstrBindCmd) == bind_bytes && offsetof(tstrBindCmd, sock) == 8 &&
              offsetof(tstrBindCmd, u16SessionID) == 10 && sizeof(tstrListenCmd) == listen_bytes &&
              sizeof(tstrConnectCmd) == connect_cmd_bytes && offsetof(tstrConnectCmd, sock) == 8 &&
              offsetof(tstrConnectCmd, u16SessionID) == 10 && sizeof(tstrRecvCmd) == recv_cmd_bytes &&
              offsetof(tstrRecvCmd, sock) == 4 && offsetof(tstrRecvCmd, u16SessionID) == 6 && sizeof(tstrSendCmd) == send_cmd_bytes &&
              offsetof(tstrSendCmd, u16DataSize) == 2 && offsetof(tstrSendCmd, strAddr) == 4 && offsetof(tstrSendCmd, u16SessionID) == 12, "requests");
static_assert(sizeof(tstrBindReply) == bind_reply_bytes && sizeof(tstrListenReply) == bind_reply_bytes &&
              sizeof(tstrAcceptReply) == accept_reply_bytes && offsetof(tstrAcceptReply, sListenSock) == 8 &&
              offsetof(tstrAcceptReply, sConnectedSock) == 9 && sizeof(tstrConnectReply) == connect_reply_bytes &&
              offsetof(tstrConnectReply, s8Error) == 1 && sizeof(tstrSendReply) == send_reply_bytes &&
              offsetof(tstrSendReply, s16SentBytes) == 2 && offsetof(tstrSendReply, u16SessionID) == 4 &&
              sizeof(tstrRecvReply) == recv_reply_bytes && offsetof(tstrRecvReply, s16RecvStatus) == 8 &&
              offsetof(tstrRecvReply, u16DataOffset) == 10 && offsetof(tstrRecvReply, sock) == 12 &&
              offsetof(tstrRecvReply, u16SessionID) == 14 && sizeof(tstrDnsReply) == dns_reply_bytes &&
              offsetof(tstrDnsReply, u32HostIP) == 64, "replies");
