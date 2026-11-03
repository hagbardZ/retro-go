/*
 * Amiga Forever's RP9 packages: a ZIP file with rp9-manifest.xml (the
 * model, Kickstart, memory, and the disks in their order: <floppy
 * priority="1">game1.adf</floppy> ...) beside the disk images.
 *
 * Read from the file as needed: the central directory for the members,
 * stored or deflated ones (the board's ROM inflater, zlib on the host).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "amiga_internal.h"
#include "rp9.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "miniz.h"
#else
#include <zlib.h>
#endif

#define MAX_DIR         (64 * 1024)

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

typedef struct {
    uint32_t local;                 /* the local header's offset */
    uint32_t packed, size;
    uint16_t method;
} member_t;

/* Find a member by name (case not minded) */
static bool find(FILE *f, const char *name, member_t *m)
{
    uint8_t tail[22 + 256];
    if (fseek(f, 0, SEEK_END) != 0) {
        return false;
    }
    long len = ftell(f);
    long back = len < (long)sizeof(tail) ? len : (long)sizeof(tail);
    fseek(f, len - back, SEEK_SET);
    if (fread(tail, 1, back, f) != (size_t)back) {
        return false;
    }
    long eocd = -1;
    for (long i = back - 22; i >= 0; i--) {
        if (le32(tail + i) == 0x06054B50) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        return false;
    }
    uint32_t dir_size = le32(tail + eocd + 12), dir_at = le32(tail + eocd + 16);
    if (dir_size > MAX_DIR) {
        return false;
    }
    uint8_t *dir = malloc(dir_size);
    if (!dir) {
        return false;
    }
    bool found = false;
    fseek(f, dir_at, SEEK_SET);
    if (fread(dir, 1, dir_size, f) == dir_size) {
        size_t nlen = strlen(name);
        for (uint32_t p = 0; p + 46 <= dir_size && le32(dir + p) == 0x02014B50;) {
            uint16_t n = le16(dir + p + 28), extra = le16(dir + p + 30), comment = le16(dir + p + 32);
            if (n == nlen && p + 46 + n <= dir_size && strncasecmp((const char *)dir + p + 46, name, n) == 0) {
                m->method = le16(dir + p + 10);
                m->packed = le32(dir + p + 20);
                m->size = le32(dir + p + 24);
                m->local = le32(dir + p + 42);
                found = true;
                break;
            }
            p += 46 + n + extra + comment;
        }
    }
    free(dir);
    return found;
}

static bool inflate_raw(const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t out_len)
{
#ifdef ESP_PLATFORM
    /* its ~10 KB of state on the heap: inserts run on small stacks (the web server's) */
    tinfl_decompressor *d = malloc(sizeof(tinfl_decompressor));
    if (!d) {
        return false;
    }
    tinfl_init(d);
    size_t in_bytes = in_len, out_bytes = out_len;
    tinfl_status st = tinfl_decompress(d, in, &in_bytes, out, out, &out_bytes, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(d);
    return st == TINFL_STATUS_DONE && out_bytes == out_len;
#else
    z_stream z = { 0 };
    if (inflateInit2(&z, -15) != Z_OK) {
        return false;
    }
    z.next_in = (Bytef *)in;
    z.avail_in = in_len;
    z.next_out = out;
    z.avail_out = out_len;
    int r = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    return r == Z_STREAM_END && z.total_out == out_len;
#endif
}

/* A member's contents into dst (max bytes); its size in *size */
bool rp9_member(const char *path, const char *name, uint8_t *dst, uint32_t max, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    member_t m;
    bool ok = false;
    uint8_t local[30];
    if (find(f, name, &m) && m.size <= max && fseek(f, m.local, SEEK_SET) == 0 &&
        fread(local, 1, 30, f) == 30 && le32(local) == 0x04034B50) {
        fseek(f, m.local + 30 + le16(local + 26) + le16(local + 28), SEEK_SET);
        if (m.method == 0) {
            ok = m.size == m.packed && fread(dst, 1, m.size, f) == m.size;
        } else if (m.method == 8) {
            uint8_t *packed = ami_malloc(m.packed ? m.packed : 1);
            if (packed) {
                ok = fread(packed, 1, m.packed, f) == m.packed && inflate_raw(packed, m.packed, dst, m.size);
                free(packed);
            }
        }
        *size = m.size;
    }
    fclose(f);
    return ok;
}

#define CHUNK           (16 * 1024)
#define OUT_CHUNK       (64 * 1024)

#ifdef ESP_PLATFORM
/* Straight to the file system, past stdio's buffer */
static bool write_all(FILE *f, const uint8_t *p, size_t n)
{
    fflush(f);
    while (n > 0) {
        ssize_t w = write(fileno(f), p, n);
        if (w <= 0) {
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}
#endif

/* Unpack len bytes of deflated data from f into out (streaming) */
static bool inflate_file(FILE *f, uint32_t packed, uint32_t size, FILE *out)
{
    uint8_t *in = malloc(CHUNK);
    uint32_t total = 0;
    bool ok = false;
#ifdef ESP_PLATFORM
    /* The output gathered into a buffer aligned to the cache line and
     * written whole past stdio: the SD card then gets runs of sectors
     * (written from the window as it filled: 0.35 MB/s, 20 s for 7 MB). A
     * yield now and then: it runs on the web server's task, which the
     * watchdog watches.
     */
    tinfl_decompressor *d = malloc(sizeof(tinfl_decompressor));
    uint8_t *dict = malloc(TINFL_LZ_DICT_SIZE);
    uint8_t *outbuf = heap_caps_aligned_alloc(128, OUT_CHUNK, MALLOC_CAP_SPIRAM);
    size_t outlen = 0;
    if (in && d && dict && outbuf) {
        tinfl_init(d);
        size_t avail = 0, pos = 0, ofs = 0;
        tinfl_status st;
        for (;;) {
            if (avail == 0 && packed) {
                avail = fread(in, 1, packed < CHUNK ? packed : CHUNK, f);
                if (avail == 0) {
                    break;
                }
                packed -= (uint32_t)avail;
                pos = 0;
            }
            size_t in_bytes = avail, out_bytes = TINFL_LZ_DICT_SIZE - ofs;
            st = tinfl_decompress(d, in + pos, &in_bytes, dict, dict + ofs, &out_bytes,
                                  packed ? TINFL_FLAG_HAS_MORE_INPUT : 0);
            pos += in_bytes;
            avail -= in_bytes;
            const uint8_t *src = dict + ofs;
            size_t left = out_bytes;
            bool failed = false;
            while (left) {
                size_t n = OUT_CHUNK - outlen < left ? OUT_CHUNK - outlen : left;
                memcpy(outbuf + outlen, src, n);
                outlen += n;
                src += n;
                left -= n;
                if (outlen == OUT_CHUNK) {
                    failed = !write_all(out, outbuf, outlen);
                    outlen = 0;
                    vTaskDelay(1);
                }
            }
            if (failed) {
                break;
            }
            total += (uint32_t)out_bytes;
            ofs = (ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
            if (st <= TINFL_STATUS_DONE) {
                ok = st == TINFL_STATUS_DONE && write_all(out, outbuf, outlen);
                break;
            }
        }
    }
    heap_caps_free(outbuf);
    free(dict);
    free(d);
#else
    uint8_t *buf = malloc(CHUNK);
    z_stream z = { 0 };
    if (in && buf && inflateInit2(&z, -15) == Z_OK) {
        int r = Z_OK;
        while (r != Z_STREAM_END) {
            if (z.avail_in == 0 && packed) {
                z.avail_in = (uInt)fread(in, 1, packed < CHUNK ? packed : CHUNK, f);
                packed -= z.avail_in;
                z.next_in = in;
            }
            z.next_out = buf;
            z.avail_out = CHUNK;
            r = inflate(&z, Z_NO_FLUSH);
            if (r != Z_OK && r != Z_STREAM_END) {
                break;
            }
            size_t n = CHUNK - z.avail_out;
            if (n && fwrite(buf, 1, n, out) != n) {
                break;
            }
            total += (uint32_t)n;
            if (n == 0 && z.avail_in == 0 && !packed) {
                break;
            }
        }
        ok = r == Z_STREAM_END;
        inflateEnd(&z);
    }
    free(buf);
#endif
    free(in);
    return ok && total == size;
}

bool rp9_extract(const char *path, const char *name, const char *out_path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    member_t m;
    bool ok = false;
    uint8_t local[30];
    if (find(f, name, &m) && fseek(f, m.local, SEEK_SET) == 0 && fread(local, 1, 30, f) == 30 &&
        le32(local) == 0x04034B50) {
        fseek(f, m.local + 30 + le16(local + 26) + le16(local + 28), SEEK_SET);
        FILE *out = fopen(out_path, "wb");
        if (out) {
            if (m.method == 0) {
                uint8_t *buf = malloc(CHUNK);
                uint32_t left = m.size;
                ok = buf != NULL && m.packed == m.size;
                while (ok && left) {
                    size_t n = left < CHUNK ? left : CHUNK;
                    ok = fread(buf, 1, n, f) == n && fwrite(buf, 1, n, out) == n;
                    left -= (uint32_t)n;
                }
                free(buf);
            } else if (m.method == 8) {
                ok = inflate_file(f, m.packed, m.size, out);
            }
            ok = fclose(out) == 0 && ok;
            if (!ok) {
                remove(out_path);
            }
        }
    }
    fclose(f);
    return ok;
}

/* The text of an element (<tag ...>text</tag>) after *from; false at the end */
static bool element(const char *xml, const char *tag, const char **from, char *text, size_t len, int *priority)
{
    char open[32];
    snprintf(open, sizeof(open), "<%s", tag);
    const char *p = *from;
    while ((p = strstr(p, open)) != NULL) {
        char c = p[strlen(open)];
        if (c == '>' || c == ' ') {
            break;
        }
        p++;
    }
    if (!p) {
        return false;
    }
    const char *end = strchr(p, '>');
    if (!end) {
        return false;
    }
    if (priority) {
        const char *pr = strstr(p, "priority=\"");
        *priority = pr && pr < end ? atoi(pr + 10) : 0;
    }
    const char *close = strstr(end, "</");
    if (!close) {
        return false;
    }
    size_t n = (size_t)(close - end - 1);
    n = n < len - 1 ? n : len - 1;
    memcpy(text, end + 1, n);
    text[n] = 0;
    *from = close;
    return true;
}

bool rp9_read(const char *path, rp9_t *r)
{
    memset(r, 0, sizeof(*r));
    char *xml = malloc(32 * 1024);
    if (!xml) {
        return false;
    }
    uint32_t size = 0;
    if (!rp9_member(path, "rp9-manifest.xml", (uint8_t *)xml, 32 * 1024 - 1, &size)) {
        free(xml);
        return false;
    }
    xml[size] = 0;
    const char *p = xml;
    element(xml, "title", &p, r->title, sizeof(r->title), NULL);
    p = xml;
    element(xml, "system", &p, r->system, sizeof(r->system), NULL);
    char rom[16] = "";
    p = xml;
    if (element(xml, "systemrom", &p, rom, sizeof(rom), NULL)) {
        r->rom = atoi(rom);
    } else {
        /* (or <rom type="system">130</rom>: Ants) */
        const char *s = strstr(xml, "<rom type=\"system\">");
        r->rom = s ? atoi(s + strlen("<rom type=\"system\">")) : 0;
    }
    /* <ram type="chip|fast|z3">bytes</ram> */
    for (const char *s = xml; (s = strstr(s, "<ram type=\"")) != NULL; s++) {
        const char *type = s + strlen("<ram type=\"");
        const char *end = strchr(type, '>');
        uint32_t bytes = end ? (uint32_t)strtoul(end + 1, NULL, 10) : 0;
        if (strncmp(type, "chip\"", 5) == 0) {
            r->chip_ram = bytes;
        } else if (strncmp(type, "fast\"", 5) == 0 || strncmp(type, "z3\"", 3) == 0) {
            r->fast_ram += bytes;
        }
    }
    /* the floppies by priority */
    int prio[RP9_DISKS];
    p = xml;
    char name[RP9_NAME];
    int pr;
    while (r->disks < RP9_DISKS && element(xml, "floppy", &p, name, sizeof(name), &pr)) {
        int i = r->disks++;
        while (i > 0 && prio[i - 1] > pr) {
            prio[i] = prio[i - 1];
            memcpy(r->disk[i], r->disk[i - 1], RP9_NAME);
            i--;
        }
        prio[i] = pr;
        snprintf(r->disk[i], RP9_NAME, "%s", name);
    }
    p = xml;
    r->harddrive = element(xml, "harddrive", &p, r->hd, sizeof(r->hd), NULL);
    r->turbo = strstr(xml, "<compatibility>turbo-floppy</compatibility>") != NULL;
    const char *boot = strstr(xml, "<boot ");
    const char *end = boot ? strchr(boot, '>') : NULL;
    if (end && strstr(boot, "type=\"hdf\"") && strstr(boot, "type=\"hdf\"") < end) {
        r->boot_hdf = atoi(end + 1);
    }
    free(xml);
    return true;
}

/* The fast RAM a board of ours has nearest to what's asked (1-8 MB, Zorro
 * II: Zorro III's 8 MB as 8)
 */
static int fast_kb(uint32_t bytes)
{
    int kb = 0;
    for (int k = 1024; k <= 8192 && (uint32_t)k * 1024 <= bytes; k *= 2) {
        kb = k;
    }
    return bytes && !kb ? 1024 : kb;
}

/* The systems as our models (Kickstart: the nearest one that model takes) */
bool amiga_rp9_machine(const char *path, amiga_rp9_machine_t *m)
{
    rp9_t r;
    if (!rp9_read(path, &r) || !r.system[0]) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    snprintf(m->title, sizeof(m->title), "%s", r.title);
    snprintf(m->system, sizeof(m->system), "%s", r.system);
    int rom = r.rom;
    if (strcmp(r.system, "a-1200") == 0 || strcmp(r.system, "a-4000") == 0) {
        m->model = AMIGA_A1200;
        m->kickstart = rom == 300 ? 300 : 310;
        m->chip_kb = 2048;
    } else if (strcmp(r.system, "a-600") == 0) {
        m->model = AMIGA_A600;
        m->kickstart = rom >= 300 ? 310 : 205;
        m->chip_kb = r.chip_ram >= 2048u * 1024 ? 2048 : 1024;
    } else if (strcmp(r.system, "a-500") == 0 || strcmp(r.system, "a-500plus") == 0 ||
               strcmp(r.system, "a-2000") == 0) {
        bool plus = strcmp(r.system, "a-500plus") == 0;
        m->model = strcmp(r.system, "a-2000") == 0 ? AMIGA_A2000 : AMIGA_A500;
        m->kickstart = rom == 0 ? (plus ? 204 : 130) : rom <= 120 ? 120 : rom < 200 ? 130 : rom < 300 ? 204 : 310;
        /* the A500+: ECS with 1 MB chip RAM; else 512 KB and 512 KB slow */
        m->chip_kb = plus || r.chip_ram >= 1024u * 1024 ? 1024 : 512;
        m->slow_kb = m->chip_kb == 512 ? 512 : 0;
    } else {
        return false;
    }
    m->fast_kb = fast_kb(r.fast_ram);
    return true;
}
