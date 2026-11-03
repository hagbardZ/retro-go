/*
 * Amiga for Retro-Go.
 *
 * Retro-Go glue around the platform-independent Amiga core from
 * rlucencic/esp32ami (components/amiga).
 */
#include <rg_system.h>
#include <amiga.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUTPUT_WIDTH  320
#define OUTPUT_HEIGHT 240
#define AUDIO_CHUNK   512

/* The Amiga PAL picture is 720x568 (284 beam lines, each doubled). The
 * standard 256-line display sits 15 lines into that window; it is scaled
 * down to 240 lines. The 720 hires pixels are halved to 360 lores pixels
 * and centre-cropped to 320.
 */
#define SRC_TOP_LINE   15
#define SRC_LINES      256
#define SRC_CROP_X     40

static rg_app_t *app;
static rg_surface_t *surface;
static amiga_config_t machine;
static uint8_t *kickstart_image;
static uint32_t kickstart_size;
static char kickstart_path[RG_PATH_MAX + 1];
static uint8_t *extension_image;
static bool initialized;
static char g_scan_dir[RG_PATH_MAX + 1];
static bool disk_pick_requested;
static uint32_t last_buttons;
static uint8_t pending_key;
static int pending_key_frames;

static const char *SETTING_MODEL = "model";

static const char *const model_names[] = {
    "A500", "A2000", "A600", "A1200", NULL
};

static void *psram_alloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!ptr)
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    return ptr;
}

static bool file_exists(const char *path)
{
    return rg_storage_exists(path);
}

static bool load_valid_kickstart(const char *path)
{
    void *data = NULL;
    size_t size = 0;
    int major = 0, minor = 0;

    if (!rg_storage_read_file(path, &data, &size, 0) || !data || !size)
        return false;

    if (!amiga_kickstart_version((const uint8_t *)data, (uint32_t)size, &major, &minor)) {
        free(data);
        return false;
    }

    free(kickstart_image);
    kickstart_image = data;
    kickstart_size = (uint32_t)size;
    snprintf(kickstart_path, sizeof(kickstart_path), "%s", path);
    RG_LOGI("Kickstart %d.%d from %s (%u bytes)", major, minor, path,
            (unsigned)kickstart_size);
    return true;
}

typedef struct {
    char path[2 * RG_PATH_MAX + 1];
    int score;
} rom_candidate_t;

static int rom_scan_cb(const rg_scandir_t *file, void *arg)
{
    rom_candidate_t *best = arg;
    const char *name = file->basename;

    if (!file->is_file || !name)
        return RG_SCANDIR_CONTINUE;
    if (!rg_extension_match(name, "rom bin"))
        return RG_SCANDIR_CONTINUE;
    if (strstr(name, "-ext") || strstr(name, "ext.rom"))
        return RG_SCANDIR_CONTINUE;

    int score = 0;
    if (strstr(name, "kickstart")) score += 4;
    if (strstr(name, "kick13")) score += 4;
    if (strstr(name, "amiga-os-130")) score += 5;
    if (strstr(name, "amiga-os")) score += 3;
    if (strstr(name, "aros")) score += 2;
    if (!score)
        return RG_SCANDIR_CONTINUE;

    if (score > best->score) {
        snprintf(best->path, sizeof(best->path), "%s/%s", g_scan_dir, name);
        best->score = score;
    }
    return RG_SCANDIR_CONTINUE;
}

static bool find_kickstart_in(const char *dir)
{
    static const char *const candidates[] = {
        "kickstart.rom", "kick13.rom", "kickstart13.rom",
        "amiga-os-120.rom", "amiga-os-130.rom", "amiga-os-204.rom",
        "amiga-os-205-a600.rom", "amiga-os-300-a1200.rom",
        "amiga-os-310-a600.rom", "amiga-os-310-a1200.rom",
        "aros.rom", "aros-20250422.rom", NULL
    };
    char path[RG_PATH_MAX + 1];

    for (int i = 0; candidates[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, candidates[i]);
        if (file_exists(path) && load_valid_kickstart(path))
            return true;
    }

    rom_candidate_t best = {0};
    snprintf(g_scan_dir, sizeof(g_scan_dir), "%s", dir);
    rg_storage_scandir(dir, rom_scan_cb, &best, RG_SCANDIR_FILES);
    if (best.path[0] && load_valid_kickstart(best.path))
        return true;

    return false;
}

static bool find_kickstart(void)
{
    if (find_kickstart_in(RG_BASE_PATH_BIOS))
        return true;

    char dir[RG_PATH_MAX + 1];
    if (app && app->romPath) {
        snprintf(dir, sizeof(dir), "%s", rg_dirname(app->romPath));
        if (find_kickstart_in(dir))
            return true;
    } else {
        snprintf(dir, sizeof(dir), "%s", RG_BASE_PATH_ROMS "/amiga");
        if (find_kickstart_in(dir))
            return true;
    }

    return false;
}

/* AROS needs its extension ROM ($E00000-$E7FFFF). Derive it from the
 * Kickstart's name when present; other Kickstarts do not use one.
 */
static bool find_extension(const char *kickstart_path)
{
    char path[RG_PATH_MAX + 1];
    const char *dot = strrchr(kickstart_path, '.');

    if (!dot || !strstr(kickstart_path, "aros"))
        return false;

    snprintf(path, sizeof(path), "%.*s-ext.rom", (int)(dot - kickstart_path), kickstart_path);
    if (!file_exists(path)) {
        snprintf(path, sizeof(path), "%s/aros-20250422-ext.rom", RG_BASE_PATH_BIOS);
        if (!file_exists(path))
            return false;
    }

    void *data = NULL;
    size_t size = 0;
    if (!rg_storage_read_file(path, &data, &size, 0) || size != 512 * 1024) {
        free(data);
        RG_LOGW("AROS extension ROM unusable: %s", path);
        return false;
    }
    extension_image = data;
    RG_LOGI("AROS extension ROM from %s", path);
    return true;
}

static void apply_model(int model)
{
    if (model < AMIGA_A500) model = AMIGA_A500;
    if (model > AMIGA_A1200) model = AMIGA_A1200;

    machine.model = (amiga_model_t)model;
    machine.ntsc = false;
    machine.ecs = false;
    machine.chip_size = 512 * 1024;
    machine.slow_size = 512 * 1024;
    machine.fast_size = 0;

    switch (machine.model) {
    case AMIGA_A2000:
        machine.slow_size = 0;
        machine.fast_size = 1024 * 1024;
        break;
    case AMIGA_A600:
        machine.ecs = true;
        machine.chip_size = 1024 * 1024;
        machine.slow_size = 0;
        break;
    case AMIGA_A1200:
        machine.ecs = true;
        machine.chip_size = 2048 * 1024;
        machine.slow_size = 0;
        break;
    default:
        break;
    }
}

static bool machine_start(void)
{
    if (!find_kickstart()) {
        rg_gui_alert("Amiga", "No Kickstart or AROS ROM found in\n" RG_BASE_PATH_BIOS);
        return false;
    }

    find_extension(kickstart_path);

    memset(&machine, 0, sizeof(machine));
    apply_model(rg_settings_get_number(NS_APP, SETTING_MODEL, AMIGA_A500));

    machine.chip = psram_alloc(machine.chip_size);
    machine.slow = machine.slow_size ? psram_alloc(machine.slow_size) : NULL;
    machine.fast = machine.fast_size ? psram_alloc(machine.fast_size) : NULL;
    machine.rom = psram_alloc(kickstart_size);
    machine.kickstart = kickstart_image;
    machine.kickstart_size = kickstart_size;
    machine.extension = extension_image;
    machine.ext_rom = extension_image ? psram_alloc(512 * 1024) : NULL;
    machine.picture = psram_alloc(amiga_picture_size());

    if (!machine.chip || (machine.slow_size && !machine.slow) ||
        (machine.fast_size && !machine.fast) || !machine.rom || !machine.picture ||
        (extension_image && !machine.ext_rom)) {
        rg_gui_alert("Amiga", "Not enough memory to start the Amiga.");
        return false;
    }

    if (!amiga_init(&machine)) {
        rg_gui_alert("Amiga", "The Kickstart/AROS ROM was rejected.");
        return false;
    }

    initialized = true;
    return true;
}

static void machine_stop(void)
{
    if (initialized) {
        amiga_floppy_flush();
        initialized = false;
    }
    free(machine.chip);
    free(machine.slow);
    free(machine.fast);
    free(machine.rom);
    free(machine.ext_rom);
    free(machine.picture);
    memset(&machine, 0, sizeof(machine));
    free(kickstart_image);
    kickstart_image = NULL;
    free(extension_image);
    extension_image = NULL;
}

static void render_surface(void)
{
    uint16_t line[1440];

    amiga_screen_signature();

    const int width = amiga_screen_width();
    const int step = width >= 1440 ? 4 : 2;
    const int crop = SRC_CROP_X * (step / 2);

    for (int y = 0; y < OUTPUT_HEIGHT; y++) {
        const int src_row = SRC_TOP_LINE + (y * SRC_LINES) / OUTPUT_HEIGHT;
        uint16_t *dst = (uint16_t *)((uint8_t *)surface->data + y * surface->stride);

        amiga_render_line_rgb565(src_row * 2, line);
        for (int x = 0; x < OUTPUT_WIDTH; x++) {
            int sx = crop + x * step;
            dst[x] = (sx < width) ? line[sx] : 0;
        }
    }
}

static void audio_drain(void)
{
    int16_t buffer[AUDIO_CHUNK * 2];
    int frames;

    while ((frames = amiga_audio_render(buffer, AUDIO_CHUNK)) > 0)
        rg_audio_submit((const rg_audio_frame_t *)buffer, (size_t)frames);
}

static void audio_discard(void)
{
    int16_t buffer[AUDIO_CHUNK * 2];
    while (amiga_audio_render(buffer, AUDIO_CHUNK) > 0)
        ;
}

static const rg_keyboard_layout_t amiga_keyboard = {
    .layout = "1234567890"
              "qwertyuiop"
              "asdfghjkl "
              "zxcvbnm,./",
    .columns = 10,
    .rows = 4,
    .label = "Amiga",
};

static void deliver_typed_text(const char *text)
{
    amiga_type(text);
    for (int i = 0; i < 2000 && amiga_typing(); i++) {
        amiga_run_frame();
        audio_discard();
    }
    for (int i = 0; i < 2; i++) {
        amiga_run_frame();
        audio_discard();
    }

    while (rg_display_is_busy())
        rg_task_yield();
    render_surface();
    rg_display_submit(surface, 0);
}

static void virtual_keyboard_session(void)
{
    const int count = (int)(amiga_keyboard.columns * amiga_keyboard.rows);
    int cursor = 0;

    rg_audio_set_mute(true);

    while (true) {
        rg_gui_draw_virtual_keyboard(RG_GUI_CENTER, RG_GUI_BOTTOM,
                                     &amiga_keyboard, cursor, false);
        rg_input_wait_for_key(RG_KEY_ALL, false, 1000);

        const uint32_t buttons = rg_input_read_gamepad();
        int next = cursor;

        if (buttons & RG_KEY_B)
            break;
        if (buttons & RG_KEY_A) {
            char text[2] = {amiga_keyboard.layout[cursor], 0};
            deliver_typed_text(text);
            rg_task_delay(60);
            continue;
        }
        if (buttons & RG_KEY_START) {
            deliver_typed_text("\n");
            rg_task_delay(60);
            continue;
        }
        if (buttons & RG_KEY_SELECT) {
            deliver_typed_text(" ");
            rg_task_delay(60);
            continue;
        }

        if (buttons & RG_KEY_LEFT) --next;
        else if (buttons & RG_KEY_RIGHT) ++next;
        else if (buttons & RG_KEY_UP) next -= (int)amiga_keyboard.columns;
        else if (buttons & RG_KEY_DOWN) next += (int)amiga_keyboard.columns;

        if (next >= 0 && next < count)
            cursor = next;

        rg_system_tick(0);
    }

    audio_discard();
    rg_audio_set_mute(false);
    rg_input_wait_for_key(RG_KEY_ALL, false, 1000);
}

static bool load_state_handler(const char *filename)
{
    RG_LOGW("Save states are not supported yet: %s", filename);
    return false;
}

static bool save_state_handler(const char *filename)
{
    RG_LOGW("Save states are not supported yet: %s", filename);
    return false;
}

static bool reset_handler(bool hard)
{
    if (!initialized)
        return false;
    amiga_reset();
    return true;
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    if (!surface)
        return false;
    while (rg_display_is_busy())
        rg_task_yield();
    return rg_surface_save_image_file(surface, filename, width, height);
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW) {
        if (surface)
            rg_display_submit(surface, 0);
    } else if (event == RG_EVENT_SHUTDOWN) {
        rg_audio_set_mute(true);
        machine_stop();
    }
}

static rg_gui_event_t model_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int model = rg_settings_get_number(NS_APP, SETTING_MODEL, AMIGA_A500);

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        model += (event == RG_DIALOG_NEXT) ? 1 : -1;
        if (model < AMIGA_A500) model = AMIGA_A1200;
        if (model > AMIGA_A1200) model = AMIGA_A500;
        rg_settings_set_number(NS_APP, SETTING_MODEL, model);
    }
    if (model < AMIGA_A500) model = AMIGA_A500;
    if (model > AMIGA_A1200) model = AMIGA_A1200;
    snprintf(option->value, 32, "%s", model_names[model]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t disk_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
        disk_pick_requested = true;

    const char *path = amiga_floppy_path(0);
    snprintf(option->value, 32, "%s", path ? rg_basename(path) : "(empty)");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t eject_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
        amiga_floppy_eject(0);
    snprintf(option->value, 32, "%s", amiga_floppy_path(0) ? "Inserted" : "(empty)");
    return RG_DIALOG_VOID;
}

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, "Model (restart)", "-",
                                RG_DIALOG_FLAG_NORMAL, &model_update_cb};
    *dest++ = (rg_gui_option_t){0, "Disk 1", "-",
                                RG_DIALOG_FLAG_NORMAL, &disk_update_cb};
    *dest++ = (rg_gui_option_t){0, "Eject disk", "-",
                                RG_DIALOG_FLAG_NORMAL, &eject_update_cb};
    *dest = (rg_gui_option_t)RG_DIALOG_END;
}

static bool disk_validator(const char *path)
{
    return rg_extension_match(path, "adf rp9");
}

static void disk_picker(void)
{
    char start_path[RG_PATH_MAX + 1];
    const char *current = amiga_floppy_path(0);

    if (current) {
        snprintf(start_path, sizeof(start_path), "%s", rg_dirname(current));
    } else if (app->romPath) {
        snprintf(start_path, sizeof(start_path), "%s", rg_dirname(app->romPath));
    } else {
        snprintf(start_path, sizeof(start_path), "%s", RG_BASE_PATH_ROMS);
    }

    char *path = rg_gui_file_picker("Insert disk", start_path, &disk_validator, false, false);
    if (path) {
        if (!amiga_floppy_insert(0, path))
            rg_gui_alert("Amiga", "Unable to insert this floppy image.");
        free(path);
    }
}

static void queue_key(uint8_t code)
{
    pending_key = code;
    pending_key_frames = 4;
}

static uint8_t gamepad_to_joystick(uint32_t buttons)
{
    uint8_t joystick = 0;

    if (buttons & RG_KEY_UP)    joystick |= AMIGA_JOY_UP;
    if (buttons & RG_KEY_DOWN)  joystick |= AMIGA_JOY_DOWN;
    if (buttons & RG_KEY_LEFT)  joystick |= AMIGA_JOY_LEFT;
    if (buttons & RG_KEY_RIGHT) joystick |= AMIGA_JOY_RIGHT;
    if (buttons & (RG_KEY_A | RG_KEY_B)) joystick |= AMIGA_JOY_FIRE;
    if (buttons & (RG_KEY_X | RG_KEY_Y)) joystick |= AMIGA_JOY_FIRE2;

    return joystick;
}

static bool insert_content(const char *path)
{
    if (!path || !path[0])
        return true;

    if (rg_extension_match(path, "adf rp9")) {
        if (!amiga_floppy_insert(0, path)) {
            rg_gui_alert("Amiga", "Unable to insert this floppy image.");
            return false;
        }
        return true;
    }

    if (rg_extension_match(path, "hdf")) {
        if (!amiga_hd_insert(0, path)) {
            rg_gui_alert("Amiga", "Unable to insert this hard disk image.");
            return false;
        }
        return true;
    }

    rg_gui_alert("Amiga", "Unsupported content. Use ADF, RP9 or HDF.");
    return false;
}

void app_main(void)
{
    const rg_config_t system_config = {
        .sampleRate = AMIGA_AUDIO_HZ,
        .frameRate = 50,
        .storageRequired = true,
        .romRequired = true,
        .isLauncher = false,
        .handlers = {
            .loadState = &load_state_handler,
            .saveState = &save_state_handler,
            .reset = &reset_handler,
            .screenshot = &screenshot_handler,
            .event = &event_handler,
            .options = &options_handler,
        },
        .mallocAlwaysInternal = 0,
    };

    app = rg_system_init(&system_config);
    app->frameskip = 0;

    if (!rg_settings_exists(NS_APP, "DispScaling"))
        rg_display_set_scaling(RG_DISPLAY_SCALING_FULL);

    surface = rg_surface_create(OUTPUT_WIDTH, OUTPUT_HEIGHT, RG_PIXEL_565_LE, MEM_FAST);
    if (!surface) {
        rg_gui_alert("Amiga", "Unable to allocate the output surface.");
        rg_system_exit();
        return;
    }

    if (!machine_start()) {
        rg_system_exit();
        return;
    }

    if (!insert_content(app->romPath)) {
        rg_system_exit();
        return;
    }

    amiga_audio_enable(true);

    unsigned skip_frames = 0;
    while (true) {
        const int64_t started_us = rg_system_timer();
        const uint32_t buttons = rg_input_read_gamepad();

        const uint32_t pressed = buttons & ~last_buttons;
        last_buttons = buttons;

        if (buttons & (RG_KEY_MENU | RG_KEY_OPTION)) {
            rg_audio_set_mute(true);
            if (buttons & RG_KEY_MENU)
                rg_gui_game_menu();
            else
                rg_gui_options_menu();
            rg_audio_set_mute(false);
            if (disk_pick_requested) {
                disk_pick_requested = false;
                disk_picker();
            }
            last_buttons = rg_input_read_gamepad();
            while (rg_input_read_gamepad() & (RG_KEY_MENU | RG_KEY_OPTION))
                rg_task_delay(20);
            continue;
        }

        if ((buttons & (RG_KEY_START | RG_KEY_SELECT)) == (RG_KEY_START | RG_KEY_SELECT)) {
            if (pending_key_frames > 0) {
                amiga_key(pending_key, false);
                pending_key_frames = 0;
            }
            virtual_keyboard_session();
            last_buttons = rg_input_read_gamepad();
            continue;
        }

        if (pressed & RG_KEY_START)  queue_key(0x44); /* Return */
        if (pressed & RG_KEY_SELECT) queue_key(0x40); /* Space */

        if (pending_key_frames > 0) {
            if (pending_key_frames == 4)
                amiga_key(pending_key, true);
            if (--pending_key_frames == 0)
                amiga_key(pending_key, false);
        }

        amiga_joystick(gamepad_to_joystick(buttons));

        int mouse_dx = 0;
        int mouse_dy = 0;
        uint8_t mouse_buttons = 0;
        if (buttons & RG_KEY_UP)    mouse_dy -= 1;
        if (buttons & RG_KEY_DOWN)  mouse_dy += 1;
        if (buttons & RG_KEY_LEFT)  mouse_dx -= 1;
        if (buttons & RG_KEY_RIGHT) mouse_dx += 1;
        if (buttons & RG_KEY_L) mouse_buttons |= 0x01; /* left mouse button */
        if (buttons & RG_KEY_R) mouse_buttons |= 0x02; /* right mouse button */
        amiga_mouse(mouse_dx, mouse_dy, mouse_buttons);

        const bool display_busy = rg_display_is_busy();
        const bool render_video = skip_frames == 0 && !display_busy;

        amiga_run_frame();

        int busy_us = (int)(rg_system_timer() - started_us);

        if (render_video) {
            const int64_t render_started_us = rg_system_timer();
            render_surface();
            busy_us += (int)(rg_system_timer() - render_started_us);
            rg_display_submit(surface, 0);
        }

        rg_system_tick(busy_us);
        audio_drain();

        if (skip_frames > 0)
            skip_frames--;
        else if (app->frameskip > 0)
            skip_frames = app->frameskip;
    }
}
