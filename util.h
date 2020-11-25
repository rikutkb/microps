#ifndef UTIL_H
#define UTIL_H

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>

#ifndef MAX
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#endif
#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif

#define sizeof_member(s, m) sizeof(((s *)NULL)->m)
#define array_tailof(x) (x + (sizeof(x) / sizeof(*x)))
#define array_offset(x, y) (((uintptr_t)y - (uintptr_t)x) / sizeof(*y))

#define errorf(...) lprintf('E', __FILE__, __LINE__, __func__, __VA_ARGS__)
#define warnf(...) lprintf('W', __FILE__, __LINE__, __func__, __VA_ARGS__)
#define infof(...) lprintf('I', __FILE__, __LINE__, __func__, __VA_ARGS__)
#define debugf(...) lprintf('D', __FILE__, __LINE__, __func__, __VA_ARGS__)

#define debugdump(...) hexdump(stderr, __VA_ARGS__)

extern int
lprintf(int level, const char *file, int line, const char *func, const char *fmt, ...);
extern void
hexdump(FILE *fp, const void *data, size_t size);

extern uint16_t
hton16(uint16_t h);
extern uint16_t
ntoh16(uint16_t n);
extern uint32_t
hton32(uint32_t h);
extern uint32_t
ntoh32(uint32_t n);

struct queue_entry {
    struct queue_entry *next;
};

struct queue_head {
    struct queue_entry *next;
    struct queue_entry *tail;
    unsigned int num;
};

extern void
queue_init(struct queue_head *queue);
extern struct queue_entry *
queue_push(struct queue_head *queue, struct queue_entry *entry);
extern struct queue_entry *
queue_pop(struct queue_head *queue);

#endif