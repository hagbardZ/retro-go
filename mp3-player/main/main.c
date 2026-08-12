#include <rg_system.h>
#include <rg_storage.h>
#include <rg_gui.h>
#include <rg_display.h>
#include <rg_audio.h>
#include <rg_input.h>
#include <rg_utils.h>
#include <rg_surface.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <esp_random.h>
#include <string.h>
#include <strings.h>

#include "mp3dec.h"

#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_BUFFER_SAMPLES 4096
#define AUDIO_BUFFER_BYTES (AUDIO_BUFFER_SAMPLES * sizeof(rg_audio_frame_t))
#define MP3_STREAM_BUFFER_SIZE 32768
#define MP3_PRIME_FRAMES 2
#define PLAYLIST_INITIAL_CAPACITY 256
#define MUSIC_PATH RG_BASE_PATH_MUSIC

static rg_app_t *app;
static rg_surface_t *surface;
static rg_audio_frame_t audio_buffer[AUDIO_BUFFER_SAMPLES];
static int16_t pcm_buffer[AUDIO_BUFFER_SAMPLES * 2];
static HMP3Decoder mp3_decoder = NULL;
static FILE *mp3_file;
static uint8_t *mp3_stream = NULL;
static size_t mp3_stream_fill = 0;
static const unsigned char *mp3_input_ptr = NULL;
static size_t mp3_input_left = 0;
static size_t mp3_stream_offset = 0;
static bool mp3_stream_eof = false;
static char current_file[RG_PATH_MAX];
static bool playing;
static int sample_rate;
static int mp3_current_bitrate;

static uint64_t playback_frames;
static size_t mp3_file_size;
static size_t mp3_id3_skip_bytes;
static bool mp3_has_duration;
static int mp3_skip_frames;
static int64_t last_session_save_ms;
static bool mp3_anchor_pending;
static uint64_t mp3_anchor_seconds;

/* ID3v2 tag metadata shown on the player screen. */
#define TAG_TEXT_MAX 128
static char tag_artist[TAG_TEXT_MAX];
static char tag_title[TAG_TEXT_MAX];
static char tag_album[TAG_TEXT_MAX];
static char tag_year[TAG_TEXT_MAX];
static char tag_genre[TAG_TEXT_MAX];

/* Parsed MP3 metadata used for accurate duration and seeking. For VBR files
 * the first frame's bitrate is meaningless, so we prefer the Xing/Info VBR
 * header (exact frame count + optional 100-entry seek TOC) and fall back to
 * sampling the bitrate across the file. */
#define MP3_SAMPLE_POINTS 8
#define MP3_TOC_SIZE 100

typedef struct {
    int v;             /* compact version: 0=MPEG2.5, 1=MPEG2, 2=MPEG1 */
    int bitrate;       /* bps */
    int samplerate;
    int channels;      /* 1 or 2 */
    int padding;
} mp3_frame_t;

typedef struct {
    bool has_xing;          /* Xing/Info header parsed successfully */
    bool has_toc;           /* Xing TOC table present */
    bool cbr;               /* constant bitrate */
    uint32_t samplerate;
    uint32_t samples_per_frame;
    uint32_t frames;        /* total frames (0 if unknown) */
    uint32_t audio_bytes;   /* audio payload bytes (0 if unknown) */
    uint32_t avg_bitrate_bps;
    uint8_t toc[MP3_TOC_SIZE];
} mp3_meta_t;

static mp3_meta_t mp3_meta;

static char session_track[RG_PATH_MAX + 1];
static uint64_t session_byte;
static bool session_random;
static bool session_repeat;
static volatile bool session_dirty;
static volatile bool session_saving;

static char (*playlist)[RG_PATH_MAX + 1] = NULL;
static int playlist_count = 0;
static int playlist_capacity = 0;
static int playlist_index = -1;
static bool random_mode = false;
static bool repeat_mode = false;

static volatile bool picker_active = false;
static volatile bool picker_done = false;
static char *picker_result = NULL;
static bool advance_pending = false;

static void save_session_request(void);
static size_t mp3_total_seconds(void);

/* Indexed by the 2-bit MPEG version field from the frame header
 * (0=MPEG2.5, 2=MPEG2, 3=MPEG1) -> compact index used below. -1 is invalid. */
static const int mp3_ver_map[4] = {0, -1, 1, 2};
static const int mp3_samplerates[3][3] = {
    {11025, 12000,  8000}, /* MPEG2.5 */
    {22050, 24000, 16000}, /* MPEG2   */
    {44100, 48000, 32000}, /* MPEG1   */
};
static const int mp3_bitrates_kbps[3][15] = {
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}, /* MPEG2.5 */
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}, /* MPEG2   */
    {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}, /* MPEG1 */
};
static const int mp3_samples_per_frame[3] = {576, 576, 1152};
/* Side info size used to locate the Xing/Info tag after the 4-byte frame
 * header. Indexed [version][mono/stereo]. */
static const int mp3_side_info[3][2] = {
    {9, 17},  /* MPEG2.5 */
    {9, 17},  /* MPEG2   */
    {17, 32}, /* MPEG1   */
};

static bool mp3_parse_frame(const unsigned char *buf, mp3_frame_t *f)
{
    int ver_idx, layer, br_idx, sr_idx, chan_mode, v;

    if (!f || !buf)
        return false;
    if (buf[0] != 0xFF || (buf[1] & 0xE0) != 0xE0)
        return false;

    ver_idx = (buf[1] >> 3) & 0x03;
    layer = 4 - ((buf[1] >> 1) & 0x03);
    br_idx = (buf[2] >> 4) & 0x0F;
    sr_idx = (buf[2] >> 2) & 0x03;
    chan_mode = (buf[3] >> 6) & 0x03;

    if (ver_idx == 1 || layer != 3 || br_idx == 0 || br_idx == 15 || sr_idx == 3)
        return false;

    v = mp3_ver_map[ver_idx];
    f->v = v;
    f->bitrate = mp3_bitrates_kbps[v][br_idx] * 1000;
    f->samplerate = mp3_samplerates[v][sr_idx];
    f->channels = (chan_mode == 3) ? 1 : 2;
    f->padding = (buf[2] >> 1) & 1;
    return true;
}

static uint32_t mp3_get_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Size of trailing tags (ID3v1, APEv2) that must not count as audio data. */
static size_t mp3_trailing_bytes(void)
{
    uint8_t buf[128];
    size_t n = 0;
    long saved;

    if (!mp3_file || mp3_file_size < 128)
        return 0;

    saved = ftell(mp3_file);
    if (fseek(mp3_file, (long)(mp3_file_size - 128), SEEK_SET) == 0 &&
        fread(buf, 1, 128, mp3_file) == 128 && memcmp(buf, "TAG", 3) == 0)
        n += 128;
    if (mp3_file_size >= n + 32 &&
        fseek(mp3_file, (long)(mp3_file_size - n - 32), SEEK_SET) == 0 &&
        fread(buf, 1, 32, mp3_file) == 32 && memcmp(buf, "APETAGEX", 8) == 0)
    {
        uint32_t size = mp3_get_be32(buf + 12);
        n += size + 32; /* footer is not included in the size field */
    }
    fseek(mp3_file, saved, SEEK_SET);
    return n;
}

/* Read up to 4KB at a byte offset and return the bitrate of the first valid
 * frame header found there. Used to estimate the average bitrate of VBR files
 * that lack a Xing header. */
static int mp3_probe_bitrate_at(size_t offset, int expected_samplerate)
{
    static uint8_t probe_buf[4096];
    size_t want, got, i;
    long saved;

    if (!mp3_file || offset >= mp3_file_size)
        return 0;

    want = sizeof(probe_buf);
    if (mp3_file_size - offset < want)
        want = mp3_file_size - offset;
    if (want < 4)
        return 0;

    saved = ftell(mp3_file);
    if (fseek(mp3_file, (long)offset, SEEK_SET) != 0)
        return 0;
    got = fread(probe_buf, 1, want, mp3_file);
    fseek(mp3_file, saved, SEEK_SET);
    if (got < 4)
        return 0;

    for (i = 0; i + 4 <= got; i++)
    {
        mp3_frame_t f;
        if (mp3_parse_frame(probe_buf + i, &f) && f.samplerate == expected_samplerate)
            return f.bitrate;
    }
    return 0;
}

/* Parse the first frame's Xing/Info VBR header and, if absent, sample the
 * bitrate across the file so total duration / seeking is accurate for VBR. */
static void mp3_analyze(void)
{
    mp3_frame_t first;
    const unsigned char *frame = mp3_input_ptr;
    size_t avail, side, i;
    uint64_t audio_bytes;
    double recip_sum;
    int count = 0, min_br = 0, max_br = 0;

    if (!frame || !mp3_parse_frame(frame, &first))
        return;

    mp3_meta.samplerate = first.samplerate;
    mp3_meta.samples_per_frame = mp3_samples_per_frame[first.v];
    mp3_meta.avg_bitrate_bps = first.bitrate;

    if (mp3_file_size > mp3_id3_skip_bytes)
        audio_bytes = (uint64_t)mp3_file_size - mp3_id3_skip_bytes - mp3_trailing_bytes();
    else
        audio_bytes = 0;

    side = (size_t)mp3_side_info[first.v][first.channels == 1 ? 0 : 1];
    avail = (size_t)(mp3_stream + mp3_stream_fill - frame);

    if (avail >= side + 16)
    {
        const unsigned char *xing = frame + 4 + side;
        if (memcmp(xing, "Xing", 4) == 0 || memcmp(xing, "Info", 4) == 0)
        {
            uint32_t flags = mp3_get_be32(xing + 4);
            const unsigned char *p = xing + 8;

            if (flags & 1)
            {
                mp3_meta.frames = mp3_get_be32(p);
                p += 4;
            }
            if (flags & 2)
            {
                mp3_meta.audio_bytes = mp3_get_be32(p);
                p += 4;
            }
            if (flags & 4 && (size_t)(p - frame) + MP3_TOC_SIZE <= avail)
            {
                memcpy(mp3_meta.toc, p, MP3_TOC_SIZE);
                mp3_meta.has_toc = true;
            }

            if (mp3_meta.frames > 0)
            {
                mp3_meta.has_xing = true;
                mp3_meta.cbr = (memcmp(xing, "Info", 4) == 0);
                if (mp3_meta.audio_bytes == 0 || mp3_meta.audio_bytes > (uint64_t)mp3_file_size)
                    mp3_meta.audio_bytes = (uint32_t)audio_bytes;
                uint64_t total_sec = (uint64_t)mp3_meta.frames * mp3_meta.samples_per_frame / mp3_meta.samplerate;
                if (total_sec > 0 && mp3_meta.audio_bytes > 0)
                    mp3_meta.avg_bitrate_bps = (uint32_t)((uint64_t)mp3_meta.audio_bytes * 8 / total_sec);
                RG_LOGI("mp3_analyze: Xing %s frames=%u bytes=%u toc=%d samplerate=%u",
                        mp3_meta.cbr ? "Info" : "VBR", (unsigned int)mp3_meta.frames,
                        (unsigned int)mp3_meta.audio_bytes, mp3_meta.has_toc, (unsigned int)mp3_meta.samplerate);
                return;
            }
        }
    }

    /* No usable Xing header: sample bitrates to tell CBR from VBR and estimate
     * the average. We sample at byte offsets, so frames are hit with probability
     * proportional to their size (i.e. their bitrate). For such biased samples
     * the harmonic mean converges to the correct byte-weighted average bitrate
     * (total bytes / total time), which is what we need for the duration. */
    min_br = max_br = first.bitrate;
    recip_sum = 1.0 / (double)first.bitrate;
    count = 1;
    if (audio_bytes > 0)
    {
        for (i = 1; i <= MP3_SAMPLE_POINTS; i++)
        {
            size_t off = mp3_id3_skip_bytes + (size_t)(audio_bytes * i / (MP3_SAMPLE_POINTS + 1));
            int br = mp3_probe_bitrate_at(off, first.samplerate);
            if (br > 0)
            {
                recip_sum += 1.0 / (double)br;
                count++;
                if (br < min_br)
                    min_br = br;
                if (br > max_br)
                    max_br = br;
            }
        }
    }
    mp3_meta.cbr = (count > 0 && min_br == max_br);
    mp3_meta.avg_bitrate_bps = recip_sum > 0.0 ? (uint32_t)((double)count / recip_sum) : first.bitrate;
    mp3_meta.audio_bytes = (uint32_t)audio_bytes;
    RG_LOGI("mp3_analyze: no Xing, %s avg_bitrate=%u samples=%d", mp3_meta.cbr ? "CBR" : "VBR",
            (unsigned int)mp3_meta.avg_bitrate_bps, count);
}

/* Decode an ID3v2 text frame body into a UTF-8 NUL-terminated string.
 * Handles ISO-8859-1, UTF-8 and UTF-16 (with/without BOM) encodings. */
static size_t id3_decode_text(const uint8_t *data, size_t len, char *out, size_t out_size)
{
    int enc;
    size_t i, o = 0;

    out[0] = '\0';
    if (!data || len < 1 || out_size < 2)
        return 0;

    enc = data[0];
    i = 1;

    if (enc == 1 && len >= 3 && ((data[1] == 0xFF && data[2] == 0xFE) || (data[1] == 0xFE && data[2] == 0xFF)))
    {
        /* UTF-16 with BOM: FF FE = little-endian, FE FF = big-endian */
        enc = (data[1] == 0xFF && data[2] == 0xFE) ? 1 : 2;
        i = 3;
    }

    while (i < len && o + 4 < out_size)
    {
        int cp;

        if (enc == 0)
        {
            /* ISO-8859-1 maps 1:1 to Unicode code points */
            uint8_t c = data[i++];
            if (c == 0)
                break;
            cp = c;
        }
        else if (enc == 3)
        {
            /* UTF-8 */
            uint8_t c = data[i];
            size_t need = 1;
            if (c == 0)
                break;
            if ((c & 0xE0) == 0xC0)
                need = 2;
            else if ((c & 0xF0) == 0xE0)
                need = 3;
            else if ((c & 0xF8) == 0xF0)
                need = 4;
            if (i + need > len)
            {
                out[o++] = c;
                i++;
                continue;
            }
            const char *p = (const char *)&data[i];
            const char *p2 = p;
            cp = rg_utf8_decode(&p2);
            if (cp < 0 || p2 == p)
            {
                out[o++] = c; /* invalid sequence, copy the byte verbatim */
                i++;
                continue;
            }
            i += (size_t)(p2 - p);
        }
        else
        {
            /* UTF-16 little-endian (enc==1) or big-endian (enc==2) */
            uint16_t w;
            if (i + 2 > len)
                break;
            w = (enc == 2) ? (uint16_t)((data[i] << 8) | data[i + 1])
                           : (uint16_t)(data[i] | (data[i + 1] << 8));
            i += 2;
            if (w == 0)
                break;
            if (w >= 0xD800 && w <= 0xDBFF && i + 2 <= len)
            {
                uint16_t w2 = (enc == 2) ? (uint16_t)((data[i] << 8) | data[i + 1])
                                         : (uint16_t)(data[i] | (data[i + 1] << 8));
                if (w2 >= 0xDC00 && w2 <= 0xDFFF)
                {
                    cp = 0x10000 + ((w - 0xD800) << 10) + (w2 - 0xDC00);
                    i += 2;
                }
                else
                    cp = 0xFFFD;
            }
            else if (w >= 0xD800 && w <= 0xDFFF)
                cp = 0xFFFD;
            else
                cp = w;
        }

        o += rg_utf8_encode(out + o, cp);
    }
    out[o] = '\0';

    /* Trim trailing whitespace */
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\t'))
        out[--o] = '\0';
    return o;
}

/* Standard ID3v1 genre names, indexed by the numeric genre code that ID3v2
 * TCON frames sometimes carry ("(17)" or "17"). */
static const char *const id3v1_genres[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop",
    "Jazz", "Metal", "New Age", "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock",
    "Techno", "Industrial", "Alternative", "Ska", "Death Metal", "Pranks", "Soundtrack",
    "Euro-Techno", "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion", "Trance",
    "Classical", "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise",
    "AlternRock", "Bass", "Soul", "Punk", "Space", "Meditative", "Instrumental Pop",
    "Instrumental Rock", "Ethnic", "Gothic", "Darkwave", "Techno-Industrial", "Electronic",
    "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40",
    "Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave",
    "Psychadelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz",
    "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock", "Folk", "Folk-Rock",
    "National Folk", "Swing", "Fast Fusion", "Bebob", "Latin", "Revival", "Celtic", "Bluegrass",
    "Avantgarde", "Gothic Rock", "Progressive Rock", "Psychedelic Rock", "Symphonic Rock",
    "Slow Rock", "Big Band", "Chorus", "Easy Listening", "Acoustic", "Humour", "Speech",
    "Chanson", "Opera", "Chamber Music", "Sonata", "Symphony", "Booty Bass", "Primus",
    "Porn Groove", "Satire", "Slow Jam", "Club", "Tango", "Samba", "Folklore", "Ballad",
    "Power Ballad", "Rhythmic Soul", "Freestyle", "Duet", "Punk Rock", "Drum Solo", "A capella",
    "Euro-House", "Dance Hall", "Goa", "Drum & Bass", "Club-House", "Hardcore", "Terror",
    "Indie", "BritPop", "Negerpunk", "Polsk Punk", "Beat", "Christian Gangsta Rap",
    "Heavy Metal", "Black Metal", "Crossover", "Contemporary Christian", "Christian Rock",
    "Merengue", "Salsa", "Thrash Metal", "Anime", "JPop", "Synthpop",
};

/* Parse the ID3v2 tag in buf (starting at "ID3") and fill the tag_* globals.
 * Supports ID3v2.2, v2.3 and v2.4 frames. */
static void mp3_parse_id3v2(const uint8_t *buf, size_t size)
{
    size_t pos, end;
    int major;

    tag_artist[0] = tag_title[0] = tag_album[0] = tag_year[0] = tag_genre[0] = '\0';

    if (!buf || size < 10 || memcmp(buf, "ID3", 3) != 0)
        return;

    major = buf[3];
    end = 10 + (((size_t)(buf[6] & 0x7F) << 21) | ((size_t)(buf[7] & 0x7F) << 14) |
                ((size_t)(buf[8] & 0x7F) << 7) | (size_t)(buf[9] & 0x7F));
    if (end > size)
        end = size;
    pos = 10;

    while (pos + 6 <= end)
    {
        const uint8_t *frame = buf + pos;
        char frame_id[5];
        size_t body_len, hdr;

        if (frame[0] == 0)
            break; /* padding */

        if (major >= 3)
        {
            if (pos + 10 > end)
                break;
            memcpy(frame_id, frame, 4);
            frame_id[4] = '\0';
            if (major == 3)
                body_len = ((size_t)frame[4] << 24) | ((size_t)frame[5] << 16) |
                           ((size_t)frame[6] << 8) | frame[7];
            else
                body_len = ((size_t)(frame[4] & 0x7F) << 21) | ((size_t)(frame[5] & 0x7F) << 14) |
                           ((size_t)(frame[6] & 0x7F) << 7) | (frame[7] & 0x7F);
            hdr = 10;
        }
        else
        {
            /* ID3v2.2: 3-byte frame id + 3-byte big-endian size */
            memcpy(frame_id, frame, 3);
            frame_id[3] = '\0';
            body_len = ((size_t)frame[3] << 16) | ((size_t)frame[4] << 8) | frame[5];
            hdr = 6;
        }

        if (pos + hdr + body_len > end)
            body_len = end - (pos + hdr);

        {
            const uint8_t *body = frame + hdr;

            if (strcmp(frame_id, "TPE1") == 0 || strcmp(frame_id, "TP1") == 0)
                id3_decode_text(body, body_len, tag_artist, TAG_TEXT_MAX);
            else if (strcmp(frame_id, "TIT2") == 0 || strcmp(frame_id, "TT2") == 0)
                id3_decode_text(body, body_len, tag_title, TAG_TEXT_MAX);
            else if (strcmp(frame_id, "TALB") == 0 || strcmp(frame_id, "TAL") == 0)
                id3_decode_text(body, body_len, tag_album, TAG_TEXT_MAX);
            else if (strcmp(frame_id, "TYER") == 0 || strcmp(frame_id, "TYE") == 0 ||
                     strcmp(frame_id, "TDRC") == 0)
                id3_decode_text(body, body_len, tag_year, TAG_TEXT_MAX);
            else if (strcmp(frame_id, "TCON") == 0 || strcmp(frame_id, "TCO") == 0)
                id3_decode_text(body, body_len, tag_genre, TAG_TEXT_MAX);
        }

        pos += hdr + body_len;
    }

    /* TDRC (ID3v2.4) is often a full ISO 8601 timestamp; keep just the year. */
    if (tag_year[0] && tag_year[1] && tag_year[2] && tag_year[3] &&
        tag_year[0] >= '0' && tag_year[0] <= '9' && tag_year[1] >= '0' && tag_year[1] <= '9' &&
        tag_year[2] >= '0' && tag_year[2] <= '9' && tag_year[3] >= '0' && tag_year[3] <= '9')
        tag_year[4] = '\0';

    /* A numeric ID3v1 genre code ("17" or "(17)") becomes its genre name. */
    if (tag_genre[0])
    {
        char *endptr;
        const char *g = tag_genre;
        long id;

        if (g[0] == '(')
            g++;
        id = strtol(g, &endptr, 10);
        if (endptr != g && *endptr == '\0' && id >= 0 &&
            (size_t)id < sizeof(id3v1_genres) / sizeof(id3v1_genres[0]))
            snprintf(tag_genre, sizeof(tag_genre), "%s", id3v1_genres[id]);
    }

    RG_LOGI("id3v2: artist='%s' title='%s' album='%s' year='%s' genre='%s'",
            tag_artist, tag_title, tag_album, tag_year, tag_genre);
}

/* Trim trailing NULs and the space padding used by ID3v1 fixed-width fields. */
static void trim_tag_text(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\0'))
        s[--len] = '\0';
}

/* Parse an ID3v1 tag (128-byte "TAG" block) into the tag_* globals. Fields
 * already filled by ID3v2 are left untouched so ID3v2 keeps priority. */
static void mp3_parse_id3v1(const uint8_t *buf)
{
    if (!buf || memcmp(buf, "TAG", 3) != 0)
        return;

    if (!tag_title[0])
    {
        memcpy(tag_title, buf + 3, 30);
        tag_title[30] = '\0';
        trim_tag_text(tag_title);
    }
    if (!tag_artist[0])
    {
        memcpy(tag_artist, buf + 33, 30);
        tag_artist[30] = '\0';
        trim_tag_text(tag_artist);
    }
    if (!tag_album[0])
    {
        memcpy(tag_album, buf + 63, 30);
        tag_album[30] = '\0';
        trim_tag_text(tag_album);
    }
    if (!tag_year[0])
    {
        memcpy(tag_year, buf + 93, 4);
        tag_year[4] = '\0';
        trim_tag_text(tag_year);
    }
    if (!tag_genre[0] && buf[127] != 0xFF &&
        (size_t)buf[127] < sizeof(id3v1_genres) / sizeof(id3v1_genres[0]))
        snprintf(tag_genre, sizeof(tag_genre), "%s", id3v1_genres[buf[127]]);

    RG_LOGI("id3v1: artist='%s' title='%s' album='%s' year='%s' genre='%s'",
            tag_artist, tag_title, tag_album, tag_year, tag_genre);
}

/* Read the 128-byte ID3v1 tag at the end of the file and parse it. Called
 * after ID3v2 parsing so its fields take priority. Restores the file position
 * since streaming relies on sequential reads. */
static void mp3_read_id3v1(void)
{
    uint8_t buf[128];
    long saved;

    if (!mp3_file || mp3_file_size < 128)
        return;

    saved = ftell(mp3_file);
    if (fseek(mp3_file, (long)(mp3_file_size - 128), SEEK_SET) == 0 &&
        fread(buf, 1, 128, mp3_file) == 128)
        mp3_parse_id3v1(buf);
    fseek(mp3_file, saved, SEEK_SET);
}

static uint64_t mp3_audio_bytes(void)
{
    if (mp3_meta.audio_bytes > 0)
        return mp3_meta.audio_bytes;
    if (mp3_file_size > mp3_id3_skip_bytes)
        return (uint64_t)mp3_file_size - mp3_id3_skip_bytes;
    return 0;
}

/* Map a seek time to an absolute byte offset. Uses the Xing TOC table when
 * available (accurate for VBR), otherwise a linear byte/time mapping. */
static size_t mp3_time_to_byte(size_t seconds)
{
    uint64_t total = mp3_total_seconds();
    uint64_t audio = mp3_audio_bytes();
    double frac, byte_frac;

    if (total == 0 || audio == 0)
        return mp3_id3_skip_bytes;
    if (seconds > total)
        seconds = (size_t)total;

    frac = (double)seconds / (double)total;
    byte_frac = frac;

    if (mp3_meta.has_toc)
    {
        double p = frac * 100.0;
        if (p <= 0.0)
            byte_frac = 0.0;
        else if (p >= 100.0)
            byte_frac = 1.0;
        else if (p < 1.0)
            byte_frac = mp3_meta.toc[0] / 256.0;
        else
        {
            int idx = (int)p; /* 1..99 */
            double f = p - idx;
            double lo = mp3_meta.toc[idx - 1] / 256.0;
            double hi = mp3_meta.toc[idx] / 256.0;
            byte_frac = lo + (hi - lo) * f;
        }
    }

    return mp3_id3_skip_bytes + (size_t)(byte_frac * (double)audio);
}

/* Map an absolute byte offset to a play time (inverse of mp3_time_to_byte). */
static size_t mp3_byte_to_time(size_t byte)
{
    uint64_t total = mp3_total_seconds();
    uint64_t audio = mp3_audio_bytes();
    double byte_frac, t_frac;

    if (total == 0 || audio == 0 || byte <= mp3_id3_skip_bytes)
        return 0;
    if (byte >= mp3_id3_skip_bytes + audio)
        return (size_t)total;

    byte_frac = (double)(byte - mp3_id3_skip_bytes) / (double)audio;
    t_frac = byte_frac;

    if (mp3_meta.has_toc)
    {
        double x = byte_frac * 256.0;
        if (x <= mp3_meta.toc[0])
            t_frac = mp3_meta.toc[0] > 0 ? (x / mp3_meta.toc[0]) / 100.0 : 0.0;
        else
        {
            int idx = 1;
            for (; idx < MP3_TOC_SIZE; idx++)
            {
                if (x <= mp3_meta.toc[idx])
                {
                    double lo = mp3_meta.toc[idx - 1];
                    double hi = mp3_meta.toc[idx];
                    double f = (hi > lo) ? (x - lo) / (hi - lo) : 0.0;
                    t_frac = (idx + f) / 100.0;
                    break;
                }
            }
            if (idx >= MP3_TOC_SIZE)
                t_frac = 1.0;
        }
    }

    return (size_t)(t_frac * (double)total);
}

static void draw_text_centered(int y, const char *text)
{
    while (rg_display_is_busy())
        rg_task_yield();
    rg_gui_set_surface(surface);
    rg_surface_fill(surface, NULL, C_BLACK);
    rg_gui_draw_text(RG_GUI_CENTER, y, 0, text, C_WHITE, C_BLACK, RG_TEXT_ALIGN_CENTER);
    rg_gui_set_surface(NULL);
    rg_display_submit(surface, 0);
}

static bool is_mp3_file(const char *path)
{
    const char *ext = rg_extension(path);
    return ext && (strcasecmp(ext, "mp3") == 0);
}

static bool find_mp3_frame_start_from(const unsigned char *start, size_t left)
{
    if (!start || left < 4)
    {
        if (start)
        {
            mp3_input_ptr = start + left;
            mp3_input_left = 0;
        }
        return false;
    }

    HMP3Decoder temp_decoder = MP3InitDecoder();
    if (!temp_decoder)
        return false;

    const unsigned char *ptr = start;
    const unsigned char *end = start + left;
    bool found = false;

    while (ptr + 4 <= end)
    {
        int offset = MP3FindSyncWord(ptr, (int)(end - ptr));
        if (offset < 0)
            break;

        ptr += offset;
        MP3FrameInfo info = {0};
        if (MP3GetNextFrameInfo(temp_decoder, &info, (unsigned char *)ptr) == ERR_MP3_NONE &&
            info.nChans > 0 && info.samprate > 0 && info.outputSamps > 0)
        {
            found = true;
            break;
        }
        ptr += 1;
    }

    MP3FreeDecoder(temp_decoder);

    if (!found || ptr >= end)
    {
        if (left >= 4)
        {
            mp3_input_ptr = end - 3;
            mp3_input_left = 3;
        }
        else
        {
            mp3_input_ptr = start + left;
            mp3_input_left = 0;
        }
        return false;
    }

    mp3_input_ptr = ptr;
    mp3_input_left = end - ptr;
    return true;
}

static bool find_mp3_frame_start(void)
{
    return find_mp3_frame_start_from(mp3_stream, mp3_stream_fill);
}

static bool refill_mp3_stream(void)
{
    if (!mp3_stream || !mp3_file)
        return false;

    if (mp3_input_left > 0 && mp3_input_ptr != mp3_stream)
    {
        mp3_stream_offset += (size_t)(mp3_input_ptr - mp3_stream);
        memmove(mp3_stream, mp3_input_ptr, mp3_input_left);
        mp3_input_ptr = mp3_stream;
    }
    else if (mp3_input_left == 0)
    {
        mp3_stream_offset += mp3_stream_fill;
        mp3_input_ptr = mp3_stream;
    }

    size_t free_space = MP3_STREAM_BUFFER_SIZE - mp3_input_left;
    /* The SD card is slow (~36ms per 32KB read), and a read longer than the
     * ~16ms audio DMA buffer starves playback (audible crackle on silence).
     * Read in smaller chunks so each stall stays well under the buffer. */
    size_t read_size = free_space > 8192 ? 8192 : free_space;
    size_t read_bytes = fread(mp3_stream + mp3_input_left, 1, read_size, mp3_file);
    mp3_input_left += read_bytes;
    mp3_stream_fill = mp3_input_left;
    mp3_stream_eof = (read_bytes == 0);


    return mp3_input_left > 0;
}

static bool open_mp3(const char *path)
{
    if (!path || !*path)
        return false;

    if (mp3_file)
    {
        fclose(mp3_file);
        mp3_file = NULL;
    }
    if (mp3_decoder)
    {
        MP3FreeDecoder(mp3_decoder);
        mp3_decoder = NULL;
    }
    free(mp3_stream);
    mp3_stream = NULL;
    mp3_stream_fill = 0;
    mp3_input_ptr = NULL;
    mp3_input_left = 0;
    mp3_stream_offset = 0;
    mp3_stream_eof = false;

    RG_LOGI("open_mp3: path=%s", path);
    draw_text_centered(120, "Opening MP3...");

    mp3_file = fopen(path, "rb");
    if (!mp3_file)
    {
        RG_LOGE("open_mp3: failed to open file");
        return false;
    }

    fseek(mp3_file, 0, SEEK_END);
    long size = ftell(mp3_file);
    if (size < 0)
    {
        RG_LOGW("open_mp3: failed to get file size");
        mp3_file_size = 0;
    }
    else
    {
        mp3_file_size = (size_t)size;
    }
    fseek(mp3_file, 0, SEEK_SET);

    mp3_stream = malloc(MP3_STREAM_BUFFER_SIZE);
    if (!mp3_stream)
    {
        RG_LOGE("open_mp3: failed to allocate stream buffer");
        fclose(mp3_file);
        mp3_file = NULL;
        return false;
    }

    mp3_stream_fill = fread(mp3_stream, 1, MP3_STREAM_BUFFER_SIZE, mp3_file);
    RG_LOGI("open_mp3: initial read=%u bytes", (unsigned int)mp3_stream_fill);
    if (mp3_stream_fill == 0)
    {
        free(mp3_stream);
        mp3_stream = NULL;
        fclose(mp3_file);
        mp3_file = NULL;
        return false;
    }

    mp3_stream_eof = (mp3_stream_fill < MP3_STREAM_BUFFER_SIZE && feof(mp3_file));
    RG_LOGD("open_mp3: initial eof=%d", mp3_stream_eof);

    mp3_id3_skip_bytes = 0;
    tag_artist[0] = tag_title[0] = tag_album[0] = tag_year[0] = tag_genre[0] = '\0';
    if (mp3_stream_fill >= 10 && memcmp(mp3_stream, "ID3", 3) == 0)
    {
        size_t tag_size = ((mp3_stream[6] & 0x7F) << 21) |
                          ((mp3_stream[7] & 0x7F) << 14) |
                          ((mp3_stream[8] & 0x7F) << 7) |
                          (mp3_stream[9] & 0x7F);
        size_t skip_bytes = 10 + tag_size;
        RG_LOGI("open_mp3: detected ID3v2 tag size=%u", (unsigned int)tag_size);
        mp3_id3_skip_bytes = skip_bytes;
        if (skip_bytes <= mp3_stream_fill)
        {
            mp3_parse_id3v2(mp3_stream, skip_bytes);
            size_t remaining = mp3_stream_fill - skip_bytes;
            memmove(mp3_stream, mp3_stream + skip_bytes, remaining);
            mp3_stream_fill = remaining;
            /* file pointer already lies after the initial read, no seek needed */
        }
        else
        {
            size_t missing = skip_bytes - mp3_stream_fill;
            uint8_t *tag_buf = malloc(skip_bytes);
            if (tag_buf)
            {
                memcpy(tag_buf, mp3_stream, mp3_stream_fill);
                size_t got = fread(tag_buf + mp3_stream_fill, 1, missing, mp3_file);
                mp3_parse_id3v2(tag_buf, mp3_stream_fill + got);
                free(tag_buf);
                /* file pointer now sits right after the tag */
            }
            else
            {
                fseek(mp3_file, (long)missing, SEEK_CUR);
            }
            mp3_stream_fill = 0;
        }
        mp3_input_ptr = mp3_stream;
        mp3_input_left = mp3_stream_fill;
        mp3_stream_eof = false;
    }

    /* Files without an ID3v2 tag (or with missing fields) fall back to the
     * 128-byte ID3v1 tag at the end of the file. */
    mp3_read_id3v1();

    mp3_stream_offset = mp3_id3_skip_bytes;

    while (!find_mp3_frame_start())
    {
        if (mp3_stream_eof)
            break;
        if (!refill_mp3_stream())
            break;
    }

    if (!mp3_input_ptr || mp3_input_left == 0)
    {
        RG_LOGE("open_mp3: failed to find MP3 frame start");
        free(mp3_stream);
        mp3_stream = NULL;
        fclose(mp3_file);
        mp3_file = NULL;
        return false;
    }

    RG_LOGI("open_mp3: found sync at offset=%u", (unsigned int)(mp3_input_ptr - mp3_stream));
    mp3_decoder = MP3InitDecoder();
    if (!mp3_decoder)
    {
        free(mp3_stream);
        mp3_stream = NULL;
        fclose(mp3_file);
        mp3_file = NULL;
        return false;
    }

    playback_frames = 0;
    mp3_has_duration = false;
    mp3_skip_frames = 0;

    memset(&mp3_meta, 0, sizeof(mp3_meta));
    mp3_analyze();

    strncpy(current_file, path, sizeof(current_file) - 1);
    current_file[sizeof(current_file) - 1] = '\0';
    return true;
}

static bool decode_and_play(void)
{
    if (!mp3_decoder || !mp3_input_ptr)
    {
        RG_LOGE("decode_and_play: no decoder/input (decoder=%p ptr=%p)", (void *)mp3_decoder, (const void *)mp3_input_ptr);
        return false;
    }

    while (mp3_input_left < 2048 && !mp3_stream_eof)    {
        if (!refill_mp3_stream())
        {
            RG_LOGE("decode_and_play: refill failed in header loop");
            return false;
        }
    }

    if (mp3_input_left == 0)
    {
        RG_LOGE("decode_and_play: no input data left, stopping playback");
        return false;
    }

    while (true)
    {
        if (!mp3_input_ptr || mp3_input_ptr < mp3_stream || mp3_input_ptr > mp3_stream + mp3_stream_fill)
        {
            RG_LOGE("decode_and_play: invalid input ptr, resetting");
            mp3_input_ptr = mp3_stream;
            mp3_input_left = mp3_stream_fill;
        }

        const unsigned char *input_ptr = mp3_input_ptr;
        size_t bytes_left = mp3_input_left;
        int err = MP3Decode(mp3_decoder, &input_ptr, &bytes_left,
                            (short *)pcm_buffer, 0);

        size_t max_left = mp3_stream + mp3_stream_fill - input_ptr;
        if (bytes_left > max_left)
        {
            RG_LOGE("decode_and_play: clamping invalid bytes_left %u to %u", (unsigned int)bytes_left, (unsigned int)max_left);
            bytes_left = max_left;
        }

        mp3_input_ptr = input_ptr;
        mp3_input_left = bytes_left;

        if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW)
        {
            RG_LOGI("decode_and_play: underflow err=%d eof=%d", err, mp3_stream_eof);
            if (mp3_stream_eof)
            {
                RG_LOGE("decode_and_play: underflow at eof, stopping playback");
                return false;
            }
            if (!refill_mp3_stream())
            {
                RG_LOGE("decode_and_play: refill failed after underflow, stopping playback");
                return false;
            }
            continue;
        }

        if (err < 0)
        {
            RG_LOGW("decode_and_play: decode error %d", err);
            if (!mp3_stream_eof)
            {
                if (mp3_input_left >= 4)
                {
                    RG_LOGD("decode_and_play: header=%02X %02X %02X %02X", mp3_input_ptr[0], mp3_input_ptr[1], mp3_input_ptr[2], mp3_input_ptr[3]);
                }
                if (find_mp3_frame_start_from(mp3_input_ptr, mp3_input_left))
                {
                    RG_LOGI("decode_and_play: resync found at offset=%u, reinitializing decoder", (unsigned int)(mp3_input_ptr - mp3_stream));
                    MP3FreeDecoder(mp3_decoder);
                    mp3_decoder = MP3InitDecoder();
                    if (!mp3_decoder)
                    {
                        RG_LOGE("decode_and_play: MP3InitDecoder failed during resync");
                        return false;
                    }
                    mp3_skip_frames = MP3_PRIME_FRAMES;
                    continue;
                }
                if (mp3_input_left > 1)
                {
                    RG_LOGD("decode_and_play: advance by one byte after failed resync");
                    mp3_input_ptr += 1;
                    mp3_input_left -= 1;
                    continue;
                }
                if (!refill_mp3_stream())
                {
                    RG_LOGE("decode_and_play: refill failed after decode error, stopping playback");
                    return false;
                }
                continue;
            }
            RG_LOGE("decode_and_play: decode error %d at eof, stopping playback", err);
            return false;
        }

        /* After a seek/resync the decoder starts at an arbitrary frame boundary,
         * so the first frames may be garbage (bit reservoir miss or false sync).
         * Prime by decoding them without touching the audio output or sample rate. */
        if (mp3_skip_frames > 0)
        {
            RG_LOGI("decode_and_play: priming, discarding frame (%d remaining)", mp3_skip_frames);
            mp3_skip_frames--;
            if (mp3_input_left < 4 && !mp3_stream_eof)
                refill_mp3_stream();
            continue;
        }

        MP3FrameInfo info = {0};
        MP3GetLastFrameInfo(mp3_decoder, &info);
        if (info.bitrate > 0)
            mp3_current_bitrate = info.bitrate;
        if (info.samprate > 0 && info.samprate != sample_rate)
        {
            RG_LOGI("decode_and_play: sample rate %d -> %d", sample_rate, info.samprate);
            sample_rate = info.samprate;
            rg_audio_set_sample_rate(sample_rate);
        }

        /* Now that the real sample rate is known, apply the pending playhead
         * anchor (set by seek_mp3_to_byte) so elapsed time stays accurate. */
        if (mp3_anchor_pending)
        {
            mp3_anchor_pending = false;
            playback_frames = mp3_anchor_seconds * (uint64_t)sample_rate;
        }

        if (info.outputSamps <= 0)
        {
            RG_LOGE("decode_and_play: outputSamps=%d, stopping playback", info.outputSamps);
            return false;
        }

        size_t frames = 0;
        if (info.nChans == 2)
        {
            frames = info.outputSamps / 2;
            if (frames > AUDIO_BUFFER_SAMPLES)
            {
                RG_LOGE("decode_and_play: stereo frame overflow %u", (unsigned int)frames);
                return false;
            }
            memcpy(audio_buffer, pcm_buffer, frames * sizeof(rg_audio_frame_t));
        }
        else if (info.nChans == 1)
        {
            if ((size_t)info.outputSamps > AUDIO_BUFFER_SAMPLES)
            {
                RG_LOGE("decode_and_play: mono frame overflow %u", (unsigned int)info.outputSamps);
                return false;
            }
            for (size_t i = 0; i < (size_t)info.outputSamps; ++i)
            {
                audio_buffer[i].left = pcm_buffer[i];
                audio_buffer[i].right = pcm_buffer[i];
            }
            frames = (size_t)info.outputSamps;
        }
        else
        {
            RG_LOGE("decode_and_play: unexpected channels %d, stopping playback", info.nChans);
            return false;
        }

        playback_frames += frames;

        if (mp3_input_left < 4 && !mp3_stream_eof)
            refill_mp3_stream();

        rg_audio_submit(audio_buffer, frames);

        return true;
    }
}

static void format_time(char *buffer, size_t size, uint32_t seconds)
{
    unsigned int mins = (unsigned int)(seconds / 60);
    unsigned int secs = (unsigned int)(seconds % 60);
    snprintf(buffer, size, "%02u:%02u", mins, secs);
}

static size_t mp3_total_seconds(void)
{
    uint64_t total = 0;

    if (mp3_meta.has_xing && mp3_meta.frames > 0 && mp3_meta.samplerate > 0)
        total = (uint64_t)mp3_meta.frames * mp3_meta.samples_per_frame / mp3_meta.samplerate;
    else if (mp3_meta.avg_bitrate_bps > 0)
    {
        uint64_t audio = mp3_audio_bytes();
        if (audio > 0)
            total = (audio * 8 + mp3_meta.avg_bitrate_bps - 1) / mp3_meta.avg_bitrate_bps;
    }

    mp3_has_duration = (total > 0);
    return (size_t)total;
}

static size_t mp3_current_seconds(void)
{
    if (sample_rate == 0)
        return 0;
    return (size_t)(playback_frames / sample_rate);
}

static bool seek_mp3_to_byte(size_t target_byte)
{
    if (!mp3_file || !mp3_stream)
    {
        RG_LOGE("seek_mp3_to_byte: no file/stream");
        return false;
    }
    if (mp3_file_size == 0)
    {
        RG_LOGE("seek_mp3_to_byte: file size is 0");
        return false;
    }

    if (target_byte < mp3_id3_skip_bytes)
        target_byte = mp3_id3_skip_bytes;
    if (target_byte >= mp3_file_size)
        target_byte = mp3_file_size > 1 ? mp3_file_size - 1 : 0;

    RG_LOGI("seek_mp3_to_byte: target=%u filesize=%u id3=%u", (unsigned int)target_byte, (unsigned int)mp3_file_size, (unsigned int)mp3_id3_skip_bytes);

    /* Save the current stream state so a failed seek can leave playback running */
    size_t saved_offset = mp3_stream_offset;
    const unsigned char *saved_ptr = mp3_input_ptr;
    size_t saved_left = mp3_input_left;
    size_t saved_fill = mp3_stream_fill;
    bool saved_eof = mp3_stream_eof;

    if (fseek(mp3_file, (long)target_byte, SEEK_SET) != 0)
    {
        RG_LOGE("seek_mp3_to_byte: fseek failed target=%u", (unsigned int)target_byte);
        return false;
    }

    mp3_stream_offset = target_byte;
    mp3_input_ptr = mp3_stream;
    mp3_input_left = 0;
    mp3_stream_fill = 0;
    mp3_stream_eof = false;

    size_t read_bytes = fread(mp3_stream, 1, MP3_STREAM_BUFFER_SIZE, mp3_file);
    mp3_input_left = read_bytes;
    mp3_stream_fill = read_bytes;
    mp3_stream_eof = (read_bytes == 0);
    RG_LOGI("seek_mp3_to_byte: fread=%u eof=%d", (unsigned int)read_bytes, mp3_stream_eof);

    bool found_sync = false;
    if (find_mp3_frame_start_from(mp3_stream, mp3_stream_fill))
    {
        found_sync = true;
    }
    else
    {
        int tries = 0;
        for (; tries < 1024 && !mp3_stream_eof && mp3_input_left > 0; tries++)
        {
            if (!refill_mp3_stream())
                break;
            if (find_mp3_frame_start_from(mp3_stream, mp3_stream_fill))
            {
                found_sync = true;
                break;
            }
        }
        RG_LOGI("seek_mp3_to_byte: rescan done tries=%d found=%d", tries, found_sync);
    }

    if (!found_sync)
    {
        RG_LOGE("seek_mp3_to_byte: no sync found near byte %u", (unsigned int)target_byte);
        /* Restore the previous stream state so playback can continue */
        if (fseek(mp3_file, (long)saved_offset, SEEK_SET) == 0 && saved_fill > 0)
        {
            fread(mp3_stream, 1, saved_fill, mp3_file);
            mp3_stream_offset = saved_offset;
            if (saved_ptr >= mp3_stream && saved_ptr <= mp3_stream + saved_fill)
                mp3_input_ptr = saved_ptr;
            else
                mp3_input_ptr = mp3_stream;
            mp3_input_left = saved_left;
            mp3_stream_fill = saved_fill;
            mp3_stream_eof = saved_eof;
        }
        return false;
    }

    RG_LOGI("seek_mp3_to_byte: sync offset=%u input_left=%u fill=%u stream_offset=%u",
        (unsigned int)(mp3_input_ptr - mp3_stream), (unsigned int)mp3_input_left, (unsigned int)mp3_stream_fill, (unsigned int)mp3_stream_offset);

    if (mp3_decoder)
    {
        MP3FreeDecoder(mp3_decoder);
        mp3_decoder = NULL;
    }
    mp3_decoder = MP3InitDecoder();
    if (!mp3_decoder)
    {
        RG_LOGE("seek_mp3_to_byte: MP3InitDecoder failed (heap exhausted?)");
        return false;
    }

    mp3_skip_frames = MP3_PRIME_FRAMES;

    /* Anchor the playhead to the byte position where playback actually resumes
     * so the elapsed time and subsequent relative seeks stay accurate (esp. VBR).
     * The anchor seconds is applied with the actual sample rate once the first
     * frame after the seek has been decoded (rate unknown right after resume). */
    uint64_t resume_byte = (uint64_t)mp3_stream_offset + (uint64_t)(mp3_input_ptr - mp3_stream);
    mp3_anchor_seconds = mp3_byte_to_time((size_t)resume_byte);
    mp3_anchor_pending = true;
    uint64_t rate = (sample_rate > 0) ? (uint64_t)sample_rate : AUDIO_SAMPLE_RATE;
    playback_frames = mp3_anchor_seconds * rate;
    RG_LOGI("seek_mp3_to_byte: anchored resume_byte=%u resume_sec=%u", (unsigned int)resume_byte, (unsigned int)mp3_anchor_seconds);
    return true;
}

static bool mp3_seek_to_time(size_t seek_seconds)
{
    size_t total = mp3_total_seconds();
    if (total == 0 || mp3_file_size == 0)
    {
        RG_LOGE("seek_to_time: FAIL total=%u filesize=%u", (unsigned int)total, (unsigned int)mp3_file_size);
        return false;
    }

    if (seek_seconds > total)
        seek_seconds = total;

    size_t target_byte = mp3_time_to_byte(seek_seconds);
    RG_LOGI("seek_to_time: want=%us total=%us target_byte=%u", (unsigned int)seek_seconds, (unsigned int)total, (unsigned int)target_byte);
    if (!seek_mp3_to_byte(target_byte))
    {
        RG_LOGE("seek_to_time: seek_mp3_to_byte FAILED at byte %u", (unsigned int)target_byte);
        return false;
    }
    RG_LOGI("seek_to_time: OK resume_sec=%u rate=%u", (unsigned int)mp3_current_seconds(), (unsigned int)sample_rate);
    return true;
}

static bool mp3_seek_by_seconds(int delta_seconds)
{
    size_t current = mp3_current_seconds();
    ssize_t target = (ssize_t)current + delta_seconds;
    if (target < 0)
        target = 0;

    RG_LOGI("seek_by_seconds: delta=%d current=%us target=%us", delta_seconds, (unsigned int)current, (unsigned int)target);
    bool ok = mp3_seek_to_time((size_t)target);
    RG_LOGI("seek_by_seconds: result=%d", ok);
    return ok;
}

static void draw_progress_bar(size_t y)
{
    uint32_t elapsed = (uint32_t)mp3_current_seconds();
    uint32_t total = mp3_total_seconds();
    char buffer[64];

    if (total > 0)
    {
        unsigned int elapsed_m = (unsigned int)(elapsed / 60);
        unsigned int elapsed_s = (unsigned int)(elapsed % 60);
        unsigned int total_m = (unsigned int)(total / 60);
        unsigned int total_s = (unsigned int)(total % 60);
        snprintf(buffer, sizeof(buffer), "%02u:%02u / %02u:%02u", elapsed_m, elapsed_s, total_m, total_s);
    }
    else
    {
        format_time(buffer, sizeof(buffer), elapsed);
    }

    rg_gui_draw_text(RG_GUI_CENTER, y, 0, buffer, C_WHITE, C_BLACK, RG_TEXT_ALIGN_CENTER);

    int bar_width = rg_display_get_width() - 40;
    int x = 20;
    int height = 10;
    int fill_width = 0;
    if (total > 0)
        fill_width = (int)((uint64_t)bar_width * elapsed / total);
    else if (playback_frames > 0)
        fill_width = bar_width;

    fill_width = RG_MAX(0, RG_MIN(fill_width, bar_width));
    rg_gui_draw_rect(x, y + 14, bar_width, height, 1, C_WHITE, C_BLACK);
    if (fill_width > 0)
        rg_gui_draw_rect(x + 1, y + 15, fill_width - 2, height - 2, 0, 0, C_GREEN);
}

/* Glyph widths of a string (no padding), matching rg_gui_draw_text's default font. */
static int line_width(const char *s)
{
    int w = 0;
    for (const char *p = s; *p;)
        w += rg_gui_measure_char(rg_utf8_decode(&p));
    return w;
}

/* Find the longest UTF-8-safe prefix of `v` whose glyph width fits in `avail`
 * and write it to `buffer` after `label`, appending an ellipsis. */
static void fit_value(char *buffer, size_t size, const char *label, const char *v, int avail)
{
    size_t prefix_len = 0;
    int w = 0;
    for (const char *p = v; *p;)
    {
        const char *next = p;
        int cw = rg_gui_measure_char(rg_utf8_decode(&next));
        if (w + cw > avail)
            break;
        w += cw;
        p = next;
        prefix_len = (size_t)(p - v);
    }
    if (prefix_len == 0)
        snprintf(buffer, size, "%s...", label);
    else
        snprintf(buffer, size, "%s%.*s...", label, (int)prefix_len, v);
}

static void format_title(char *buffer, size_t size)
{
    const char *base = rg_basename(current_file[0] ? current_file : "No file");
    const int max_width = rg_display_get_width() - 8;
    const int padding = 2; /* Matches rg_gui_draw_text's default padding. */

    snprintf(buffer, size, "MP3: %s", base);
    if (line_width(buffer) + padding <= max_width)
        return;

    /* The filename is too long to fit on one line. Shorten the basename
     * (without splitting UTF-8 sequences) and add an ellipsis so the title
     * is always centered and never overflows the screen. */
    fit_value(buffer, size, "MP3: ", base, max_width - padding - line_width("MP3: ") - line_width("..."));
}

/* Build "Label: value" for one tag field, truncating the value (without
 * splitting UTF-8 sequences) so the line always fits on screen. */
static void format_tag_line(char *buffer, size_t size, const char *label, const char *value)
{
    const char *v = (value && *value) ? value : "-";
    const int max_width = rg_display_get_width() - 8;
    const int padding = 2;

    snprintf(buffer, size, "%s%s", label, v);
    if (line_width(buffer) + padding <= max_width)
        return;

    fit_value(buffer, size, label, v, max_width - padding - line_width(label) - line_width("..."));
}

/* Draw the battery state-of-charge icon right-aligned at the far edge of the
 * line whose top is at `y`, using the same style as the launcher status bar. */
static void draw_battery_icon(int y)
{
    rg_battery_t battery = rg_input_read_battery();
    if (!battery.present)
        return;

    rg_rect_t txt = TEXT_RECT("00:00", 0);
    int bar_height = txt.height;
    int icon_height = RG_MAX(8, bar_height - 4);
    int icon_top = y + RG_MAX(0, (bar_height - icon_height - 1) / 2);

    int width = 16;
    int width_fill = RG_MAX(0, RG_MIN((int)(width / 100.f * battery.level), width));
    int right = 22;
    int x_pos = -right;
    int y_pos = icon_top;

    rg_color_t color_fill = (battery.level > 20 ? (battery.level > 40 ? C_FOREST_GREEN : C_ORANGE) : C_RED);
    rg_color_t color_border = C_SILVER;

    rg_gui_draw_rect(x_pos, y_pos, width + 2, icon_height, 1, color_border, C_NONE);
    rg_gui_draw_rect(x_pos + width + 2, y_pos + 2, 2, icon_height - 4, 1, color_border, C_NONE);
    if (width_fill > 0)
        rg_gui_draw_rect(x_pos + 1, y_pos + 1, width_fill, icon_height - 2, 0, 0, color_fill);
    if (width - width_fill > 0)
        rg_gui_draw_rect(x_pos + 1 + width_fill, y_pos + 1, width - width_fill, icon_height - 2, 0, 0, C_BLACK);
}

static bool draw_state(void)
{
    char buffer[128];
    const char *driver = rg_audio_get_driver();

    while (rg_display_is_busy())
        rg_task_yield();

    rg_gui_set_surface(surface);
    rg_surface_fill(surface, NULL, C_BLACK);

if (playing)
{
    snprintf(buffer, sizeof(buffer), "  Playing: %dkbps/%dHz/%s Vol: %d%%  ", mp3_current_bitrate / 1000, sample_rate, driver ?driver : "Unknown", rg_audio_get_volume());
    rg_gui_draw_text(RG_GUI_CENTER, 16, 0, buffer, C_INDIGO, C_YELLOW_GREEN, RG_TEXT_ALIGN_CENTER);
}
else
{
    snprintf(buffer, sizeof(buffer), "  PAUSED:  press B to continue   Vol: %d%%  ", rg_audio_get_volume());
    rg_gui_draw_text(RG_GUI_CENTER, 16, 0, buffer, C_INDIGO, C_LIGHT_CORAL, RG_TEXT_ALIGN_CENTER);
}
    draw_battery_icon(16);
    format_title(buffer, sizeof(buffer));
    rg_gui_draw_text(RG_GUI_CENTER, 34, 0, buffer, C_WHITE, C_BLACK, RG_TEXT_ALIGN_CENTER);

// Tag info

    format_tag_line(buffer, sizeof(buffer), "Artist: ", tag_artist);
    rg_gui_draw_text(RG_GUI_LEFT, 64, 0, buffer, C_LIGHT_CORAL, C_BLACK, RG_TEXT_ALIGN_RIGHT);
    format_tag_line(buffer, sizeof(buffer), "Title:  ", tag_title);
    rg_gui_draw_text(RG_GUI_LEFT, 80, 0, buffer, C_POWDER_BLUE, C_BLACK, RG_TEXT_ALIGN_CENTER);
    format_tag_line(buffer, sizeof(buffer), "Album:  ", tag_album);
    rg_gui_draw_text(RG_GUI_LEFT, 96, 0, buffer, C_LIGHT_YELLOW, C_BLACK, RG_TEXT_ALIGN_LEFT);
    format_tag_line(buffer, sizeof(buffer), "Year:   ", tag_year);
    rg_gui_draw_text(RG_GUI_LEFT, 112, 0, buffer, C_LIME_GREEN, C_BLACK, RG_TEXT_ALIGN_LEFT);
    format_tag_line(buffer, sizeof(buffer), "Genre:  ", tag_genre);
    rg_gui_draw_text(RG_GUI_LEFT, 128, 0, buffer, C_YELLOW_GREEN, C_BLACK, RG_TEXT_ALIGN_LEFT);




    draw_progress_bar(158);

    if (playlist_count > 0 && playlist_index >= 0)
        snprintf(buffer, sizeof(buffer), "Track: %d/%d %s%s", playlist_index + 1, playlist_count, random_mode ? "Random: ON" : "Random: OFF", repeat_mode ? " Repeat: ON" : "");
    else
        snprintf(buffer, sizeof(buffer), "Track: -");
    rg_gui_draw_text(RG_GUI_CENTER, 182, 0, buffer, C_WHITE, C_BLACK, RG_TEXT_ALIGN_CENTER);
    snprintf(buffer, sizeof(buffer), "A=Play/Pause B=Repeat Y=Choose MENU=Exit");
    rg_gui_draw_text(RG_GUI_CENTER, 204, 0, buffer, C_SILVER, C_BLACK, RG_TEXT_ALIGN_CENTER);
    snprintf(buffer, sizeof(buffer), "UP/DOWN=Track L/R=Seek 5s X=Random");
    rg_gui_draw_text(RG_GUI_CENTER, 218, 0, buffer, C_SILVER, C_BLACK, RG_TEXT_ALIGN_CENTER);
    rg_gui_set_surface(NULL);
    rg_display_submit(surface, 0);

    return true;
}

static int playlist_scandir_cb(const rg_scandir_t *entry, void *arg)
{
    if (!entry->is_file || !is_mp3_file(entry->path))
        return RG_SCANDIR_CONTINUE;

    if (playlist_count >= playlist_capacity)
    {
        int new_capacity = playlist_capacity ? playlist_capacity * 2 : PLAYLIST_INITIAL_CAPACITY;
        char (*new_list)[RG_PATH_MAX + 1] = realloc(playlist, (size_t)new_capacity * (RG_PATH_MAX + 1));
        if (!new_list)
        {
            RG_LOGW("playlist: realloc to %d failed, keeping %d track(s)", new_capacity, playlist_count);
            return RG_SCANDIR_STOP;
        }
        playlist = new_list;
        playlist_capacity = new_capacity;
    }

    strncpy(playlist[playlist_count], entry->path, RG_PATH_MAX);
    playlist[playlist_count][RG_PATH_MAX] = '\0';
    playlist_count++;
    return RG_SCANDIR_CONTINUE;
}

static bool load_playlist(void)
{
    playlist_count = 0;
    playlist_index = -1;
    rg_storage_scandir(MUSIC_PATH, playlist_scandir_cb, NULL, RG_SCANDIR_FILES | RG_SCANDIR_RECURSIVE | RG_SCANDIR_SORT);
    RG_LOGI("load_playlist: %d track(s) in %s", playlist_count, MUSIC_PATH);
    return playlist_count > 0;
}

static void set_playlist_index(const char *path)
{
    playlist_index = -1;
    if (!path || !*path)
        return;
    for (int i = 0; i < playlist_count; i++)
    {
        if (strcmp(playlist[i], path) == 0)
        {
            playlist_index = i;
            break;
        }
    }
}

static bool play_track(int index)
{
    if (index < 0 || index >= playlist_count)
        return false;

    if (playlist_index == index && current_file[0])
        return true;

    if (!open_mp3(playlist[index]))
    {
        RG_LOGE("play_track: failed to open %s", playlist[index]);
        return false;
    }
    playlist_index = index;
    playing = true;
    draw_state();
    save_session_request();
    RG_LOGI("play_track: now playing %d/%d: %s", playlist_index + 1, playlist_count, rg_basename(playlist[playlist_index]));
    return true;
}

static void save_session_worker(void *arg)
{
    for (;;)
    {
        rg_task_delay(10);
        if (!session_dirty || session_saving)
            continue;

        session_saving = true;
        session_dirty = false;

        int64_t t0 = rg_system_timer();
        RG_LOGI("session-save: writing state track=%s byte=%u",
                rg_basename(session_track), (unsigned int)session_byte);

        rg_settings_set_string(NS_APP, "lastTrack", session_track);
        rg_settings_set_number(NS_APP, "lastByte", (double)session_byte);
        rg_settings_set_boolean(NS_APP, "lastRandom", session_random);
        rg_settings_set_boolean(NS_APP, "lastRepeat", session_repeat);
        rg_settings_commit();

        int64_t elapsed_us = rg_system_timer() - t0;
        RG_LOGI("session-save: committed in %u ms%s", (unsigned int)(elapsed_us / 1000),
                session_dirty ? " (update queued)" : "");
        session_saving = false;
    }
}

/* Snapshot the current session and let the background task write it to disk,
 * so the MP3 decode loop is never blocked by SD/config I/O. */
static void save_session_request(void)
{
    if (!current_file[0] || !mp3_input_ptr || session_saving)
        return;

    uint64_t current_byte = (uint64_t)mp3_stream_offset + (uint64_t)(mp3_input_ptr - mp3_stream);
    if (current_byte > mp3_file_size)
        current_byte = mp3_file_size;

    RG_LOGI("save_session_request: %s byte=%u", rg_basename(current_file), (unsigned int)current_byte);
    strncpy(session_track, current_file, RG_PATH_MAX);
    session_track[RG_PATH_MAX] = '\0';
    session_byte = current_byte;
    session_random = random_mode;
    session_repeat = repeat_mode;
    session_dirty = true;
}

/* Wait for any in-flight save to finish (used at shutdown). */
static void save_session_flush(void)
{
    int64_t deadline = rg_system_timer() + 3000000;
    while (session_dirty || session_saving)
    {
        if (rg_system_timer() >= deadline)
            break;
        rg_task_delay(10);
    }
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_SHUTDOWN)
    {
        save_session_request();
        save_session_flush();
    }
}

static bool play_random_track(void)
{
    if (playlist_count <= 0)
        return false;
    if (playlist_count == 1)
        return play_track(0);
    int index = playlist_index;
    for (int attempt = 0; attempt < 8; attempt++)
    {
        int candidate = esp_random() % playlist_count;
        if (candidate != playlist_index)
        {
            index = candidate;
            break;
        }
    }
    if (index == playlist_index)
        index = (playlist_index + 1) % playlist_count;
    return play_track(index);
}

static void handle_track_finished(void)
{
    playing = false;
    if (playlist_count > 0)
    {
        if (repeat_mode && playlist_index >= 0)
        {
            RG_LOGI("handle_track_finished: repeat mode, replaying %d/%d", playlist_index + 1, playlist_count);
            if (open_mp3(playlist[playlist_index]))
            {
                playing = true;
                draw_state();
            }
            else
            {
                draw_text_centered(100, "Playback finished");
                rg_task_delay(500);
                draw_state();
            }
            return;
        }
        int index = playlist_index + 1 >= playlist_count ? 0 : playlist_index + 1;
        if (random_mode)
        {
            RG_LOGI("handle_track_finished: random mode, picking a random track");
            if (!play_random_track())
            {
                draw_text_centered(100, "Playback finished");
                rg_task_delay(500);
                draw_state();
            }
            return;
        }
        RG_LOGI("handle_track_finished: advancing to %d/%d", index + 1, playlist_count);
        if (!play_track(index))
        {
            draw_text_centered(100, "Playback finished");
            rg_task_delay(500);
            draw_state();
        }
    }
    else
    {
        draw_text_centered(100, "Playback finished");
        rg_task_delay(500);
        draw_state();
    }
}

static void picker_task(void *arg)
{
    RG_LOGI("picker_task: opening file picker");
    char *filename = rg_gui_file_picker("Select MP3", MUSIC_PATH, is_mp3_file, true, true);
    picker_result = filename;
    picker_done = true;
    RG_LOGI("picker_task: done, result=%p", (void *)filename);
}

static void play_loop(void)
{
    uint32_t prev_keys = 0;

    int redraw_ticks = 0;
    while (true)
    {
        uint32_t keys = rg_input_read_gamepad();

        /* If the file picker task just finished, consume its result */
        if (picker_done)
        {
            picker_done = false;
            picker_active = false;
            char *filename = picker_result;
            picker_result = NULL;

            if (filename && *filename)
            {
                if (open_mp3(filename))
                {
                    set_playlist_index(filename);
                    playing = true;
                    save_session_request();
                }
                else
                {
                    rg_gui_alert("Error", "Failed to open MP3 file");
                }
            }
            free(filename);

            if (advance_pending)
            {
                advance_pending = false;
                handle_track_finished();
            }
            else
            {
                draw_state();
            }
        }

        if (!picker_active)
        {
            if ((keys & RG_KEY_MENU) && !(prev_keys & RG_KEY_MENU))
            {
                RG_LOGI("play_loop: MENU pressed");
                save_session_request();
                rg_system_exit();
            }

            if ((keys & RG_KEY_A) && !(prev_keys & RG_KEY_A))
            {
                RG_LOGI("play_loop: A pressed");
                playing = !playing;
                if (!playing)
                    rg_audio_set_mute(true);
                else
                    rg_audio_set_mute(false);
                draw_state();
            }

            if ((keys & RG_KEY_B) && !(prev_keys & RG_KEY_B))
            {
                RG_LOGI("play_loop: B pressed, repeat mode %s", repeat_mode ? "OFF" : "ON");
                repeat_mode = !repeat_mode;
                draw_state();
            }

            if ((keys & RG_KEY_Y) && !(prev_keys & RG_KEY_Y))
            {
                RG_LOGI("play_loop: Y pressed, opening file picker");
                load_playlist();
                picker_active = true;
                picker_done = false;
                picker_result = NULL;
                if (!rg_task_create("filepicker", picker_task, NULL, 8192, 0, RG_TASK_PRIORITY_1, -1))
                {
                    RG_LOGE("play_loop: failed to create file picker task");
                    picker_active = false;
                }
            }

            if ((keys & RG_KEY_X) && !(prev_keys & RG_KEY_X))
            {
                random_mode = !random_mode;
                RG_LOGI("play_loop: X pressed, random mode %s", random_mode ? "ON" : "OFF");
                if (random_mode)
                    play_random_track();
                else
                    draw_state();
            }

            if ((keys & RG_KEY_UP) && !(prev_keys & RG_KEY_UP))
            {
                if (random_mode)
                {
                    RG_LOGI("play_loop: UP pressed, random track");
                    play_random_track();
                }
                else
                {
                    RG_LOGI("play_loop: UP pressed, next track");
                    if (playlist_count > 0)
                    {
                        int index = playlist_index + 1 >= playlist_count ? 0 : playlist_index + 1;
                        play_track(index);
                    }
                }
            }

            if ((keys & RG_KEY_DOWN) && !(prev_keys & RG_KEY_DOWN))
            {
                if (random_mode)
                {
                    RG_LOGI("play_loop: DOWN pressed, random track");
                    play_random_track();
                }
                else
                {
                    RG_LOGI("play_loop: DOWN pressed, previous track");
                    if (playlist_count > 0)
                    {
                        int index = playlist_index <= 0 ? playlist_count - 1 : playlist_index - 1;
                        play_track(index);
                    }
                }
            }

            if ((keys & RG_KEY_LEFT) && !(prev_keys & RG_KEY_LEFT))
            {
                RG_LOGI("play_loop: LEFT pressed, seeking -5s");
                if (mp3_seek_by_seconds(-5))
                    draw_state();
            }

            if ((keys & RG_KEY_RIGHT) && !(prev_keys & RG_KEY_RIGHT))
            {
                RG_LOGI("play_loop: RIGHT pressed, seeking +5s");
                if (mp3_seek_by_seconds(5))
                    draw_state();
            }
        }

        if (playing)
        {
            if (!decode_and_play())
            {
                RG_LOGE("play_loop: decode_and_play returned false, playback ended");
                if (picker_active)
                {
                    /* Keep the audio running until the picker closes */
                    advance_pending = true;
                    playing = false;
                }
                else
                {
                    handle_track_finished();
                }
            }
            else
            {
                if (++redraw_ticks >= 60)
                {
                    if (!picker_active)
                    {
                        draw_state();
                        redraw_ticks = 0;
                    }
                }
            }
        }

        int64_t now_ms = rg_system_timer() / 1000;
        if (now_ms - last_session_save_ms >= 30000)
        {
            last_session_save_ms = now_ms;
            save_session_request();
        }

        prev_keys = keys;
        rg_task_yield();
    }
}

void app_main(void)
{
    app = rg_system_init(&(const rg_config_t){
        .sampleRate = AUDIO_SAMPLE_RATE,
        .frameRate = 0,
        .storageRequired = true,
        .romRequired = false,
        .handlers = {
            .event = &event_handler,
        },
    });
    app->configNs = "mp3-player";
    sample_rate = AUDIO_SAMPLE_RATE;

    surface = rg_surface_create(rg_display_get_width(), rg_display_get_height(), RG_PIXEL_565_LE, MEM_SLOW);

    load_playlist();

    if (!rg_task_create("session-save", save_session_worker, NULL, 6144, 0, RG_TASK_PRIORITY_1, -1))
        RG_LOGE("app_main: failed to create session save task");

    const char *path = app->romPath && *app->romPath ? app->romPath : NULL;
    char *last_track = rg_settings_get_string(NS_APP, "lastTrack", NULL);
    double last_byte = rg_settings_get_number(NS_APP, "lastByte", 0);

    /* Always resume the last played song on startup so the player continues
     * where it left off after a power cycle, regardless of what file the
     * launcher passed as romPath. The in-app picker (Y) is used to switch. */
    bool resume_launch = last_track && *last_track && is_mp3_file(last_track);
    if (resume_launch && strncmp(last_track, MUSIC_PATH, strlen(MUSIC_PATH)) != 0)
    {
        /* The saved track points outside the current music directory (eg. the
         * path was changed between builds); drop the stale session. */
        RG_LOGW("app_main: ignoring stale session track %s (outside %s)", last_track, MUSIC_PATH);
        resume_launch = false;
    }
    if (resume_launch)
    {
        random_mode = rg_settings_get_boolean(NS_APP, "lastRandom", false);
        repeat_mode = rg_settings_get_boolean(NS_APP, "lastRepeat", false);
        RG_LOGI("app_main: resuming %s byte=%u", last_track, (unsigned int)last_byte);
        if (open_mp3(last_track))
        {
            set_playlist_index(last_track);
            if (last_byte > 0)
                seek_mp3_to_byte((size_t)last_byte);
            playing = true;
            draw_state();
            play_loop();
        }
    }
    free(last_track);

    if (!playing && path && is_mp3_file(path))
    {
        if (open_mp3(path))
        {
            set_playlist_index(path);
            playing = true;
            draw_state();
            play_loop();
        }
    }

    if (!playing)
    {
        draw_text_centered(120, "Select an MP3 file");
        char *filename = rg_gui_file_picker("Select MP3", MUSIC_PATH, is_mp3_file, true, true);
        if (!filename || !*filename)
            rg_system_exit();

        if (!open_mp3(filename))
        {
            rg_gui_alert("Error", "Failed to open MP3 file");
            free(filename);
            rg_system_exit();
        }
        set_playlist_index(filename);
        free(filename);

        playing = true;
        draw_state();
        play_loop();
    }
}
