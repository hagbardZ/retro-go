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
#define MAX_TRACKS 256
#define MUSIC_PATH RG_BASE_PATH_ROMS "/music"

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
static uint64_t playback_frames;
static size_t mp3_file_size;
static size_t mp3_id3_skip_bytes;
static int mp3_bitrate_bps;
static bool mp3_has_duration;
static int mp3_skip_frames;
static uint64_t mp3_resume_byte;
static int64_t last_session_save_ms;

static char session_track[RG_PATH_MAX + 1];
static uint64_t session_byte;
static bool session_random;
static bool session_repeat;
static volatile bool session_dirty;
static volatile bool session_saving;

static char playlist[MAX_TRACKS][RG_PATH_MAX + 1];
static int playlist_count = 0;
static int playlist_index = -1;
static bool random_mode = false;
static bool repeat_mode = false;

static volatile bool picker_active = false;
static volatile bool picker_done = false;
static char *picker_result = NULL;
static bool advance_pending = false;

static void save_session_request(void);

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
    static int64_t last_refill_warn_ms = 0;
    int64_t t0 = rg_system_timer();
    size_t read_bytes = fread(mp3_stream + mp3_input_left, 1, read_size, mp3_file);
    int64_t read_us = rg_system_timer() - t0;
    if (read_us > 8000)
    {
        int64_t now_ms = rg_system_timer() / 1000;
        if (now_ms - last_refill_warn_ms >= 1000)
        {
            last_refill_warn_ms = now_ms;
            RG_LOGW("refill: SD read blocked %u ms (%u/%u bytes) offset=%u in_left=%u",
                    (unsigned int)(read_us / 1000), (unsigned int)read_bytes, (unsigned int)read_size,
                    (unsigned int)mp3_stream_offset, (unsigned int)mp3_input_left);
        }
    }
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
            size_t remaining = mp3_stream_fill - skip_bytes;
            memmove(mp3_stream, mp3_stream + skip_bytes, remaining);
            mp3_stream_fill = remaining;
            /* file pointer already lies after the initial read, no seek needed */
        }
        else
        {
            fseek(mp3_file, (long)(skip_bytes - mp3_stream_fill), SEEK_CUR);
            mp3_stream_fill = 0;
        }
        mp3_input_ptr = mp3_stream;
        mp3_input_left = mp3_stream_fill;
        mp3_stream_eof = false;
    }

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
    mp3_bitrate_bps = 0;
    mp3_has_duration = false;
    mp3_skip_frames = 0;

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

    while (mp3_input_left < 2048 && !mp3_stream_eof)
    {
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
        if (info.samprate > 0 && info.samprate != sample_rate)
        {
            RG_LOGI("decode_and_play: sample rate %d -> %d", sample_rate, info.samprate);
            sample_rate = info.samprate;
            rg_audio_set_sample_rate(sample_rate);
        }

        if (!mp3_bitrate_bps && info.bitrate > 0)
        {
            mp3_bitrate_bps = info.bitrate;
            if (mp3_resume_byte > 0)
            {
                uint64_t resume_byte = mp3_resume_byte;
                mp3_resume_byte = 0;
                if (resume_byte > mp3_id3_skip_bytes)
                    resume_byte -= mp3_id3_skip_bytes;
                else
                    resume_byte = 0;
                playback_frames = (resume_byte * 8 / (uint64_t)info.bitrate) * (uint64_t)info.samprate;
            }
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
    if (mp3_bitrate_bps <= 0 || mp3_file_size <= mp3_id3_skip_bytes)
        return 0;

    uint64_t data_bytes = (uint64_t)mp3_file_size - mp3_id3_skip_bytes;
    uint64_t bitrate_bps = (uint64_t)mp3_bitrate_bps;
    mp3_has_duration = true;
    return (size_t)((data_bytes * 8 + bitrate_bps - 1) / bitrate_bps);
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
    return true;
}

static bool mp3_seek_to_time(size_t seek_seconds)
{
    size_t total = mp3_total_seconds();
    if (total == 0 || mp3_file_size == 0)
    {
        RG_LOGE("seek_to_time: FAIL total=%u filesize=%u bitrate=%d", (unsigned int)total, (unsigned int)mp3_file_size, mp3_bitrate_bps);
        return false;
    }

    if (seek_seconds > total)
        seek_seconds = total;

    uint64_t data_bytes = (uint64_t)mp3_file_size - mp3_id3_skip_bytes;
    uint64_t target_byte = mp3_id3_skip_bytes + (data_bytes * seek_seconds) / total;
    RG_LOGI("seek_to_time: want=%us total=%us target_byte=%u", (unsigned int)seek_seconds, (unsigned int)total, (unsigned int)target_byte);
    if (!seek_mp3_to_byte((size_t)target_byte))
    {
        RG_LOGE("seek_to_time: seek_mp3_to_byte FAILED at byte %u", (unsigned int)target_byte);
        return false;
    }

    /* Anchor the playhead to the byte position where playback actually resumes
     * so the elapsed time and subsequent relative seeks stay accurate (esp. VBR). */
    uint64_t resume_bytes = (uint64_t)mp3_stream_offset + (uint64_t)(mp3_input_ptr - mp3_stream);
    if (resume_bytes > mp3_id3_skip_bytes)
        resume_bytes -= mp3_id3_skip_bytes;
    else
        resume_bytes = 0;

    uint64_t bitrate_bps = (uint64_t)mp3_bitrate_bps;
    uint64_t rate = (sample_rate > 0) ? (uint64_t)sample_rate : AUDIO_SAMPLE_RATE;
    uint64_t resume_seconds = bitrate_bps > 0 ? (resume_bytes * 8) / bitrate_bps : seek_seconds;
    playback_frames = resume_seconds * rate;
    RG_LOGI("seek_to_time: OK resume_byte=%u resume_sec=%u rate=%u", (unsigned int)resume_bytes, (unsigned int)resume_seconds, (unsigned int)rate);
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

static void format_title(char *buffer, size_t size)
{
    const char *base = rg_basename(current_file[0] ? current_file : "No file");
    const int max_width = rg_display_get_width() - 8;

    snprintf(buffer, size, "MP3: %s", base);
    if (TEXT_RECT(buffer, 0).width <= max_width)
        return;

    /* The filename is too long to fit on one line. Shorten the basename
     * (without splitting UTF-8 sequences) and add an ellipsis so the title
     * is always centered and never overflows the screen. */
    size_t base_len = strlen(base);
    while (base_len > 0)
    {
        base_len--;
        while (base_len > 0 && ((unsigned char)base[base_len] & 0xC0) == 0x80)
            base_len--;
        snprintf(buffer, size, "MP3: %.*s...", (int)base_len, base);
        if (TEXT_RECT(buffer, 0).width <= max_width)
            return;
    }
    snprintf(buffer, size, "MP3: ...");
}

static bool draw_state(void)
{
    char buffer[128];
    const char *driver = rg_audio_get_driver();

    while (rg_display_is_busy())
        rg_task_yield();

    rg_gui_set_surface(surface);
    rg_surface_fill(surface, NULL, C_BLACK);
    format_title(buffer, sizeof(buffer));
    rg_gui_draw_text(RG_GUI_CENTER, 16, 0, buffer, C_WHITE, C_BLACK, RG_TEXT_ALIGN_CENTER);
    snprintf(buffer, sizeof(buffer), "Playback: %s", playing ? "Playing" : "Stopped");
    rg_gui_draw_text(RG_GUI_CENTER, 44, 0, buffer, C_WHITE, C_BLACK, RG_TEXT_ALIGN_CENTER);
    snprintf(buffer, sizeof(buffer), "Audio: %dHz/%s", sample_rate, driver ?driver : "Unknown");
    rg_gui_draw_text(RG_GUI_CENTER, 72, 0, buffer, C_WHITE, C_BLACK, RG_TEXT_ALIGN_CENTER);
    draw_progress_bar(110);
    if (playlist_count > 0 && playlist_index >= 0)
        snprintf(buffer, sizeof(buffer), "Track: %d/%d %s%s", playlist_index + 1, playlist_count, random_mode ? "Random: ON" : "Random: OFF", repeat_mode ? " Repeat: ON" : "");
    else
        snprintf(buffer, sizeof(buffer), "Track: -");
    rg_gui_draw_text(RG_GUI_CENTER, 138, 0, buffer, C_WHITE, C_BLACK, RG_TEXT_ALIGN_CENTER);
    snprintf(buffer, sizeof(buffer), "A=Play/Pause B=Repeat Y=Choose MENU=Exit");
    rg_gui_draw_text(RG_GUI_CENTER, 166, 0, buffer, C_SILVER, C_BLACK, RG_TEXT_ALIGN_CENTER);
    snprintf(buffer, sizeof(buffer), "UP/DOWN=Track L/R=Seek 5s X=Random");
    rg_gui_draw_text(RG_GUI_CENTER, 186, 0, buffer, C_SILVER, C_BLACK, RG_TEXT_ALIGN_CENTER);
    rg_gui_set_surface(NULL);
    rg_display_submit(surface, 0);
    return true;
}

static int playlist_scandir_cb(const rg_scandir_t *entry, void *arg)
{
    if (entry->is_file && playlist_count < MAX_TRACKS && is_mp3_file(entry->path))
    {
        strncpy(playlist[playlist_count], entry->path, RG_PATH_MAX);
        playlist[playlist_count][RG_PATH_MAX] = '\0';
        playlist_count++;
    }
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
    if (resume_launch)
    {
        random_mode = rg_settings_get_boolean(NS_APP, "lastRandom", false);
        repeat_mode = rg_settings_get_boolean(NS_APP, "lastRepeat", false);
        RG_LOGI("app_main: resuming %s byte=%u", last_track, (unsigned int)last_byte);
        if (open_mp3(last_track))
        {
            set_playlist_index(last_track);
            if (last_byte > 0 && seek_mp3_to_byte((size_t)last_byte))
                mp3_resume_byte = (uint64_t)mp3_stream_offset + (uint64_t)(mp3_input_ptr - mp3_stream);
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
