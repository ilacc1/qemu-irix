/*
 * QEMU block driver for MAME CHD (Compressed Hunks of Data) images
 *
 * Copyright (c) 2026
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * CHD is MAME's container format for disk images.  Every header field is
 * stored big-endian.  A drive's contents are divided into equally sized "hunks"
 * of hunk_bytes, and a map says where each hunk lives in the container.  A
 * hunk is stored uncompressed, ZLIB compressed, "mini" encoded (the map entry
 * holds eight bytes of data that repeat to fill the hunk), or is absent from
 * the container entirely, in which case it reads as zeroes.
 *
 * This implements header versions 1 through 5 with read and write access.  A
 * hunk being written is always stored uncompressed: in place when it already
 * occupies a whole hunk, otherwise at a freshly appended slot whose map entry
 * is updated in the container.
 *
 * Deliberately not implemented, and rejected at open time rather than
 * mis-decoded: the Huffman-coded map of a compressed v5 image, and
 * parent-backed hunks.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/module.h"

#include "block/block-io.h"
#include "block/block_int.h"
#include "block/thread-pool.h"
#include "qemu/coroutine.h"
#include "qapi/error.h"

#include <zlib.h>

enum {
    CHD_V1_HEADER_SIZE = 76,
    CHD_V2_HEADER_SIZE = 80,

    CHD_MAX_HEADER_SIZE = 124,   /* the v5 header is the largest */
};

enum {
    CHD_FLAG_HAS_PARENT   = 1 << 0,
    CHD_FLAG_IS_WRITEABLE = 1 << 1,
};

/* internal codec ids; these need not match MAME's own numbering */
enum {
    CHD_CODEC_NONE,
    CHD_CODEC_ZLIB,
    CHD_CODEC_MINI,
    CHD_CODEC_SELF,
};

/* v3/v4 map entry: byte 15 holds the type in its low nibble */
enum {
    CHD_V34_TYPE_MASK         = 0x0f,
    CHD_V34_TYPE_COMPRESSED   = 0x00,
    CHD_V34_TYPE_UNCOMPRESSED = 0x01,
    CHD_V34_TYPE_MINI         = 0x02,
    CHD_V34_TYPE_SELF_HUNK    = 0x03,
    CHD_V34_TYPE_PARENT_HUNK  = 0x04,
};

typedef struct BDRVChdState {
    int64_t total_bytes;        /* logical size presented to the guest */
    uint32_t version;
    uint32_t flags;
    uint32_t hunk_bytes;
    uint32_t unit_bytes;
    uint32_t hunk_count;
    uint32_t map_entry_bytes;

    /* map expanded into one record per hunk, indexed by hunk number */
    uint64_t *hunk_offset;      /* container offset, or source hunk for SELF */
    uint8_t  *hunk_codec;       /* CHD_CODEC_* */
    uint32_t *hunk_length;      /* bytes actually stored in the container */

    uint64_t map_offset;        /* needed when writing map entries back */
    uint64_t append_offset;     /* first byte not referenced by any hunk */

    /* staging buffers, allocated lazily and then reused */
    uint8_t *hunk_buf;
    uint32_t hunk_buf_size;
    uint8_t *comp_buf;
    uint32_t comp_buf_size;

    /* Serialises use of hunk_buf/comp_buf across coroutines: the file read
     * and the inflate can both yield. */
    CoMutex hunk_lock;

    /* Decompressed-hunk cache (ZLIB hunks). LRU by stamp; protected by
     * hunk_lock like the staging buffers. */
    struct ChdCacheSlot {
        uint32_t hunk;
        uint64_t stamp;
        bool valid;
        uint8_t *buf;
    } *cache;
    uint32_t cache_nslots;
    uint64_t cache_clock;
} BDRVChdState;

static uint64_t chd_be64(const uint8_t *p)
{
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8)  | (uint64_t)p[7];
}

static uint32_t chd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static uint16_t chd_be16(const uint8_t *p)
{
    return (uint16_t)(((uint32_t)p[0] << 8) | (uint32_t)p[1]);
}

static void chd_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static void chd_put_be64(uint8_t *p, uint64_t v)
{
    p[0] = v >> 56;
    p[1] = v >> 48;
    p[2] = v >> 40;
    p[3] = v >> 32;
    p[4] = v >> 24;
    p[5] = v >> 16;
    p[6] = v >> 8;
    p[7] = v;
}

static int chd_probe(const uint8_t *buf, int buf_size, const char *filename)
{
    const char *ext;

    if (buf_size < CHD_MAX_HEADER_SIZE || memcmp(buf, "MComprHD", 8)) {
        return 0;
    }

    ext = strrchr(filename, '.');
    if (ext && !strcasecmp(ext, ".chd")) {
        return 2;
    }
    return 1;
}

/*
 * Read the on-disk map and expand it into the per-hunk arrays.
 */
static int chd_load_map(BlockDriverState *bs, BDRVChdState *s,
                        uint64_t map_offset, const uint8_t *hdr,
                        Error **errp)
{
    int64_t file_size;
    uint64_t map_bytes;
    uint8_t *map = NULL;
    uint32_t i;
    int ret;

    /*
     * A compressed v5 image keeps its map as a Huffman-coded bit stream that
     * has to be unpacked before it can be indexed.  Refuse it rather than
     * produce garbage.
     */
    if (s->version == 5 && chd_be32(hdr + 16) != 0) {
        error_setg(errp, "chd: compressed v5 maps are not supported");
        return -ENOTSUP;
    }

    file_size = bdrv_getlength(bs->file->bs);
    if (file_size <= 0) {
        error_setg(errp, "chd: cannot size backing file");
        return -EIO;
    }

    s->map_entry_bytes = s->version <= 2 ? 8 : s->version <= 4 ? 16 : 4;
    map_bytes = (uint64_t)s->hunk_count * s->map_entry_bytes;

    if (map_offset > (uint64_t)file_size ||
        map_bytes > (uint64_t)file_size - map_offset) {
        error_setg(errp, "chd: map (%" PRIu64 " bytes at %" PRIu64
                   ") does not fit in the %" PRId64 "-byte file",
                   map_bytes, map_offset, file_size);
        return -EINVAL;
    }

    map = g_try_malloc(map_bytes);
    if (!map) {
        error_setg(errp, "chd: out of memory reading a %" PRIu64 "-byte map",
                   map_bytes);
        return -ENOMEM;
    }

    ret = bdrv_pread(bs->file, map_offset, map_bytes, map, 0);
    if (ret < 0) {
        error_setg(errp, "chd: cannot read map: %s", strerror(-ret));
        g_free(map);
        return ret;
    }

    for (i = 0; i < s->hunk_count; i++) {
        const uint8_t *e = map + (uint64_t)i * s->map_entry_bytes;

        if (s->version <= 2) {
            /*
             * V1/V2: one 64-bit big-endian word with a 44-bit container
             * offset and a 20-bit stored length.  A length equal to the whole
             * hunk size means the data is stored uncompressed.
             */
            uint64_t word = chd_be64(e);
            uint64_t length = word >> 44;
            uint64_t offset = word & ((1ULL << 44) - 1);

            s->hunk_offset[i] = offset;
            if (length == 0) {
                s->hunk_codec[i] = CHD_CODEC_NONE;
                s->hunk_length[i] = 0;
            } else if (length == s->hunk_bytes) {
                s->hunk_codec[i] = CHD_CODEC_NONE;
                s->hunk_length[i] = s->hunk_bytes;
            } else {
                s->hunk_codec[i] = CHD_CODEC_ZLIB;
                s->hunk_length[i] = length;
            }
        } else if (s->version <= 4) {
            uint32_t length = chd_be16(e + 12) | ((uint32_t)e[14] << 16);
            uint8_t type = e[15] & CHD_V34_TYPE_MASK;

            switch (type) {
            case CHD_V34_TYPE_COMPRESSED:
                s->hunk_offset[i] = chd_be64(e);
                s->hunk_codec[i] = CHD_CODEC_ZLIB;
                s->hunk_length[i] = length;
                break;
            case CHD_V34_TYPE_UNCOMPRESSED:
                s->hunk_offset[i] = chd_be64(e);
                s->hunk_codec[i] = CHD_CODEC_NONE;
                s->hunk_length[i] = s->hunk_bytes;
                break;
            case CHD_V34_TYPE_MINI:
                s->hunk_offset[i] = chd_be64(e);
                s->hunk_codec[i] = CHD_CODEC_MINI;
                s->hunk_length[i] = 0;
                break;
            case CHD_V34_TYPE_SELF_HUNK:
                s->hunk_offset[i] = chd_be64(e);
                s->hunk_codec[i] = CHD_CODEC_SELF;
                s->hunk_length[i] = 0;
                break;
            case CHD_V34_TYPE_PARENT_HUNK:
                error_setg(errp, "chd: parented images are not supported");
                ret = -ENOTSUP;
                goto out;
            default:
                error_setg(errp, "chd: unknown map entry type %u for hunk %u",
                           type, i);
                ret = -EINVAL;
                goto out;
            }
        } else {
            /*
             * V5 uncompressed map: a 32-bit container offset expressed in
             * hunk_bytes units.  Zero means the hunk has never been written,
             * and reads as zeroes.
             */
            uint32_t entry = chd_be32(e);

            s->hunk_offset[i] = (uint64_t)entry * s->hunk_bytes;
            s->hunk_codec[i] = CHD_CODEC_NONE;
            s->hunk_length[i] = entry ? s->hunk_bytes : 0;
        }
    }

    /*
     * Appended hunks go after everything the map already references, so we
     * never disturb existing data.
     */
    for (i = 0; i < s->hunk_count; i++) {
        if (s->hunk_length[i]) {
            uint64_t end = s->hunk_offset[i] + s->hunk_length[i];
            if (end > s->append_offset) {
                s->append_offset = end;
            }
        }
    }

    ret = 0;
out:
    g_free(map);
    return ret;
}

static void chd_cache_init(BDRVChdState *s);

static int chd_open(BlockDriverState *bs, QDict *options, int flags,
                    Error **errp)
{
    BDRVChdState *s = bs->opaque;

    qemu_co_mutex_init(&s->hunk_lock);
    uint8_t hdr[CHD_MAX_HEADER_SIZE];
    uint64_t logical_bytes = 0;
    uint64_t map_offset = 0;
    uint32_t hunksize = 0, totalhunks = 0, seclen = 512;
    int ret;

    ret = bdrv_open_file_child(NULL, options, "file", bs, errp);
    if (ret < 0) {
        return ret;
    }

    GRAPH_RDLOCK_GUARD_MAINLOOP();

    ret = bdrv_pread(bs->file, 0, CHD_MAX_HEADER_SIZE, hdr, 0);
    if (ret < 0) {
        error_setg(errp, "chd: cannot read header: %s", strerror(-ret));
        return ret;
    }
    if (memcmp(hdr, "MComprHD", 8)) {
        error_setg(errp, "chd: missing MComprHD tag");
        return -EINVAL;
    }

    s->version = chd_be32(hdr + 12);
    /*
     * Only v1 to v4 carry a flags word at this offset; for v5 it is
     * compressors[0], and the switch below overrides it.
     */
    s->flags = s->version <= 4 ? chd_be32(hdr + 16) : 0;

    switch (s->version) {
    case 1:
        hunksize = chd_be32(hdr + 24);
        totalhunks = chd_be32(hdr + 28);
        logical_bytes = (uint64_t)chd_be32(hdr + 32) * chd_be32(hdr + 36) *
                        chd_be32(hdr + 40) * 512;
        map_offset = CHD_V1_HEADER_SIZE;
        s->hunk_bytes = hunksize * 512;
        break;
    case 2:
        hunksize = chd_be32(hdr + 24);
        totalhunks = chd_be32(hdr + 28);
        seclen = chd_be32(hdr + 76);
        logical_bytes = (uint64_t)chd_be32(hdr + 32) * chd_be32(hdr + 36) *
                        chd_be32(hdr + 40) * seclen;
        map_offset = CHD_V2_HEADER_SIZE;
        s->hunk_bytes = hunksize * seclen;
        break;
    case 3:
        totalhunks = chd_be32(hdr + 24);
        logical_bytes = chd_be64(hdr + 28);
        s->hunk_bytes = chd_be32(hdr + 76);
        break;
    case 4:
        totalhunks = chd_be32(hdr + 24);
        logical_bytes = chd_be64(hdr + 28);
        s->hunk_bytes = chd_be32(hdr + 44);
        break;
    case 5:
        /*
         * V5 dropped the flags word: byte 16 is compressors[0].  There is no
         * parent support here, so such an image is always writable.
         */
        logical_bytes = chd_be64(hdr + 32);
        map_offset = chd_be64(hdr + 40);
        s->hunk_bytes = chd_be32(hdr + 56);
        s->unit_bytes = chd_be32(hdr + 60);
        s->flags = CHD_FLAG_IS_WRITEABLE;
        break;
    default:
        error_setg(errp, "chd: unsupported header version %u", s->version);
        return -EINVAL;
    }

    if (s->flags & CHD_FLAG_HAS_PARENT) {
        error_setg(errp, "chd: parented images are not supported");
        return -ENOTSUP;
    }
    if (s->version == 5 && s->unit_bytes == 0) {
        error_setg(errp, "chd: v5 image has a zero unit size");
        return -EINVAL;
    }
    if (s->hunk_bytes == 0 || s->hunk_bytes > (512 * 1024)) {
        error_setg(errp, "chd: implausible hunk size %u", s->hunk_bytes);
        return -EINVAL;
    }

    if (totalhunks == 0 && logical_bytes) {
        totalhunks = DIV_ROUND_UP(logical_bytes, s->hunk_bytes);
    }
    if (totalhunks == 0 || logical_bytes == 0) {
        error_setg(errp, "chd: image describes no data");
        return -EINVAL;
    }

    s->hunk_count = totalhunks;
    s->total_bytes = logical_bytes;
    s->map_offset = map_offset;

    s->hunk_offset = g_try_new0(uint64_t, s->hunk_count);
    s->hunk_codec = g_try_new0(uint8_t, s->hunk_count);
    s->hunk_length = g_try_new0(uint32_t, s->hunk_count);
    if (!s->hunk_offset || !s->hunk_codec || !s->hunk_length) {
        error_setg(errp, "chd: out of memory for %u hunk entries",
                   s->hunk_count);
        ret = -ENOMEM;
        goto fail;
    }

    ret = chd_load_map(bs, s, map_offset, hdr, errp);
    if (ret < 0) {
        goto fail;
    }

    if ((flags & BDRV_O_RDWR) && !(s->flags & CHD_FLAG_IS_WRITEABLE)) {
        error_setg(errp, "chd: image is not marked writeable");
        ret = -EINVAL;
        goto fail;
    }

    chd_cache_init(s);
    return 0;

fail:
    g_clear_pointer(&s->hunk_offset, g_free);
    g_clear_pointer(&s->hunk_codec, g_free);
    g_clear_pointer(&s->hunk_length, g_free);
    return ret;
}

/*
 * SGI_CHD_SYNC=1 inflates hunks inline in the requesting coroutine instead
 * of handing them to the block thread pool.
 */
static bool sgi_chd_sync_enabled(void)
{
    static int cached = -1;

    if (cached < 0) {
        const char *e = getenv("SGI_CHD_SYNC");

        cached = (e && *e && strtoul(e, NULL, 0) != 0) ? 1 : 0;
    }
    return cached != 0;
}

/*
 * Decompressed-hunk cache. Size from SGI_CHD_CACHE_MB (default 256). Without
 * it, every read of a compressed hunk decompresses the whole hunk again, even
 * when the previous request just did so. Set SGI_CHD_CACHE_MB=0 to disable
 * the cache entirely.
 */
static uint32_t chd_cache_bytes(void)
{
    const char *e = getenv("SGI_CHD_CACHE_MB");
    unsigned long mb = e ? strtoul(e, NULL, 0) : 256;

    return (uint32_t)MIN(mb, 4096UL) * 1024 * 1024;
}

static void chd_cache_init(BDRVChdState *s)
{
    uint64_t bytes = chd_cache_bytes();

    if (!s->hunk_bytes || bytes < s->hunk_bytes) {
        return;
    }
    s->cache_nslots = bytes / s->hunk_bytes;
    s->cache = g_new0(struct ChdCacheSlot, s->cache_nslots);
}

static struct ChdCacheSlot *chd_cache_find(BDRVChdState *s, uint32_t hunk)
{
    uint32_t i;

    for (i = 0; i < s->cache_nslots; i++) {
        if (s->cache[i].valid && s->cache[i].hunk == hunk) {
            s->cache[i].stamp = ++s->cache_clock;
            return &s->cache[i];
        }
    }
    return NULL;
}

static void chd_cache_insert(BDRVChdState *s, uint32_t hunk,
                             const uint8_t *data)
{
    struct ChdCacheSlot *victim = NULL;
    uint32_t i;

    for (i = 0; i < s->cache_nslots; i++) {
        if (!s->cache[i].valid) {
            victim = &s->cache[i];
            break;
        }
        if (!victim || s->cache[i].stamp < victim->stamp) {
            victim = &s->cache[i];
        }
    }
    if (!victim) {
        return;
    }
    if (!victim->buf) {
        victim->buf = g_malloc(s->hunk_bytes);
    }
    memcpy(victim->buf, data, s->hunk_bytes);
    victim->hunk = hunk;
    victim->valid = true;
    victim->stamp = ++s->cache_clock;
}

static void chd_cache_invalidate(BDRVChdState *s, uint32_t first,
                                 uint32_t last)
{
    uint32_t i;

    for (i = 0; i < s->cache_nslots; i++) {
        if (s->cache[i].valid && s->cache[i].hunk >= first &&
            s->cache[i].hunk <= last) {
            s->cache[i].valid = false;
        }
    }
}

static void chd_close(BlockDriverState *bs)
{
    BDRVChdState *s = bs->opaque;
    uint32_t i;

    if (s->cache) {
        for (i = 0; i < s->cache_nslots; i++) {
            g_free(s->cache[i].buf);
        }
        g_free(s->cache);
    }
    g_free(s->hunk_offset);
    g_free(s->hunk_codec);
    g_free(s->hunk_length);
    g_free(s->hunk_buf);
    g_free(s->comp_buf);
}

static int chd_ensure_hunk_buf(BlockDriverState *bs, BDRVChdState *s)
{
    if (s->hunk_buf_size < s->hunk_bytes) {
        g_free(s->hunk_buf);
        s->hunk_buf = qemu_try_blockalign(bs, s->hunk_bytes);
        if (!s->hunk_buf) {
            s->hunk_buf_size = 0;
            return -ENOMEM;
        }
        s->hunk_buf_size = s->hunk_bytes;
    }
    return 0;
}

static int chd_ensure_comp_buf(BlockDriverState *bs, BDRVChdState *s,
                               uint32_t need)
{
    if (s->comp_buf_size < need) {
        g_free(s->comp_buf);
        s->comp_buf = qemu_try_blockalign(bs, need);
        if (!s->comp_buf) {
            s->comp_buf_size = 0;
            return -ENOMEM;
        }
        s->comp_buf_size = need;
    }
    return 0;
}

/*
 * Fill the whole of s->hunk_buf with the contents of hunk @hunk.  SELF entries
 * name another hunk rather than container data; follow them with a small depth
 * limit so a malformed image cannot send us round in circles.
 */
/* Runs in the thread pool: no BQL, touches only the buffers passed in. */
typedef struct ChdInflateJob {
    uint8_t *dst;
    uLongf dst_len;
    const uint8_t *src;
    uLong src_len;
    int zr;
} ChdInflateJob;

static int chd_inflate_job(void *opaque)
{
    ChdInflateJob *job = opaque;

    job->zr = uncompress(job->dst, &job->dst_len, job->src, job->src_len);
    return job->zr;
}

static int coroutine_fn GRAPH_RDLOCK
chd_read_hunk(BlockDriverState *bs, BDRVChdState *s, uint32_t hunk,
                          uint32_t depth)
{
    uint32_t complen = s->hunk_length[hunk];
    int ret;

    switch (s->hunk_codec[hunk]) {
    case CHD_CODEC_NONE:
        if (complen == s->hunk_bytes) {
            return bdrv_co_pread(bs->file, s->hunk_offset[hunk],
                                 s->hunk_bytes, s->hunk_buf, 0);
        }
        /* absent from the container: the hunk reads as zeroes */
        memset(s->hunk_buf, 0, s->hunk_bytes);
        return 0;

    case CHD_CODEC_MINI:
        /*
         * The map entry's own offset field supplies eight bytes of data which
         * then repeat to fill the hunk.
         */
        {
            uint32_t i;

            chd_put_be64(s->hunk_buf, s->hunk_offset[hunk]);
            for (i = 8; i < s->hunk_bytes; i++) {
                s->hunk_buf[i] = s->hunk_buf[i - 8];
            }
        }
        return 0;

    case CHD_CODEC_SELF:
        if (depth >= 8 || s->hunk_offset[hunk] >= s->hunk_count) {
            error_report("chd: hunk %u self-references %" PRIu64, hunk,
                         s->hunk_offset[hunk]);
            return -EINVAL;
        }
        return chd_read_hunk(bs, s, s->hunk_offset[hunk], depth + 1);

    default:
        if (complen == 0) {
            return -EINVAL;
        }
        break;
    }

    /* CHD_CODEC_ZLIB */
    if (s->cache) {
        struct ChdCacheSlot *hit = chd_cache_find(s, hunk);

        if (hit) {
            memcpy(s->hunk_buf, hit->buf, s->hunk_bytes);
            return 0;
        }
    }
    ret = chd_ensure_comp_buf(bs, s, complen);
    if (ret < 0) {
        return ret;
    }
    ret = bdrv_co_pread(bs->file, s->hunk_offset[hunk], complen, s->comp_buf,
                        0);
    if (ret < 0) {
        return ret;
    }

    {
        ChdInflateJob job = {
            .dst = s->hunk_buf,
            .dst_len = s->hunk_bytes,
            .src = s->comp_buf,
            .src_len = complen,
        };
        /* Inflate off the main loop; this coroutine sleeps until it is done.
         * SGI_CHD_SYNC=1 inflates inline instead, for A/B comparison. */
        if (sgi_chd_sync_enabled()) {
            chd_inflate_job(&job);
        } else {
            thread_pool_submit_co(chd_inflate_job, &job);
        }

        if (job.zr != Z_OK || job.dst_len != s->hunk_bytes) {
            error_report("chd: zlib error %d expanding hunk %u", job.zr, hunk);
            return -EIO;
        }
    }
    if (s->cache) {
        chd_cache_insert(s, hunk, s->hunk_buf);
    }
    return 0;
}

static int coroutine_fn GRAPH_RDLOCK
chd_co_preadv_body(BlockDriverState *bs, int64_t offset, int64_t bytes,
                   QEMUIOVector *qiov, BdrvRequestFlags flags);
static int coroutine_fn GRAPH_RDLOCK
chd_co_pwritev_body(BlockDriverState *bs, int64_t offset, int64_t bytes,
                    QEMUIOVector *qiov, BdrvRequestFlags flags);

/*
 * One request at a time: the hunk buffers and the decompressed-hunk cache are
 * shared state, so each request holds hunk_lock for its whole duration.
 */
static int coroutine_fn GRAPH_RDLOCK
chd_co_preadv(BlockDriverState *bs, int64_t offset, int64_t bytes,
              QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    BDRVChdState *s = bs->opaque;
    int ret;

    qemu_co_mutex_lock(&s->hunk_lock);
    ret = chd_co_preadv_body(bs, offset, bytes, qiov, flags);
    qemu_co_mutex_unlock(&s->hunk_lock);
    return ret;
}

static int coroutine_fn GRAPH_RDLOCK
chd_co_pwritev(BlockDriverState *bs, int64_t offset, int64_t bytes,
               QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    BDRVChdState *s = bs->opaque;
    int ret;

    qemu_co_mutex_lock(&s->hunk_lock);
    if (s->cache && bytes > 0) {
        chd_cache_invalidate(s, offset / s->hunk_bytes,
                             (offset + bytes - 1) / s->hunk_bytes);
    }
    ret = chd_co_pwritev_body(bs, offset, bytes, qiov, flags);
    if (s->cache && bytes > 0) {
        chd_cache_invalidate(s, offset / s->hunk_bytes,
                             (offset + bytes - 1) / s->hunk_bytes);
    }
    qemu_co_mutex_unlock(&s->hunk_lock);
    return ret;
}

static int coroutine_fn GRAPH_RDLOCK
chd_co_preadv_body(BlockDriverState *bs, int64_t offset, int64_t bytes,
                   QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    BDRVChdState *s = bs->opaque;
    int64_t cur = offset;
    int64_t left = bytes;
    size_t done = 0;

    while (left > 0) {
        uint32_t hunk = cur / s->hunk_bytes;
        uint32_t in_hunk = cur % s->hunk_bytes;
        uint32_t n = MIN(left, (int64_t)(s->hunk_bytes - in_hunk));
        int ret;

        if (hunk >= s->hunk_count) {
            return -EINVAL;
        }

        if (s->hunk_codec[hunk] == CHD_CODEC_NONE &&
            s->hunk_length[hunk] == 0) {
            /* absent from the container: zeroes, no I/O needed */
            qemu_iovec_memset(qiov, done, 0, n);
        } else if (s->hunk_codec[hunk] == CHD_CODEC_NONE) {
            ret = bdrv_co_preadv_part(bs->file, s->hunk_offset[hunk] + in_hunk,
                                      n, qiov, done, flags);
            if (ret < 0) {
                return ret;
            }
        } else {
            ret = chd_ensure_hunk_buf(bs, s);
            if (ret < 0) {
                return ret;
            }
            ret = chd_read_hunk(bs, s, hunk, 0);
            if (ret < 0) {
                return ret;
            }
            qemu_iovec_from_buf(qiov, done, s->hunk_buf + in_hunk, n);
        }

        done += n;
        cur += n;
        left -= n;
    }

    return 0;
}

/* Point hunk @hunk's map entry at an uncompressed hunk stored at @offset. */
static int coroutine_fn GRAPH_RDLOCK
chd_publish_hunk(BlockDriverState *bs, BDRVChdState *s, uint32_t hunk,
                 uint64_t offset)
{
    uint8_t entry[16];
    int ret;

    memset(entry, 0, sizeof(entry));

    if (s->version <= 2) {
        chd_put_be64(entry, ((uint64_t)s->hunk_bytes << 44) | offset);
    } else if (s->version <= 4) {
        chd_put_be64(entry, offset);
        /* the crc32 field is only checked for compressed hunks */
        entry[15] = CHD_V34_TYPE_UNCOMPRESSED;
    } else {
        chd_put_be32(entry, offset / s->hunk_bytes);
    }

    ret = bdrv_co_pwrite(bs->file,
                         s->map_offset + (uint64_t)hunk * s->map_entry_bytes,
                         s->map_entry_bytes, entry, 0);
    if (ret < 0) {
        return ret;
    }

    s->hunk_offset[hunk] = offset;
    s->hunk_codec[hunk] = CHD_CODEC_NONE;
    s->hunk_length[hunk] = s->hunk_bytes;
    return 0;
}

static int coroutine_fn GRAPH_RDLOCK
chd_co_pwritev_body(BlockDriverState *bs, int64_t offset, int64_t bytes,
                    QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    BDRVChdState *s = bs->opaque;
    int64_t cur = offset;
    int64_t left = bytes;
    size_t done = 0;

    while (left > 0) {
        uint32_t hunk = cur / s->hunk_bytes;
        uint32_t in_hunk = cur % s->hunk_bytes;
        uint32_t n = MIN(left, (int64_t)(s->hunk_bytes - in_hunk));
        bool in_place;
        int ret;

        if (hunk >= s->hunk_count) {
            return -EINVAL;
        }

        in_place = s->hunk_codec[hunk] == CHD_CODEC_NONE &&
                   s->hunk_length[hunk] == s->hunk_bytes;

        if (!in_place || in_hunk != 0 || n != s->hunk_bytes) {
            /*
             * The write does not cover the whole hunk, so start from what the
             * hunk currently holds.
             */
            ret = chd_ensure_hunk_buf(bs, s);
            if (ret < 0) {
                return ret;
            }
            ret = chd_read_hunk(bs, s, hunk, 0);
            if (ret < 0) {
                return ret;
            }
            qemu_iovec_to_buf(qiov, done, s->hunk_buf + in_hunk, n);

            if (in_place) {
                ret = bdrv_co_pwrite(bs->file, s->hunk_offset[hunk] + in_hunk,
                                     n, s->hunk_buf + in_hunk, 0);
            } else {
                uint64_t at = s->append_offset;

                ret = bdrv_co_pwrite(bs->file, at, s->hunk_bytes, s->hunk_buf,
                                     0);
                if (ret == 0) {
                    ret = chd_publish_hunk(bs, s, hunk, at);
                }
                if (ret == 0) {
                    s->append_offset = at + s->hunk_bytes;
                }
            }
            if (ret < 0) {
                error_report("chd: write to hunk %u failed: %s", hunk,
                             strerror(-ret));
                return ret;
            }
        } else {
            /* the request covers the hunk exactly and it is already stored */
            uint8_t *tmp = g_try_malloc(s->hunk_bytes);

            if (!tmp) {
                return -ENOMEM;
            }
            qemu_iovec_to_buf(qiov, done, tmp, n);
            ret = bdrv_co_pwrite(bs->file, s->hunk_offset[hunk], n, tmp, 0);
            g_free(tmp);
            if (ret < 0) {
                return ret;
            }
        }

        done += n;
        cur += n;
        left -= n;
    }

    return 0;
}

static int coroutine_fn GRAPH_RDLOCK
chd_co_flush_to_os(BlockDriverState *bs)
{
    /* Writes land in the container file immediately. */
    return 0;
}

static int coroutine_fn GRAPH_RDLOCK
chd_co_flush_to_disk(BlockDriverState *bs)
{
    return bdrv_co_flush(bs->file->bs);
}

static void coroutine_fn GRAPH_RDLOCK
chd_co_invalidate_cache(BlockDriverState *bs, Error **errp)
{
}

static int64_t coroutine_fn GRAPH_RDLOCK chd_getlength(BlockDriverState *bs)
{
    BDRVChdState *s = bs->opaque;

    return s->total_bytes;
}

static BlockDriver bdrv_chd = {
    .format_name              = "chd",
    .instance_size            = sizeof(BDRVChdState),
    .bdrv_probe               = chd_probe,
    .bdrv_open                = chd_open,
    .bdrv_close               = chd_close,
    .bdrv_child_perm          = bdrv_default_perms,
    .bdrv_co_preadv           = chd_co_preadv,
    .bdrv_co_pwritev          = chd_co_pwritev,
    .bdrv_co_flush_to_os      = chd_co_flush_to_os,
    .bdrv_co_flush_to_disk    = chd_co_flush_to_disk,
    .bdrv_co_invalidate_cache = chd_co_invalidate_cache,
    .bdrv_co_getlength        = chd_getlength,
    .is_format                = true,
};

static void bdrv_chd_init(void)
{
    bdrv_register(&bdrv_chd);
}

block_init(bdrv_chd_init);
