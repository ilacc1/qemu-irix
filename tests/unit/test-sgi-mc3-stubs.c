#include "qemu/osdep.h"
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

void memory_region_set_size(MemoryRegion *mr, uint64_t size)
{
    mr->size = int128_make64(size);
}

void memory_region_init_alias(MemoryRegion *mr, Object *owner,
                              const char *name, MemoryRegion *orig,
                              hwaddr offset, uint64_t size)
{
    memset(mr, 0, sizeof(*mr));
    mr->alias = orig;
    mr->alias_offset = offset;
    mr->name = name;
    mr->size = int128_make64(size);
    mr->enabled = true;
}

void memory_region_add_subregion_overlap(MemoryRegion *mr, hwaddr offset,
                                         MemoryRegion *subregion,
                                         int priority)
{
}

void memory_region_set_enabled(MemoryRegion *mr, bool enabled)
{
    mr->enabled = enabled;
}

uint64_t memory_region_size(const MemoryRegion *mr)
{
    return int128_get64(mr->size);
}

void memory_region_transaction_begin(void)
{
}

void memory_region_transaction_commit(void)
{
}

void address_space_init(AddressSpace *as, MemoryRegion *root, const char *name)
{
    memset(as, 0, sizeof(*as));
    as->root = root;
}

void address_space_destroy(AddressSpace *as)
{
}

MemTxResult address_space_read_full(const AddressSpace *as, hwaddr addr,
                                    MemTxAttrs attrs, void *buf, hwaddr len)
{
    MemoryRegion *root = as->root;

    if (!root || !root->opaque || addr + len > memory_region_size(root)) {
        memset(buf, 0xff, len);
        return MEMTX_OK;
    }
    memcpy(buf, (uint8_t *)root->opaque + addr, len);
    return MEMTX_OK;
}

MemTxResult address_space_write(const AddressSpace *as, hwaddr addr,
                                MemTxAttrs attrs, const void *buf,
                                hwaddr len)
{
    MemoryRegion *root = as->root;

    if (!root || !root->opaque || addr + len > memory_region_size(root)) {
        return MEMTX_OK;
    }
    memcpy((uint8_t *)root->opaque + addr, buf, len);
    return MEMTX_OK;
}

