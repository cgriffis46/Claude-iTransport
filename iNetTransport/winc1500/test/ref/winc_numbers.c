/*
 * winc_numbers.c: prints the ATWINC1500 host-interface numbers that
 * WincProtocol.h uses, from Microchip's own headers, for the 32-bit ARM
 * ABI the module's firmware and an STM32 share. Run it again after
 * changing WincProtocol.h, and compare.
 *
 * Microchip's host driver 19.5.2 (BSD-3, Atmel 2016-2017), as shipped in
 * Arduino's WiFi101 library (git clone https://github.com/arduino-libraries/WiFi101):
 *
 *   cd WiFi101/src
 *   arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -I. -S <this file> -o - | grep '^->'
 *
 * (Compiled for ARM, not the PC: the headers' uint32 is "unsigned long",
 * 8 bytes on a 64-bit PC, which would give the wrong struct layouts. The
 * values come out of the assembly, as the Linux kernel's asm-offsets.)
 */

#include <stddef.h>
#define V(x) __asm__ volatile("\n->" #x " %0" :: "i"((int)(x)))
#define Z(t) __asm__ volatile("\n->sizeof_" #t " %0" :: "i"((int)sizeof(t)))
#define O(t,f) __asm__ volatile("\n->  " #t "." #f " %0" :: "i"((int)offsetof(t,f)))
#include "driver/include/m2m_types.h"
#include "socket/include/m2m_socket_host_if.h"
#include "driver/source/m2m_hif.h"
#include "driver/source/nmdrv.h"
#include "driver/source/nmasic.h"
int main(void){
 V(M2M_REQ_GROUP_WIFI);V(M2M_REQ_GROUP_IP);V(M2M_REQ_GROUP_HIF);V(M2M_REQ_DATA_PKT);V(M2M_HIF_HDR_OFFSET);V(M2M_HIF_MAX_PACKET_SIZE);
 V(M2M_WIFI_REQ_RESTART);V(M2M_WIFI_REQ_CURRENT_RSSI);V(M2M_WIFI_RESP_CURRENT_RSSI);V(M2M_WIFI_REQ_GET_CONN_INFO);V(M2M_WIFI_RESP_CONN_INFO);
 V(M2M_WIFI_REQ_SET_SYS_TIME);V(M2M_WIFI_REQ_ENABLE_SNTP_CLIENT);V(M2M_WIFI_REQ_DISABLE_SNTP_CLIENT);V(M2M_WIFI_RESP_MEMORY_RECOVER);
 V(M2M_WIFI_REQ_GET_SYS_TIME);V(M2M_WIFI_RESP_GET_SYS_TIME);
 V(M2M_WIFI_REQ_CONNECT);V(M2M_WIFI_REQ_DEFAULT_CONNECT);V(M2M_WIFI_RESP_DEFAULT_CONNECT);V(M2M_WIFI_REQ_DISCONNECT);V(M2M_WIFI_RESP_CON_STATE_CHANGED);
 V(M2M_WIFI_REQ_SLEEP);V(M2M_WIFI_REQ_DHCP_CONF);V(M2M_WIFI_RESP_IP_CONFIGURED);V(M2M_WIFI_RESP_IP_CONFLICT);
 V(M2M_IP_REQ_STATIC_IP_CONF);V(M2M_IP_REQ_ENABLE_DHCP);V(M2M_IP_REQ_DISABLE_DHCP);
 V(M2M_WIFI_DISCONNECTED);V(M2M_WIFI_CONNECTED);V(M2M_WIFI_SEC_OPEN);V(M2M_WIFI_SEC_WPA_PSK);V(M2M_WIFI_CH_ALL);
 V(M2M_MAX_SSID_LEN);V(M2M_MAX_PSK_LEN);V(M2M_ERR_SCAN_FAIL);V(M2M_ERR_JOIN_FAIL);V(M2M_ERR_AUTH_FAIL);V(M2M_ERR_ASSOC_FAIL);V(M2M_ERR_CONN_INPROGRESS);
 V(SOCKET_CMD_BIND);V(SOCKET_CMD_LISTEN);V(SOCKET_CMD_ACCEPT);V(SOCKET_CMD_CONNECT);V(SOCKET_CMD_SEND);V(SOCKET_CMD_RECV);V(SOCKET_CMD_CLOSE);V(SOCKET_CMD_DNS_RESOLVE);V(SOCKET_CMD_SET_SOCKET_OPTION);
 V(TCP_SOCK_MAX);V(M2M_MAKE_VERSION_INFO(19,5,2,19,5,2));V(M2M_MAKE_VERSION(19,5,0));V(rHAVE_RESERVED1_BIT);V(rHAVE_USE_PMU_BIT);V(NMI_STATE_REG);V(BOOTROM_REG);V(NMI_REV_REG);V(M2M_WAIT_FOR_HOST_REG);V(M2M_FINISH_INIT_STATE);V(M2M_FINISH_BOOT_ROM);V(M2M_START_FIRMWARE);V(rNMI_GP_REG_1);V(rNMI_GP_REG_2);V(REV_3A0);V(REV_B0);V(UDP_SOCK_MAX);V(MAX_SOCKET);V(SOCKET_BUFFER_MAX_LENGTH);V(HOSTNAME_MAX_SIZE);V(AF_INET);
 V(SOCK_ERR_NO_ERROR);V(SOCK_ERR_INVALID_ADDRESS);V(SOCK_ERR_ADDR_ALREADY_IN_USE);V(SOCK_ERR_MAX_TCP_SOCK);V(SOCK_ERR_MAX_UDP_SOCK);V(SOCK_ERR_INVALID_ARG);V(SOCK_ERR_MAX_LISTEN_SOCK);V(SOCK_ERR_INVALID);V(SOCK_ERR_ADDR_IS_REQUIRED);V(SOCK_ERR_CONN_ABORTED);V(SOCK_ERR_TIMEOUT);V(SOCK_ERR_BUFFER_FULL);
 V(SO_SET_UDP_SEND_CALLBACK);
 Z(tstrHifHdr);Z(tstrM2mWifiConnect);O(tstrM2mWifiConnect,strSec);O(tstrM2mWifiConnect,u16Ch);O(tstrM2mWifiConnect,au8SSID);O(tstrM2mWifiConnect,u8NoSaveCred);
 Z(tstrM2MWifiSecInfo);O(tstrM2MWifiSecInfo,uniAuth);O(tstrM2MWifiSecInfo,u8SecType);
 Z(tstrM2mWifiStateChanged);O(tstrM2mWifiStateChanged,u8CurrState);O(tstrM2mWifiStateChanged,u8ErrCode);
 Z(tstrM2MIPConfig);O(tstrM2MIPConfig,u32StaticIP);O(tstrM2MIPConfig,u32Gateway);O(tstrM2MIPConfig,u32DNS);O(tstrM2MIPConfig,u32SubnetMask);O(tstrM2MIPConfig,u32DhcpLeaseTime);
 Z(tstrSystemTime);O(tstrSystemTime,u16Year);O(tstrSystemTime,u8Month);O(tstrSystemTime,u8Day);O(tstrSystemTime,u8Hour);O(tstrSystemTime,u8Minute);O(tstrSystemTime,u8Second);
 Z(tstrM2mRev);O(tstrM2mRev,u32Chipid);O(tstrM2mRev,u8FirmwareMajor);O(tstrM2mRev,u8FirmwareMinor);O(tstrM2mRev,u8FirmwarePatch);O(tstrM2mRev,u8DriverMajor);O(tstrM2mRev,u8DriverMinor);O(tstrM2mRev,u8DriverPatch);O(tstrM2mRev,BuildDate);O(tstrM2mRev,BuildTime);O(tstrM2mRev,u16FirmwareSvnNum);
 Z(tstrGpRegs);O(tstrGpRegs,u32Mac_efuse_mib);O(tstrGpRegs,u32Firmware_Ota_rev);
 Z(tstrSockAddr);Z(tstrBindCmd);O(tstrBindCmd,sock);O(tstrBindCmd,u16SessionID);Z(tstrBindReply);Z(tstrListenCmd);Z(tstrListenReply);
 Z(tstrAcceptReply);O(tstrAcceptReply,sListenSock);O(tstrAcceptReply,sConnectedSock);O(tstrAcceptReply,u16AppDataOffset);
 Z(tstrConnectCmd);O(tstrConnectCmd,sock);O(tstrConnectCmd,u8SslFlags);O(tstrConnectCmd,u16SessionID);Z(tstrConnectReply);O(tstrConnectReply,s8Error);O(tstrConnectReply,u16AppDataOffset);
 Z(tstrSendCmd);O(tstrSendCmd,u16DataSize);O(tstrSendCmd,strAddr);O(tstrSendCmd,u16SessionID);Z(tstrSendReply);O(tstrSendReply,s16SentBytes);O(tstrSendReply,u16SessionID);
 Z(tstrRecvCmd);O(tstrRecvCmd,sock);O(tstrRecvCmd,u16SessionID);Z(tstrRecvReply);O(tstrRecvReply,s16RecvStatus);O(tstrRecvReply,u16DataOffset);O(tstrRecvReply,sock);O(tstrRecvReply,u16SessionID);
 Z(tstrDnsReply);O(tstrDnsReply,u32HostIP);Z(tstrSetSocketOptCmd);O(tstrSetSocketOptCmd,sock);O(tstrSetSocketOptCmd,u8Option);O(tstrSetSocketOptCmd,u16SessionID);

 return 0;}
