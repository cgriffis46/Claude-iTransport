// lwIP's options for Lwip_test: the full stack with sockets, threads
// from the Unix port, and only the loopback interface (127.0.0.1).
// Close to an STM32 CubeMX lwIP configuration in the parts that matter
// here (NO_SYS 0, sockets, select, core locking), with more memory.
#pragma once

#define NO_SYS                          0
#define LWIP_SOCKET                     1
#define LWIP_NETCONN                    1
#define LWIP_COMPAT_SOCKETS             0   // the lwip_ names only: no clash with the host's
#define LWIP_POSIX_SOCKETS_IO_NAMES     0
#define LWIP_SOCKET_SELECT              1
#define LWIP_TIMEVAL_PRIVATE            0   // the host's struct timeval
#define LWIP_TCPIP_CORE_LOCKING         1
#define SYS_LIGHTWEIGHT_PROT            1

#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_TCP                        1
#define LWIP_UDP                        1
#define LWIP_DHCP                       0
#define LWIP_DNS                        0
#define LWIP_HAVE_LOOPIF                1
#define LWIP_NETIF_LOOPBACK             1
#define SO_REUSE                        1

#define MEM_ALIGNMENT                   8
#define MEM_SIZE                        (512 * 1024)
#define MEMP_NUM_PBUF                   256
#define MEMP_NUM_TCP_PCB                32
#define MEMP_NUM_TCP_PCB_LISTEN         8
#define MEMP_NUM_TCP_SEG                512
#define MEMP_NUM_NETCONN                32
#define MEMP_NUM_NETBUF                 64
#define MEMP_NUM_UDP_PCB                8
#define MEMP_NUM_SYS_TIMEOUT            (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 8)
#define PBUF_POOL_SIZE                  256

#define TCP_MSS                         1460
#define TCP_WND                         (8 * TCP_MSS)
#define TCP_SND_BUF                     (8 * TCP_MSS)
#define TCP_SND_QUEUELEN                (4 * TCP_SND_BUF / TCP_MSS)
#define TCP_LISTEN_BACKLOG              1

#define TCPIP_MBOX_SIZE                 128
#define DEFAULT_TCP_RECVMBOX_SIZE       128
#define DEFAULT_UDP_RECVMBOX_SIZE       32
#define DEFAULT_ACCEPTMBOX_SIZE         16
#define DEFAULT_THREAD_STACKSIZE        0
#define TCPIP_THREAD_STACKSIZE          0

#define LWIP_STATS                      0
#define LWIP_NETIF_API                  0
