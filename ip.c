#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "util.h"
#include "net.h"
#include "arp.h"
#include "ip.h"

#include "icmp.h"

#define NET_PROTOCOL_TYPE_IP 0x0800

#define IP_ROUTE_TABLE_SIZE 8

struct ip_route {
    int used;
    ip_addr_t network;
    ip_addr_t netmask;
    ip_addr_t nexthop;
    struct ip_iface *iface;
};

struct ip_protocol {
    struct ip_protocol *next;
    char name[16];
    uint8_t type;
    void (*handler)(const uint8_t *data, size_t len, ip_addr_t src, ip_addr_t dst);
};

const ip_addr_t IP_ADDR_ANY = 0x00000000;
const ip_addr_t IP_ADDR_BROADCAST = 0xffffffff;

static pthread_mutex_t m_ifaces = PTHREAD_MUTEX_INITIALIZER;
static struct ip_iface *ifaces;

static pthread_mutex_t m_routes = PTHREAD_MUTEX_INITIALIZER;
static struct ip_route routes[IP_ROUTE_TABLE_SIZE];

static pthread_mutex_t m_protocols = PTHREAD_MUTEX_INITIALIZER;
static struct ip_protocol *protocols;

int
ip_addr_pton(const char *p, ip_addr_t *n)
{
    char *sp, *ep;
    int idx;
    long ret;

    sp = (char *)p;
    for (idx = 0; idx < 4; idx++) {
        ret = strtol(sp, &ep, 10);
        if (ret < 0 || ret > 255) {
            return -1;
        }
        if (ep == sp) {
            return -1;
        }
        if ((idx == 3 && *ep != '\0') || (idx != 3 && *ep != '.')) {
            return -1;
        }
        ((uint8_t *)n)[idx] = ret;
        sp = ep + 1;
    }
    return 0;
}

char *
ip_addr_ntop(const ip_addr_t *n, char *p, size_t size)
{
    uint8_t *u8;

    u8 = (uint8_t *)n;
    snprintf(p, size, "%d.%d.%d.%d", u8[0], u8[1], u8[2], u8[3]);
    return p;
}

void
ip_dump(const uint8_t *packet, size_t plen)
{
    struct ip_hdr *hdr;
    uint8_t hl;
    uint16_t offset;
    char addr[IP_ADDR_STR_LEN];

    
    hdr = (struct ip_hdr *)packet;
    hl = hdr->vhl & 0x0f;
    flockfile(stderr);
    fprintf(stderr, "     vhl: 0x%02x [v: %u, hl: %u (%u)]\n", hdr->vhl, (hdr->vhl & 0xf0) >> 4, hl, hl << 2);
    fprintf(stderr, "     tos: 0x%02x\n", hdr->tos);
    fprintf(stderr, "     len: %u\n", ntoh16(hdr->len));
    fprintf(stderr, "      id: %u\n", ntoh16(hdr->id));
    offset = ntoh16(hdr->offset);
    fprintf(stderr, "  offset: 0x%04x [flags=%x, offset=%u]\n", offset, (offset & 0xe000) >> 13, offset & 0x1fff);
    fprintf(stderr, "     ttl: %u\n", hdr->ttl);
    fprintf(stderr, "protocol: %u\n", hdr->protocol);
    fprintf(stderr, "     sum: 0x%04x\n", ntoh16(hdr->sum));
    fprintf(stderr, "     src: %s\n", ip_addr_ntop(&hdr->src, addr, sizeof(addr)));
    fprintf(stderr, "     dst: %s\n", ip_addr_ntop(&hdr->dst, addr, sizeof(addr)));
    hexdump(stderr, packet, plen);
    funlockfile(stderr);
}

/*
 * IP ROUTING
 */

static int
ip_route_add(ip_addr_t network, ip_addr_t netmask, ip_addr_t nexthop, struct ip_iface *iface)
{
    struct ip_route *route;

    pthread_mutex_lock(&m_routes);
    for (route = routes; route < array_tailof(routes); route++) {
        if (!route->used) {
            route->used = 1;
            route->network = network;
            route->netmask = netmask;
            route->nexthop = nexthop;
            route->iface = iface;
            pthread_mutex_unlock(&m_routes);
            return 0;
        }
    }
    pthread_mutex_unlock(&m_routes);
    errorf("no free space");
    return -1;
}

#if 0
static int
ip_route_del(struct ip_iface *iface)
{
    struct ip_route *route;

    for (route = route_table; route < array_tailof(route_table); route++) {
        if (route->used) {
            if (route->iface == iface) {
                route->used = 0;
                route->network = IP_ADDR_ANY;
                route->netmask = IP_ADDR_ANY;
                route->nexthop = IP_ADDR_ANY;
                route->iface = NULL;
            }
        }
    }
    return 0;
}
#endif

static struct ip_route *
ip_route_lookup(ip_addr_t dst)
{
    struct ip_route *route, *candidate = NULL;

    pthread_mutex_lock(&m_routes);
    for (route = routes; route < array_tailof(routes); route++) {
        if (route->used && (dst & route->netmask) == route->network) {
            if (!candidate || ntoh32(candidate->netmask) < ntoh32(route->netmask)) {
                candidate = route;
            }
        }
    }
    pthread_mutex_unlock(&m_routes);
    return candidate;
}

/*
 * IP INTERFACE
 */

struct ip_iface *
ip_iface_alloc(const char *unicast, const char *netmask)
{
    struct ip_iface *iface;

    if (!unicast || !netmask) {
        errorf("invalid arguments");
        return NULL;
    }
    iface = malloc(sizeof(struct ip_iface));
    if (!iface) {
        errorf("malloc() failure");
        return NULL;
    }
    NET_IFACE(iface)->next = NULL;
    NET_IFACE(iface)->dev = NULL;
    NET_IFACE(iface)->family = NET_IFACE_FAMILY_IPV4;
    NET_IFACE(iface)->alen = IP_ADDR_LEN;
    iface->next = NULL;
    if (ip_addr_pton(unicast, &iface->unicast) == -1) {
        errorf("ip_addr_pton() failure, unicast=%s", unicast);
        free(iface);
        return NULL;
    }
    if (ip_addr_pton(netmask, &iface->netmask) == -1) {
        errorf("ip_addr_pton() failure, netmask=%s", netmask);
        free(iface);
        return NULL;
    }
    iface->broadcast = (iface->unicast & iface->netmask) | ~iface->netmask;
    return iface;
}

int
ip_iface_register(struct net_device *dev, struct ip_iface *iface)
{
    char addr1[IP_ADDR_STR_LEN], addr2[IP_ADDR_STR_LEN];

    if (ip_route_add(iface->unicast & iface->netmask, iface->netmask, IP_ADDR_ANY, iface) == -1) {
        errorf("ip_route_add() failure");
        return -1;
    }
    if (net_device_add_iface(dev, NET_IFACE(iface)) == -1) {
        errorf("net_device_add_iface() failure");
        return -1;
    }
    pthread_mutex_lock(&m_ifaces);
    iface->next = ifaces;
    ifaces = iface;
    pthread_mutex_unlock(&m_ifaces);
    infof("registerd: %s %s", ip_addr_ntop(&iface->unicast, addr1, sizeof(addr1)), ip_addr_ntop(&iface->netmask, addr2, sizeof(addr2)));
    return 0;
}

struct ip_iface *
ip_iface_by_addr(ip_addr_t addr)
{
    struct ip_iface *entry;

    pthread_mutex_lock(&m_ifaces);
    for (entry = ifaces; entry; entry = entry->next) {
        if (entry->unicast == addr) {
            break;
        }
    }
    pthread_mutex_unlock(&m_ifaces);
    return entry;
}

struct ip_iface *
ip_iface_by_peer(ip_addr_t peer)
{
    struct ip_route *route;

    route = ip_route_lookup(peer);
    if (!route) {
        return NULL;
    }
    return route->iface;
}

int
ip_set_default_gateway(struct ip_iface *iface, const char *gateway)
{
    ip_addr_t gw;

    if (ip_addr_pton(gateway, &gw) == -1) {
        errorf("ip_addr_pton() failure, gateway=%s", gateway);
        return -1;
    }
    if (ip_route_add(IP_ADDR_ANY, IP_ADDR_ANY, gw, iface) == -1) {
        errorf("ip_route_add() failure");
        return -1;
    }
    return 0;
}

static void
ip_input(struct net_device *dev, const uint8_t *data, size_t len)
{
    struct ip_hdr *hdr;
    uint16_t hlen;
    struct ip_iface *iface;
    struct ip_protocol *proto;

    if (len < sizeof(struct ip_hdr)) {
        errorf("ip packet too small");
        return;
    }
    hdr = (struct ip_hdr *)data;
    if ((hdr->vhl >> 4) != IP_VERSION_IPV4) {
        errorf("ip packet version error");
        return;
    }
    hlen = (hdr->vhl & 0x0f) << 2;
    if (len < hlen || len < ntoh16(hdr->len)) {
        errorf("ip packet length error");
        return;
    }
    if (!hdr->ttl) {
        errorf("ip packet was dead (TTL=0)");
        return;
    }
    if (cksum16((uint16_t *)hdr, hlen, 0) != 0) {
        errorf("ip pakcet checksum error");
        return;
    }
    iface = (struct ip_iface *)net_device_get_iface(dev, NET_IFACE_FAMILY_IPV4);
    if (!iface) {
        errorf("<%s> ip interface does not registerd", dev->name);
        return;
    }
    if (hdr->dst != iface->unicast) {
        if (hdr->dst != iface->broadcast && hdr->dst != IP_ADDR_BROADCAST) {
            /* for other host */
            return;
        }
    }
    debugf("<%s> arrived %zd bytes data", dev->name, len);
    ip_dump(data, len);
    pthread_mutex_lock(&m_protocols);
    for (proto = protocols; proto; proto = proto->next) {
        if (proto->type == hdr->protocol) {
            proto->handler((uint8_t *)hdr + hlen, len - hlen, hdr->src, hdr->dst);
            break;
        }
    }
    pthread_mutex_unlock(&m_protocols);
    if (!proto) {
        /* unsupported protocol */
        return;
    }
}

static uint16_t
ip_generate_id(void)
{
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    static uint16_t id = 128;
    uint16_t ret;

    pthread_mutex_lock(&mutex);
    ret = id++;
    pthread_mutex_unlock(&mutex);
    return ret;
}

static int
ip_output_device(struct ip_iface *iface, uint8_t *data, size_t len, ip_addr_t dst)
{
    uint8_t ha[NET_DEVICE_ADDR_LEN];
    char addr[IP_ADDR_STR_LEN];
    ssize_t ret;

    do {
        if (NET_IFACE(iface)->dev->flags & NET_DEVICE_FLAG_NOARP) {
            memset(ha, 0, sizeof(ha));
            break;
        }
        if (dst == iface->broadcast || dst == IP_ADDR_BROADCAST) {
            memcpy(ha, NET_IFACE(iface)->dev->broadcast, NET_IFACE(iface)->dev->alen);
            break;
        }
        ret = arp_resolve(NET_IFACE(iface), dst, ha);
        if (ret != ARP_RESOLVE_FOUND) {
            /* unresolved at the moment */
            return ret;
        }
    } while (0);
    debugf("<%s> %zd bytes data to %s", NET_IFACE(iface)->dev->name, len, ip_addr_ntop(&dst, addr, sizeof(addr)));
    ip_dump((uint8_t *)data, len);
    return net_device_output(NET_IFACE(iface)->dev, NET_PROTOCOL_TYPE_IP, data, len, ha);
}

static ssize_t
ip_output_core(struct ip_iface *iface, uint8_t protocol, const uint8_t *data, size_t len, ip_addr_t src, ip_addr_t dst, ip_addr_t nexthop, uint16_t id, uint16_t offset)
{
    uint8_t buf[4096];
    struct ip_hdr *hdr;
    uint16_t hlen;

    hdr = (struct ip_hdr *)buf;
    hlen = sizeof(struct ip_hdr);
    hdr->vhl = (IP_VERSION_IPV4 << 4) | (hlen >> 2);
    hdr->tos = 0;
    hdr->len = hton16(hlen + len);
    hdr->id = hton16(id);
    hdr->offset = hton16(offset);
    hdr->ttl = 0xff;
    hdr->protocol = protocol;
    hdr->sum = 0;
    hdr->src = src;
    hdr->dst = dst;
    hdr->sum = cksum16((uint16_t *)hdr, hlen, 0);
    memcpy(hdr + 1, data, len);
    return ip_output_device(iface, buf, hlen + len, nexthop);
}

ssize_t
ip_output(uint8_t protocol, const uint8_t *data, size_t len, ip_addr_t src, ip_addr_t dst)
{
    struct ip_iface *iface;
    ip_addr_t nexthop;
    struct ip_route *route;
    uint16_t id;

    if (dst == IP_ADDR_BROADCAST) {
        if (src == IP_ADDR_ANY) {
            errorf("need specify source address to send to broadcast address");
            return -1;
        }
        iface = ip_iface_by_addr(src);
        if (!iface) {
            errorf("iface not found");
            return -1;
        }
        nexthop = dst;
    } else {
        route = ip_route_lookup(dst);
        if (!route) {
            errorf("ip no route to host");
            return -1;
        }
        if (src == IP_ADDR_ANY) {
            src = route->iface->unicast;
        }
        nexthop = (route->nexthop != IP_ADDR_ANY) ? route->nexthop : dst;
        iface = route->iface;
    }
    if (len > (size_t)(NET_IFACE(iface)->dev->mtu - IP_HDR_SIZE_MIN)) {
        /* flagmentation does not support */
        errorf("data is too long");
        return -1;
    }
    id = ip_generate_id();
    if (ip_output_core(iface, protocol, data, len, src, dst, nexthop, id, 0) == -1) {
        errorf("ip_output_core() failure");
        return -1;
    }
    return len;
}

int
ip_protocol_register(const char *name, uint8_t type, void (*handler)(const uint8_t *data, size_t len, ip_addr_t src, ip_addr_t dst))
{
    struct ip_protocol *entry;

    pthread_mutex_lock(&m_protocols);
    for (entry = protocols; entry; entry = entry->next) {
        if (entry->type == type) {
            errorf("already registered: %s (0x%02x)", entry->name, entry->type);
            pthread_mutex_unlock(&m_protocols);
            return -1;
        }
    }
    entry = malloc(sizeof(struct ip_protocol));
    if (!entry) {
        errorf("malloc() failure");
        pthread_mutex_unlock(&m_protocols);
        return -1;
    }
    entry->next = protocols;
    strncpy(entry->name, name, sizeof(entry->name)-1);
    entry->type = type;
    entry->handler = handler;
    protocols = entry;
    pthread_mutex_unlock(&m_protocols);
    infof("registerd: %s (0x%02x)", name, type);
    return 0;
}

int
ip_init(void)
{
    net_protocol_register(NET_PROTOCOL_TYPE_IP, ip_input);
    return 0;
}
