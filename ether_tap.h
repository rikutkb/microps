#ifndef TAP_H
#define TAP_H

#include "net.h"

extern struct net_device *
ether_tap_init(const char *name);

#endif
