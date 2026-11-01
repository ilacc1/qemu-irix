#include "qemu/osdep.h"
#include "chardev/char-fe.h"
#include "system/memory.h"

void memory_region_init_io(MemoryRegion *mr, Object *owner,
                           const MemoryRegionOps *ops, void *opaque,
                           const char *name, uint64_t size)
{
    memset(mr, 0, sizeof(*mr));
    mr->ops = ops;
    mr->opaque = opaque;
    mr->name = name;
    mr->size = int128_make64(size);
}

bool qemu_chr_fe_init(CharFrontend *c, Chardev *s, Error **errp)
{
    c->chr = s;
    return true;
}

void qemu_chr_fe_deinit(CharFrontend *c, bool del)
{
    c->chr = NULL;
}

void qemu_chr_fe_set_handlers(CharFrontend *c,
                              IOCanReadHandler *fd_can_read,
                              IOReadHandler *fd_read,
                              IOEventHandler *fd_event,
                              BackendChangeHandler *be_change,
                              void *opaque,
                              GMainContext *context,
                              bool set_open)
{
}

int qemu_chr_fe_write_all(CharFrontend *c, const uint8_t *buf, int len)
{
    return len;
}

void qemu_chr_fe_accept_input(CharFrontend *c)
{
}
