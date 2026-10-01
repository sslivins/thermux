/**
 * @file bip_espidf.c
 * @brief BACnet/IP datalink port for ESP-IDF/lwIP sockets
 */

#include "bacnet/bacdcode.h"
#include "bacnet/bacint.h"
#include "bacnet/datalink/bip.h"
#include "bacnet/datalink/bvlc.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "bacnet-bip";
static int s_socket = -1;
static uint16_t s_port;
static uint16_t s_broadcast_port;
static struct in_addr s_addr;
static struct in_addr s_broadcast;
static struct in_addr s_gateway;
static uint8_t s_prefix;
static char s_interface[16] = "auto";
static bool s_port_changed;
static bool s_debug;

static bool refresh_netif(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (netif == NULL) {
        netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    }
    if (netif == NULL) {
        return false;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        return false;
    }
    s_addr.s_addr = ip.ip.addr;
    s_gateway.s_addr = ip.gw.addr;
    s_broadcast.s_addr = ip.ip.addr | ~ip.netmask.addr;
    uint32_t mask = ntohl(ip.netmask.addr);
    s_prefix = 0;
    while (mask & 0x80000000U) {
        s_prefix++;
        mask <<= 1;
    }
    return true;
}

int bip_get_socket(void) { return s_socket; }
int bip_get_broadcast_socket(void) { return s_socket; }
void bip_debug_enable(void) { s_debug = true; }
void bip_debug_disable(void) { s_debug = false; }

void bip_set_port(uint16_t port)
{
    if (s_port != port) {
        s_port_changed = true;
    }
    s_port = port;
}

void bip_set_broadcast_port(uint16_t port) { s_broadcast_port = port; }
bool bip_port_changed(void)
{
    bool changed = s_port_changed;
    s_port_changed = false;
    return changed;
}
uint16_t bip_get_port(void) { return s_port; }
uint16_t bip_get_broadcast_port(void) { return s_broadcast_port ? s_broadcast_port : s_port; }
const char *bip_get_interface(void) { return s_interface; }

void bip_set_interface(const char *ifname)
{
    if (ifname && ifname[0]) {
        strlcpy(s_interface, ifname, sizeof(s_interface));
    }
    refresh_netif();
}

/* Re-read the interface address (DHCP may hand out a new one after a link
 * drop). Returns true if the address changed. */
bool bip_espidf_refresh_address(void)
{
    struct in_addr before = s_addr;
    if (!refresh_netif()) {
        return false;
    }
    return before.s_addr != s_addr.s_addr;
}

bool bip_valid(void) { return s_socket >= 0 && s_addr.s_addr != 0; }

void bip_get_my_address(BACNET_ADDRESS *addr)
{
    if (addr == NULL) return;
    uint16_t port = htons(s_port);
    memset(addr, 0, sizeof(*addr));
    addr->mac_len = 6;
    memcpy(&addr->mac[0], &s_addr.s_addr, 4);
    memcpy(&addr->mac[4], &port, 2);
    addr->net = 0;
}

void bip_get_broadcast_address(BACNET_ADDRESS *dest)
{
    if (dest == NULL) return;
    uint16_t port = htons(bip_get_broadcast_port());
    memset(dest, 0, sizeof(*dest));
    dest->mac_len = 6;
    memcpy(&dest->mac[0], &s_broadcast.s_addr, 4);
    memcpy(&dest->mac[4], &port, 2);
    dest->net = BACNET_BROADCAST_NETWORK;
}

bool bip_set_addr(const BACNET_IP_ADDRESS *addr)
{
    if (addr == NULL) return false;
    memcpy(&s_addr.s_addr, addr->address, 4);
    return true;
}

bool bip_get_addr(BACNET_IP_ADDRESS *addr)
{
    if (addr == NULL) return false;
    memcpy(addr->address, &s_addr.s_addr, 4);
    addr->port = s_port;
    return true;
}

bool bip_get_addr_by_name(const char *host_name, BACNET_IP_ADDRESS *addr)
{
    (void)host_name;
    (void)addr;
    return false;
}

bool bip_set_broadcast_addr(const BACNET_IP_ADDRESS *addr)
{
    if (addr == NULL) return false;
    memcpy(&s_broadcast.s_addr, addr->address, 4);
    s_broadcast_port = addr->port;
    return true;
}

bool bip_get_broadcast_addr(BACNET_IP_ADDRESS *addr)
{
    if (addr == NULL) return false;
    memcpy(addr->address, &s_broadcast.s_addr, 4);
    addr->port = bip_get_broadcast_port();
    return true;
}

bool bip_get_gateway_addr(BACNET_IP_ADDRESS *addr)
{
    if (addr == NULL) return false;
    memcpy(addr->address, &s_gateway.s_addr, 4);
    addr->port = 0;
    return true;
}

bool bip_set_subnet_prefix(uint8_t prefix)
{
    s_prefix = prefix;
    return true;
}

uint8_t bip_get_subnet_prefix(void) { return s_prefix; }
int bip_set_broadcast_binding(const char *ip4_broadcast)
{
    (void)ip4_broadcast;
    return 0;
}

bool bip_init(const char *ifname)
{
    if (ifname) {
        bip_set_interface(ifname);
    } else {
        bip_set_interface("auto");
    }
    if (s_port == 0) {
        s_port = 47808;
    }
    if (s_addr.s_addr == 0 && !refresh_netif()) {
        ESP_LOGW(TAG, "No IP address for BACnet/IP yet");
        return false;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket failed errno=%d", errno);
        return false;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct timeval tv = {.tv_sec = 0, .tv_usec = 20000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(s_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind UDP %u failed errno=%d", s_port, errno);
        close(fd);
        return false;
    }
    s_socket = fd;
    s_port_changed = false;
    ESP_LOGI(TAG, "BACnet/IP bound to UDP %u", s_port);
    return true;
}

void bip_cleanup(void)
{
    if (s_socket >= 0) {
        close(s_socket);
        s_socket = -1;
    }
}

int bip_send_mpdu(const BACNET_IP_ADDRESS *dest, const uint8_t *mtu, uint16_t mtu_len)
{
    if (s_socket < 0 || dest == NULL) return -1;
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(dest->port)};
    memcpy(&sa.sin_addr.s_addr, dest->address, 4);
    int sent = sendto(s_socket, mtu, mtu_len, 0, (struct sockaddr *)&sa, sizeof(sa));
    if (s_debug && sent < 0) ESP_LOGW(TAG, "sendto errno=%d", errno);
    return sent;
}

static int send_raw(const struct in_addr *addr, uint16_t port, const uint8_t *mtu, uint16_t mtu_len)
{
    BACNET_IP_ADDRESS dest;
    memcpy(dest.address, &addr->s_addr, 4);
    dest.port = port;
    return bip_send_mpdu(&dest, mtu, mtu_len);
}

int bip_send_pdu(BACNET_ADDRESS *dest, BACNET_NPDU_DATA *npdu_data, uint8_t *pdu, unsigned pdu_len)
{
    (void)npdu_data;
    if (s_socket < 0 || dest == NULL || pdu == NULL || pdu_len + 4 > BIP_MPDU_MAX) return -1;
    uint8_t mtu[BIP_MPDU_MAX];
    uint16_t mtu_len;
    if ((dest->net == BACNET_BROADCAST_NETWORK) || ((dest->net > 0) && (dest->len == 0)) || dest->mac_len == 0) {
        mtu_len = (uint16_t)bvlc_encode_original_broadcast(mtu, sizeof(mtu), pdu, pdu_len);
        return send_raw(&s_broadcast, bip_get_broadcast_port(), mtu, mtu_len);
    }
    if (dest->mac_len == 6) {
        struct in_addr addr;
        uint16_t net_port;
        memcpy(&addr.s_addr, &dest->mac[0], 4);
        memcpy(&net_port, &dest->mac[4], 2);
        mtu_len = (uint16_t)bvlc_encode_original_unicast(mtu, sizeof(mtu), pdu, pdu_len);
        return send_raw(&addr, ntohs(net_port), mtu, mtu_len);
    }
    return -1;
}

uint16_t bip_receive(BACNET_ADDRESS *src, uint8_t *pdu, uint16_t max_pdu, unsigned timeout)
{
    if (s_socket < 0 || src == NULL || pdu == NULL) return 0;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s_socket, &rfds);
    struct timeval tv = {.tv_sec = timeout / 1000, .tv_usec = (int)(timeout % 1000) * 1000};
    int ready = select(s_socket + 1, &rfds, NULL, NULL, &tv);
    if (ready <= 0) return 0;

    /* Receive straight into the caller's buffer and strip the BVLC header in
     * place, rather than bouncing through a second 1.5 KB stack buffer. */
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    int received = recvfrom(s_socket, pdu, max_pdu, 0, (struct sockaddr *)&from, &from_len);
    if (received < 4 || pdu[0] != BVLL_TYPE_BACNET_IP) return 0;
    if (from.sin_addr.s_addr == s_addr.s_addr && ntohs(from.sin_port) == s_port) return 0;

    uint16_t bvlc_len = 0;
    decode_unsigned16(&pdu[2], &bvlc_len);
    if (bvlc_len > received) return 0;

    uint8_t function = pdu[1];
    uint16_t offset = 0;
    if (function == BVLC_ORIGINAL_UNICAST_NPDU || function == BVLC_ORIGINAL_BROADCAST_NPDU) {
        offset = 4;
        memset(src, 0, sizeof(*src));
        src->mac_len = 6;
        memcpy(&src->mac[0], &from.sin_addr.s_addr, 4);
        memcpy(&src->mac[4], &from.sin_port, 2);
    } else if (function == BVLC_FORWARDED_NPDU && bvlc_len >= 10) {
        offset = 10;
        memset(src, 0, sizeof(*src));
        src->mac_len = 6;
        memcpy(&src->mac[0], &pdu[4], 4);
        memcpy(&src->mac[4], &pdu[8], 2);
    } else {
        return 0;
    }
    if (bvlc_len < offset) return 0;
    uint16_t npdu_len = bvlc_len - offset;
    memmove(pdu, &pdu[offset], npdu_len);
    return npdu_len;
}
