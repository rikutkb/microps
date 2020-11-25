#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/time.h>

#include "util.h"
#include "net.h"

struct net_protocol {
    struct net_protocol *next;
    uint16_t type;
    pthread_mutex_t mutex;
    struct queue_head queue; /* receive queue */
    void (*handler)(struct net_device *dev, const uint8_t *data, size_t len);
};

struct txq_entry {
    struct queue_entry *next;
    uint8_t dst[NET_DEVICE_ADDR_LEN];
    uint16_t type;
    size_t len;
    uint8_t data[0];
};

struct rxq_entry {
    struct queue_entry *next;
    struct net_device *dev;
    size_t len;
    uint8_t data[0];
};

static pthread_t thread;
volatile sig_atomic_t net_interrupt;

static pthread_mutex_t m_devices = PTHREAD_MUTEX_INITIALIZER;
static struct net_device *devices;
static pthread_mutex_t m_protocols = PTHREAD_MUTEX_INITIALIZER;
static struct net_protocol *protocols;

struct net_device *
net_device_alloc(void (*setup)(struct net_device *net))
{
    struct net_device *dev;

    dev = malloc(sizeof(struct net_device));
    if (!dev) {
        errorf("malloc() failure");
        return NULL;
    }
    memset(dev, 0, sizeof(struct net_device));
    pthread_mutex_init(&dev->mutex, NULL);
    if (setup) {
        setup(dev);
    }
    return dev;
}

int
net_device_register(struct net_device *dev)
{
    static unsigned int index = 0;

    pthread_mutex_lock(&m_devices);
    dev->index = index++;
    snprintf(dev->name, sizeof(dev->name), "net%d", dev->index);
    dev->next = devices;
    devices = dev;
    pthread_mutex_unlock(&m_devices);

    infof("<%s> registerd, type=0x%04x", dev->name, dev->type);
    return 0;
}

int
net_device_transmit(struct net_device *dev, uint16_t type, const uint8_t *data, size_t len, const void *dst)
{
    struct txq_entry *entry;

    debugf("<%s> type=0x%04x len=%zd dst=%p", dev->name, type, len, dst);
    debugdump(data, len);

    entry = malloc(sizeof(struct txq_entry) + len);
    if (!entry) {
        errorf("malloc() failure");
        return -1;
    }
    if (dst) {
        memcpy(entry->dst, dst, dev->alen);
    } else {
        memset(entry->dst, 0, dev->alen);
    }
    entry->type = type;
    entry->len = len;
    memcpy(entry->data, data, len);
    pthread_mutex_lock(&dev->mutex);
    queue_push(&dev->queue, (struct queue_entry *)entry);
    pthread_mutex_unlock(&dev->mutex);
    return 0;
}

int
net_device_received(struct net_device *dev, uint16_t type, const uint8_t *data, size_t len)
{
    struct net_protocol *proto;
    struct rxq_entry *entry;

    debugf("<%s> type=0x%04x len=%zd", dev->name, type, len);
    debugdump(data, len);

    pthread_mutex_lock(&m_protocols);
    for (proto = protocols; proto; proto = proto->next) {
        if (proto->type == type) {
            entry = malloc(sizeof(struct rxq_entry) + len);
            if (!entry) {
                pthread_mutex_unlock(&m_protocols);
                errorf("malloc() failure");
                return -1;
            }
            entry->dev = dev;
            entry->len = len;
            memcpy(entry->data, data, len);
            pthread_mutex_lock(&proto->mutex);
            queue_push(&proto->queue, (struct queue_entry *)entry);
            pthread_mutex_unlock(&proto->mutex);
            break;
        }
    }
    pthread_mutex_unlock(&m_protocols);
    if (!proto) {
        /* unsupported protocol */
        return -1;
    }
    return 0;
}

int
net_protocol_register(uint16_t type, void (*handler)(struct net_device *dev, const uint8_t *data, size_t len))
{
    struct net_protocol *entry;

    pthread_mutex_lock(&m_protocols);
    for (entry = protocols; entry; entry = entry->next) {
        if (entry->type == type) {
            pthread_mutex_unlock(&m_protocols);
            errorf("already registerd: 0x%04x", type);
            return -1;
        }
    }
    entry = malloc(sizeof(struct net_protocol));
    if (!entry) {
        pthread_mutex_unlock(&m_protocols);
        errorf("malloc() failure");
        return -1;
    }
    memset(entry, 0 , sizeof(struct net_protocol));
    entry->next = protocols;
    entry->type = type;
    pthread_mutex_init(&entry->mutex, NULL);
    entry->handler = handler;
    protocols = entry;
    pthread_mutex_unlock(&m_protocols);
    infof("registerd: 0x%04x", type);
    return 0;
}

static void *
net_background_thread(void *arg)
{
    struct net_device *dev;
    struct txq_entry *tx;
    struct net_protocol *proto;
    struct rxq_entry *rx;
    unsigned int count;

    debugf("running...");
    while (!net_interrupt) {
        count = 0;
        pthread_mutex_lock(&m_devices);
        for (dev = devices; dev; dev = dev->next) {
            if (dev->flags & NET_DEVICE_FLAG_UP) {
                pthread_mutex_lock(&dev->mutex);
                tx = (struct txq_entry *)queue_pop(&dev->queue);
                pthread_mutex_unlock(&dev->mutex);
                if (tx) {
                    dev->ops->transmit(dev, tx->type, tx->data, tx->len, tx->dst);
                    free(tx);
                    count++;
                }
                if (dev->ops->poll) {
                    if (dev->ops->poll(dev) != -1) {
                        count++;
                    }
                }
            }
        }
        pthread_mutex_unlock(&m_devices);
        pthread_mutex_lock(&m_protocols);
        for (proto = protocols; proto; proto = proto->next) {
            pthread_mutex_lock(&proto->mutex);
            rx = (struct rxq_entry *)queue_pop(&proto->queue);
            pthread_mutex_unlock(&proto->mutex);
            if (rx) {
                proto->handler(rx->dev, rx->data, rx->len);
                free(rx);
                count++;
            }
        }
        pthread_mutex_unlock(&m_protocols);
        if (!count) {
            usleep(1000);
        }
    }
    debugf("shutdown");
    return NULL;
}

void
net_shutdown(void)
{
    net_interrupt = 1;
    pthread_join(thread, NULL);
}

void
net_init(void)
{
    pthread_create(&thread, NULL, net_background_thread, NULL);
    debugf("initialized");
}
