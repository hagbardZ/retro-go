#include "rg_system.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(RG_STORAGE_USBOTG_HOST)
#include <esp_intr_alloc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include "driver/gpio.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "hal/usb_wrap_ll.h"
#endif
#endif

#if defined(RG_STORAGE_SDSPI_HOST)
#include <driver/sdspi_host.h>
#define SDCARD_DO_TRANSACTION sdspi_host_do_transaction
#elif defined(RG_STORAGE_SDMMC_HOST)
#include <driver/sdmmc_host.h>
#ifdef CONFIG_IDF_TARGET_ESP32P4
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif
#define SDCARD_DO_TRANSACTION sdmmc_host_do_transaction
#endif

#ifdef ESP_PLATFORM
#include <esp_vfs_fat.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#include <windows.h>
#define access _access
#define mkdir(A, B) mkdir(A)
#if defined(__MINGW32__)
#include <dirent.h>
#endif
#else
#include <dirent.h>
#include <unistd.h>
#endif

static bool disk_mounted = false;
#if defined(RG_STORAGE_SDSPI_HOST) || defined(RG_STORAGE_SDMMC_HOST)
static sdmmc_card_t *card_handle = NULL;
#endif
#if defined(RG_STORAGE_FLASH_PARTITION)
static wl_handle_t wl_handle = WL_INVALID_HANDLE;
#endif

#define CHECK_PATH(path)          \
    if (!(path && path[0]))       \
    {                             \
        RG_LOGE("No path given"); \
        return false;             \
    }

#if defined(RG_STORAGE_USBOTG_HOST)
static void rg_storage_usb_init(void);
static void rg_storage_usb_deinit(void);
#endif

#if defined(RG_STORAGE_SDSPI_HOST) || defined(RG_STORAGE_SDMMC_HOST)
static esp_err_t sdcard_do_transaction(int slot, sdmmc_command_t *cmdinfo)
{
    rg_system_set_indicator(RG_INDICATOR_ACTIVITY_DISK, 1);

    esp_err_t ret = SDCARD_DO_TRANSACTION(slot, cmdinfo);
    if (ret == ESP_ERR_NO_MEM)
    {
        // free some memory and try again?
    }

    rg_system_set_indicator(RG_INDICATOR_ACTIVITY_DISK, 0);
    return ret;
}
#endif

void rg_storage_init(void)
{
    RG_ASSERT(!disk_mounted, "Storage already initialized!");
    int error_code = -1;

#if defined(RG_STORAGE_SDSPI_HOST)

    RG_LOGI("Looking for SD Card using SDSPI...");

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = RG_GPIO_SDSPI_MOSI,
        .miso_io_num = RG_GPIO_SDSPI_MISO,
        .sclk_io_num = RG_GPIO_SDSPI_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    esp_err_t err = spi_bus_initialize(RG_STORAGE_SDSPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) // check but do not abort, let esp_vfs_fat_sdspi_mount decide
        RG_LOGW("SPI bus init failed (0x%x)", err);

    sdmmc_host_t host_config = SDSPI_HOST_DEFAULT();
    host_config.slot = RG_STORAGE_SDSPI_HOST;
    host_config.max_freq_khz = RG_STORAGE_SDSPI_SPEED;
    host_config.do_transaction = &sdcard_do_transaction;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = RG_STORAGE_SDSPI_HOST;
    slot_config.gpio_cs = RG_GPIO_SDSPI_CS;

    // If we're using esp-idf >= 5.0 and the SPI bus is not shared, we must keep the SD card selected
    // to work around slow accesses. (https://github.com/espressif/esp-idf/issues/10493)
    #ifdef RG_STORAGE_SDSPI_HOLD_CS
    gpio_set_direction(slot_config.gpio_cs, GPIO_MODE_OUTPUT);
    gpio_set_level(slot_config.gpio_cs, 0);
    slot_config.gpio_cs = GPIO_NUM_NC;
    #endif

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 0,
    };

    err = esp_vfs_fat_sdspi_mount(RG_STORAGE_ROOT, &host_config, &slot_config, &mount_config, &card_handle);
    if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_INVALID_CRC)
    {
        RG_LOGW("SD Card mounting failed (0x%x), retrying at lower speed...\n", err);
        host_config.max_freq_khz = SDMMC_FREQ_PROBING;
        err = esp_vfs_fat_sdspi_mount(RG_STORAGE_ROOT, &host_config, &slot_config, &mount_config, &card_handle);
    }
    error_code = (int)err;

#elif defined(RG_STORAGE_SDMMC_HOST)

    RG_LOGI("Looking for SD Card using SDMMC...");

    sdmmc_host_t host_config = SDMMC_HOST_DEFAULT();
    host_config.slot = RG_STORAGE_SDMMC_HOST;
    host_config.max_freq_khz = RG_STORAGE_SDMMC_SPEED;
    host_config.do_transaction = &sdcard_do_transaction;

#ifdef CONFIG_IDF_TARGET_ESP32P4
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;

    esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ret != ESP_OK) {
        RG_LOGE("Failed to create a new on-chip LDO power control driver");
        return;
    }
    host_config.pwr_ctrl_handle = pwr_ctrl_handle;
#endif

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
#if SOC_SDMMC_USE_GPIO_MATRIX /* Only the esp32-s3 routes SDMMC through the GPIO matrix */
    slot_config.clk = RG_GPIO_SDMMC_CLK;
    slot_config.cmd = RG_GPIO_SDMMC_CMD;
    slot_config.d0 = RG_GPIO_SDMMC_D0;
#if defined(RG_GPIO_SDMMC_D1) && defined(RG_GPIO_SDMMC_D2) && defined(RG_GPIO_SDMMC_D3)
    slot_config.width = 4;
    slot_config.d1 = RG_GPIO_SDMMC_D1;
    slot_config.d2 = RG_GPIO_SDMMC_D2;
    slot_config.d3 = RG_GPIO_SDMMC_D3;
#else
    // d1 and d3 normally not used in width=1 but sdmmc_host_init_slot saves them, so just in case
    slot_config.d1 = slot_config.d3 = -1;
#endif
#endif

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 0,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(RG_STORAGE_ROOT, &host_config, &slot_config, &mount_config, &card_handle);
    if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_INVALID_CRC)
    {
        RG_LOGW("SD Card mounting failed (0x%x), retrying at lower speed...\n", err);
        host_config.max_freq_khz = SDMMC_FREQ_PROBING;
        err = esp_vfs_fat_sdmmc_mount(RG_STORAGE_ROOT, &host_config, &slot_config, &mount_config, &card_handle);
    }
    error_code = (int)err;

#elif defined(RG_STORAGE_USBOTG_HOST)

    RG_LOGI("Looking for USB mass storage on %s0...", RG_STORAGE_USB_MOUNT_PATH);
    error_code = -1; // USB is mounted asynchronously, see rg_storage_usb_init()

#elif !defined(RG_STORAGE_FLASH_PARTITION)

    RG_LOGI("Using host (stdlib) for storage.");
    // Maybe we should just check if RG_STORAGE_ROOT exists?
    error_code = 0;

#endif

#if defined(RG_STORAGE_FLASH_PARTITION)

    if (error_code) // only if no previous storage was successfully mounted already
    {
        RG_LOGI("Looking for an internal flash partition labelled '%s' to mount for storage...", RG_STORAGE_FLASH_PARTITION);

        esp_vfs_fat_mount_config_t mount_config = {
            .format_if_mount_failed = true, // if mount failed, it's probably because it's a clean install so the partition hasn't been formatted yet
            .max_files = 4, // must be initialized, otherwise it will be 0, which doesn't make sense, and will trigger an ESP_ERR_NO_MEM error
        };

        esp_err_t err = esp_vfs_fat_spiflash_mount(RG_STORAGE_ROOT, RG_STORAGE_FLASH_PARTITION, &mount_config, &wl_handle);
        error_code = (int)err;
    }

#endif

#if defined(RG_STORAGE_USBOTG_HOST)
    /* USB mass storage is supplemental (and asynchronous), so it is started
     * regardless of whether the main storage mounted successfully. */
    rg_storage_usb_init();
#endif

    disk_mounted = !error_code;

    if (disk_mounted)
        RG_LOGI("Storage mounted at %s.", RG_STORAGE_ROOT);
    else
        RG_LOGE("Storage mounting failed! err=0x%x", error_code);
}

void rg_storage_deinit(void)
{
    if (disk_mounted)
    {
        rg_storage_commit();

        int error_code = 0;

#if defined(RG_STORAGE_SDSPI_HOST) || defined(RG_STORAGE_SDMMC_HOST)
        if (card_handle != NULL)
        {
            esp_err_t err = esp_vfs_fat_sdcard_unmount(RG_STORAGE_ROOT, card_handle);
            card_handle = NULL; // NULL it regardless of success, nothing we can do on errors...
            error_code = (int)err;
        }
#endif

#if defined(RG_STORAGE_FLASH_PARTITION)
        if (wl_handle != WL_INVALID_HANDLE)
        {
            esp_err_t err = esp_vfs_fat_spiflash_unmount(RG_STORAGE_ROOT, wl_handle);
            wl_handle = WL_INVALID_HANDLE;
            error_code = (int)err;
        }
#endif

        if (error_code)
            RG_LOGE("Storage unmounting failed. err=0x%x", error_code);
        else
            RG_LOGI("Storage unmounted.");

        disk_mounted = false;
    }

#if defined(RG_STORAGE_USBOTG_HOST)
    rg_storage_usb_deinit();
#endif
}

bool rg_storage_ready(void)
{
    return disk_mounted || rg_storage_usb_mount_count() > 0;
}

void rg_storage_commit(void)
{
    if (!disk_mounted)
        return;
    // flush buffers();
}

bool rg_storage_mkdir(const char *dir)
{
    CHECK_PATH(dir);

    if (mkdir(dir, 0777) == 0)
        return true;

    // FIXME: Might want to stat to see if it's a dir
    if (errno == EEXIST)
        return true;

    // Possibly missing some parents, try creating them
    char *temp = strdup(dir);
    for (char *p = temp + strlen(RG_STORAGE_ROOT) + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = 0;
            if (strlen(temp) > 0)
            {
                mkdir(temp, 0777);
            }
            *p = '/';
            while (*(p + 1) == '/')
                p++;
        }
    }
    free(temp);

    // Finally try again
    if (mkdir(dir, 0777) == 0)
        return true;

    return false;
}

static int delete_cb(const rg_scandir_t *file, void *arg)
{
    rg_storage_delete(file->path);
    return RG_SCANDIR_CONTINUE;
}

bool rg_storage_delete(const char *path)
{
    CHECK_PATH(path);

    // Try the fast way first
    if (remove(path) == 0 || rmdir(path) == 0)
        return true;

    // If that fails, it's likely a non-empty directory and we go recursive
    // (errno could confirm but it has proven unreliable across platforms...)
    if (rg_storage_scandir(path, delete_cb, NULL, 0))
        return rmdir(path) == 0;

    return false;
}

rg_stat_t rg_storage_stat(const char *path)
{
    rg_stat_t ret = {0};
    struct stat statbuf;
    if (path && stat(path, &statbuf) == 0)
    {
        ret.basename = rg_basename(path);
        ret.extension = rg_extension(path);
        ret.size = statbuf.st_size;
        ret.mtime = statbuf.st_mtime;
        ret.is_file = S_ISREG(statbuf.st_mode);
        ret.is_dir = S_ISDIR(statbuf.st_mode);
        ret.exists = true;
    }
    return ret;
}

bool rg_storage_exists(const char *path)
{
    CHECK_PATH(path);
    return access(path, F_OK) == 0;
}

bool rg_storage_scandir(const char *path, rg_scandir_cb_t *callback, void *arg, uint32_t flags)
{
    CHECK_PATH(path);
    uint32_t types = flags & (RG_SCANDIR_FILES | RG_SCANDIR_DIRS);
    size_t path_len = strlen(path) + 1;
    struct stat statbuf;
    struct dirent *ent;

    if (path_len > RG_PATH_MAX - 5)
    {
        RG_LOGE("Folder path too long '%s'", path);
        return false;
    }

    DIR *dir = opendir(path);
    if (!dir)
    {
        if (errno != ENOENT) // Only log unusual errors. Path not found isn't unusual.
            RG_LOGE("Opendir failed (%d): '%s'", errno, path);
        return false;
    }

    // We allocate on heap in case we go recursive through rg_storage_delete
    rg_scandir_t *result = calloc(1, sizeof(rg_scandir_t));
    if (!result)
    {
        RG_LOGE("Memory allocation failed: '%s'", path);
        closedir(dir);
        return false;
    }

    strcat(strcpy(result->path, path), "/");
    result->basename = result->path + path_len;
    result->dirname = path;

    while ((ent = readdir(dir)))
    {
        if (ent->d_name[0] == '.' && (!ent->d_name[1] || ent->d_name[1] == '.'))
        {
            // Skip self and parent
            continue;
        }

        if (path_len + strlen(ent->d_name) >= RG_PATH_MAX)
        {
            RG_LOGE("File path too long '%s/%s'", path, ent->d_name);
            continue;
        }

        strcpy((char *)result->basename, ent->d_name);
    #if defined(DT_REG) && defined(DT_DIR)
        result->is_file = ent->d_type == DT_REG;
        result->is_dir = ent->d_type == DT_DIR;
    #else
        result->is_file = 0;
        result->is_dir = 0;
        // We're forced to stat() if the OS doesn't provide type via dirent
        flags |= RG_SCANDIR_STAT;
    #endif

        if ((flags & RG_SCANDIR_STAT) && stat(result->path, &statbuf) == 0)
        {
            result->is_file = S_ISREG(statbuf.st_mode);
            result->is_dir = S_ISDIR(statbuf.st_mode);
            result->size = statbuf.st_size;
            result->mtime = statbuf.st_mtime;
        }

        if ((result->is_dir && types != RG_SCANDIR_FILES) || (result->is_file && types != RG_SCANDIR_DIRS))
        {
            int ret = (callback)(result, arg);

            if (ret == RG_SCANDIR_STOP)
                break;

            if (ret == RG_SCANDIR_SKIP)
                continue;
        }

        if ((flags & RG_SCANDIR_RECURSIVE) && result->is_dir)
        {
            rg_storage_scandir(result->path, callback, arg, flags);
        }
    }

    closedir(dir);
    free(result);

    return true;
}

int64_t rg_storage_get_free_space(const char *path)
{
    // Here we should translate the provided VFS path to the matching filesystem driver and drive
    // But we don't. Instead we just assume it's drive 0 of the fatfs driver. Yay laziness.
#ifdef ESP_PLATFORM
    DWORD nclst;
    FATFS *fatfs;
    if (f_getfree("0:", &nclst, &fatfs) == FR_OK)
    {
        return (int64_t)nclst * fatfs->csize * fatfs->ssize;
    }
#endif

    return -1;
}

bool rg_storage_read_file(const char *path, void **data_out, size_t *data_len, uint32_t flags)
{
    RG_ASSERT_ARG(data_out && data_len);
    CHECK_PATH(path);

    size_t output_buffer_alloc_size;
    size_t output_buffer_size;
    void *output_buffer;
    size_t file_size;

    FILE *fp = fopen(path, "rb");
    if (!fp)
    {
        if (errno != ENOENT) // Only log unusual errors. Path not found isn't unusual.
            RG_LOGE("Fopen failed (%d): '%s'", errno, path);
        return false;
    }

    fseek(fp, 0, SEEK_END);
    file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (flags & RG_FILE_USER_BUFFER)
    {
        output_buffer_alloc_size = *data_len;
        output_buffer_size = RG_MIN(*data_len, file_size);
        output_buffer = *data_out;
    }
    else
    {
        size_t blocksize = RG_MAX(0x400, (flags & 0xF) * 0x2000);
        output_buffer_alloc_size = (file_size + (blocksize - 1)) & ~(blocksize - 1);
        output_buffer_size = file_size;
        output_buffer = malloc(output_buffer_alloc_size);
    }

    if (!output_buffer)
    {
        RG_LOGE("Memory allocation failed: '%s'", path);
        fclose(fp);
        return false;
    }

    if (!fread(output_buffer, output_buffer_size, 1, fp))
    {
        RG_LOGE("File read failed (%d): '%s'", errno, path);
        fclose(fp);
        if (!(flags & RG_FILE_USER_BUFFER))
            free(output_buffer);
        return false;
    }

    fclose(fp);

    // Wipe the extra allocated space, if any
    if (output_buffer_alloc_size > output_buffer_size)
    {
        memset(output_buffer + output_buffer_size, 0, output_buffer_alloc_size - output_buffer_size);
    }

    *data_out = output_buffer;
    *data_len = output_buffer_size;
    return true;
}

bool rg_storage_write_file(const char *path, const void *data_ptr, size_t data_len, uint32_t flags)
{
    RG_ASSERT_ARG(data_ptr || !data_len);
    CHECK_PATH(path);

    // TODO: If atomic is true we should write to a temp file and only replace the target on success
    FILE *fp = fopen(path, "wb");
    if (!fp)
    {
        RG_LOGE("Fopen failed (%d): '%s'", errno, path);
        return false;
    }

    if (data_len && !fwrite(data_ptr, data_len, 1, fp))
    {
        RG_LOGE("Fwrite failed (%d): '%s'", errno, path);
        fclose(fp);
        return false;
    }

    fclose(fp);
    return true;
}

/**
 * This is a minimal UNZIP implementation that utilizes only the miniz primitives found in ESP32's ROM.
 * I think that we should use miniz' ZIP API instead and bundle miniz with retro-go. But first I need
 * to do some testing to determine if the increased executable size is acceptable...
 */
#if RG_ZIP_SUPPORT

#include <miniz.h>

#define ZIP_MAGIC 0x04034b50
typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint16_t compression;
    uint16_t modified_time;
    uint16_t modified_date;
    uint32_t checksum;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t filename_size;
    uint16_t extra_field_size;
    uint8_t filename[226];
    // uint8_t extra_field[];
    // uint8_t compressed_data[];
} zip_header_t;

bool rg_storage_unzip_file(const char *zip_path, const char *filter, void **data_out, size_t *data_len, uint32_t flags)
{
    RG_ASSERT_ARG(data_out && data_len);
    CHECK_PATH(zip_path);

    zip_header_t header = {0};
    int header_pos = 0;

    FILE *fp = fopen(zip_path, "rb");
    if (!fp)
    {
        if (errno != ENOENT) // Only log unusual errors. Path not found isn't unusual.
            RG_LOGE("Fopen failed (%d): '%s'", errno, zip_path);
        return false;
    }

    // Very inefficient, we should read a block at a time and search it for a header. But I'm lazy.
    // Thankfully the header is usually found on the very first read :)
    for (header_pos = 0; !feof(fp) && header_pos < 0x10000; ++header_pos)
    {
        fseek(fp, header_pos, SEEK_SET);
        fread(&header, sizeof(header), 1, fp);
        if (header.magic == ZIP_MAGIC)
            break;
    }

    if (header.magic != ZIP_MAGIC)
    {
        RG_LOGE("No valid header found: '%s'", zip_path);
        fclose(fp);
        return false;
    }

    // Zero terminate or truncate filename just in case
    header.filename[RG_MIN(header.filename_size, 225)] = 0;

    RG_LOGI("Found file at %d, name: '%s', size: %d", header_pos, header.filename, (int)header.uncompressed_size);

    size_t stream_offset = header_pos + 30 + header.filename_size + header.extra_field_size;
    size_t stream_remaining = header.compressed_size;
    size_t output_buffer_align = RG_MAX(0x1000, (flags & 0xF) * 0x2000);
    size_t output_buffer_size;
    size_t output_buffer_pos = 0;
    uint8_t *output_buffer = NULL;

    if (flags & RG_FILE_USER_BUFFER)
    {
        output_buffer_size = RG_MIN(*data_len, header.uncompressed_size);
        output_buffer = *data_out;
    }
    else
    {
        output_buffer_size = header.uncompressed_size;
        output_buffer = malloc((output_buffer_size + (output_buffer_align - 1)) & ~(output_buffer_align - 1));
    }

    size_t read_buffer_size = 0x8000;
    uint8_t *read_buffer = malloc(read_buffer_size);
    tinfl_decompressor *decomp = malloc(sizeof(tinfl_decompressor));

    if (!read_buffer || !output_buffer || !decomp)
    {
        RG_LOGE("Memory allocation failed: '%s'", zip_path);
        goto _fail;
    }

    tinfl_status status;
    tinfl_init(decomp);

    do
    {
        size_t input_size = RG_MIN(read_buffer_size, stream_remaining);
        size_t output_size = output_buffer_size - output_buffer_pos;
        if (fseek(fp, stream_offset, SEEK_SET) != 0 || fread(read_buffer, input_size, 1, fp) != 1)
        {
            RG_LOGE("Read error (%d): '%s'", errno, zip_path);
            goto _fail;
        }
        stream_offset += input_size;
        stream_remaining -= input_size;
        status = tinfl_decompress(
            decomp, read_buffer, &input_size, output_buffer, output_buffer + output_buffer_pos, &output_size,
            TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF | (stream_remaining ? TINFL_FLAG_HAS_MORE_INPUT : 0));
        output_buffer_pos += output_size;
    } while (status == TINFL_STATUS_NEEDS_MORE_INPUT);

    // With user-provided buffer we might not reach TINFL_STATUS_DONE, but it doesn't mean we've failed
    if (status < TINFL_STATUS_DONE || output_buffer_pos != output_buffer_size) // (status != TINFL_STATUS_DONE)
    {
        RG_LOGE("Decompression failed (%d): %s", (int)status, zip_path);
        goto _fail;
    }

    free(read_buffer);
    free(decomp);
    fclose(fp);

    *data_out = output_buffer;
    *data_len = output_buffer_size;
    return true;

_fail:
    if (!(flags & RG_FILE_USER_BUFFER))
        free(output_buffer);
    free(read_buffer);
    free(decomp);
    fclose(fp);
    return false;
}
#else
bool rg_storage_unzip_file(const char *zip_path, const char *filter, void **data_out, size_t *data_len, uint32_t flags)
{
    RG_LOGE("ZIP support hasn't been enabled!");
    return false;
}
#endif

#if defined(RG_STORAGE_USBOTG_HOST)

#if CONFIG_IDF_TARGET_ESP32P4
#include "hal/usb_wrap_ll.h"
#include "soc/usb_wrap_struct.h"
#endif

/* USB Host Mass Storage support. A background task installs the USB host
 * library and MSC driver, then enumerates connected USB drives and mounts each
 * one at /usb0, /usb1, ... (see RG_STORAGE_USB_MOUNT_PATH). The main storage
 * (SD card / flash) is unaffected and keeps RG_STORAGE_ROOT. */
#define RG_USB_MAX_DEVICES (CONFIG_FATFS_VOLUME_COUNT > 1 ? CONFIG_FATFS_VOLUME_COUNT - 1 : 1)

typedef struct
{
    uint8_t usb_addr;                 /* USB device address */
    msc_host_device_handle_t msc_device;  /* MSC device handle */
    msc_host_vfs_handle_t vfs_handle;     /* VFS handle for the mount point */
} rg_usb_device_t;

typedef struct
{
    enum
    {
        RG_USB_DEVICE_CONNECTED,
        RG_USB_DEVICE_DISCONNECTED,
    } id;
    union
    {
        uint8_t address;                 /* Newly connected device address */
        msc_host_device_handle_t device; /* Handle of the removed device */
    } data;
} rg_usb_msg_t;

static rg_usb_device_t *rg_usb_devices[RG_USB_MAX_DEVICES] = {0};
static QueueHandle_t rg_usb_queue = NULL;
static int rg_usb_mount_count = 0;

static int rg_usb_find_free_slot(void)
{
    for (int i = 0; i < RG_USB_MAX_DEVICES; i++)
        if (rg_usb_devices[i] == NULL)
            return i;
    return -1;
}

static void rg_usb_msc_event_cb(const msc_host_event_t *event, void *arg)
{
    if (!rg_usb_queue)
        return;

    if (event->event == MSC_DEVICE_CONNECTED)
    {
        rg_usb_msg_t msg = {.id = RG_USB_DEVICE_CONNECTED, .data.address = event->device.address};
        xQueueSend(rg_usb_queue, &msg, portMAX_DELAY);
    }
    else if (event->event == MSC_DEVICE_DISCONNECTED)
    {
        rg_usb_msg_t msg = {.id = RG_USB_DEVICE_DISCONNECTED, .data.device = event->device.handle};
        xQueueSend(rg_usb_queue, &msg, portMAX_DELAY);
    }
}

static void rg_usb_host_task(void *arg)
{
    const usb_host_config_t host_config = {.intr_flags = ESP_INTR_FLAG_LEVEL1};
    if (usb_host_install(&host_config) != ESP_OK)
    {
        RG_LOGE("rg_usb: usb_host_install failed");
        vTaskDelete(NULL);
        return;
    }
#if CONFIG_IDF_TARGET_ESP32P4
    RG_LOGI("rg_usb: DWC selected, FSLS phy pad_enable=%d (usb_wrap otg_conf=0x%08lx)",
            (int)usb_wrap_ll_phy_is_pad_enabled(&USB_WRAP), (unsigned long)USB_WRAP.otg_conf.val);

    // The HS/UTMI DWC host talks over its dedicated USB pads (module pins 16/17) and does
    // not need the OTG11 FSLS PHY that is mapped to GPIO26/27 (buttons A/B). Powering that
    // PHY block back down and returning the pads to GPIO inputs prevents the FSLS bus-idle
    // state from holding the gamepad lines low.
    LP_AON_CLKRST.hp_usb_clkrst_ctrl0.usb_otg11_48m_clk_en = 0;
    HP_SYS_CLKRST.soc_clk_ctrl1.reg_usb_otg11_sys_clk_en = 0;
    LP_AON_CLKRST.hp_usb_clkrst_ctrl1.rst_en_usb_otg11 = 1;
    LP_AON_CLKRST.hp_usb_clkrst_ctrl1.rst_en_usb_otg11 = 0;
    USB_WRAP.otg_conf.usb_pad_enable = 0;
    gpio_config_t gpio_cfg = {
        .pin_bit_mask = (1ULL << GPIO_NUM_26) | (1ULL << GPIO_NUM_27),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&gpio_cfg);
    RG_LOGI("rg_usb: OTG11 FSLS PHY powered down, GPIO26/27 restored (otg_conf=0x%08lx)",
            (unsigned long)USB_WRAP.otg_conf.val);
#endif

    const msc_host_driver_config_t msc_config = {
        .create_backround_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .callback = rg_usb_msc_event_cb,
    };
    if (msc_host_install(&msc_config) != ESP_OK)
    {
        RG_LOGE("rg_usb: msc_host_install failed");
        vTaskDelete(NULL);
        return;
    }

    RG_LOGI("rg_usb: host installed, waiting for USB mass storage devices");

    for (;;)
    {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        /* The MSC driver is a client and keeps running until we uninstall it,
         * so there is nothing else to do here. */
    }
}

static void rg_usb_event_task(void *arg)
{
    rg_usb_msg_t msg;

    while (xQueueReceive(rg_usb_queue, &msg, portMAX_DELAY) == pdTRUE)
    {
        if (msg.id == RG_USB_DEVICE_CONNECTED)
        {
            int slot = rg_usb_find_free_slot();
            if (slot < 0)
            {
                RG_LOGW("rg_usb: no free slot for new device (max %d)", RG_USB_MAX_DEVICES);
                continue;
            }

            rg_usb_device_t *dev = calloc(1, sizeof(*dev));
            if (!dev)
                continue;

            esp_err_t err = msc_host_install_device(msg.data.address, &dev->msc_device);
            if (err != ESP_OK)
            {
                RG_LOGE("rg_usb: msc_host_install_device failed: %s", esp_err_to_name(err));
                free(dev);
                continue;
            }
            dev->usb_addr = msg.data.address;

            const esp_vfs_fat_mount_config_t mount_config = {
                .format_if_mount_failed = false,
                .max_files = 4,
                .allocation_unit_size = 8192,
            };

            char mount_path[16];
            snprintf(mount_path, sizeof(mount_path), RG_STORAGE_USB_MOUNT_PATH "%d", slot);

            err = msc_host_vfs_register(dev->msc_device, mount_path, &mount_config, &dev->vfs_handle);
            if (err != ESP_OK)
            {
                RG_LOGW("rg_usb: failed to mount %s: %s", mount_path, esp_err_to_name(err));
                msc_host_uninstall_device(dev->msc_device);
                free(dev);
                continue;
            }

            rg_usb_devices[slot] = dev;
            rg_usb_mount_count++;
            RG_LOGI("rg_usb: mounted %s", mount_path);
        }
        else if (msg.id == RG_USB_DEVICE_DISCONNECTED)
        {
            for (int i = 0; i < RG_USB_MAX_DEVICES; i++)
            {
                rg_usb_device_t *dev = rg_usb_devices[i];
                if (dev && dev->msc_device == msg.data.device)
                {
                    msc_host_vfs_unregister(dev->vfs_handle);
                    msc_host_uninstall_device(dev->msc_device);
                    free(dev);
                    rg_usb_devices[i] = NULL;
                    rg_usb_mount_count--;
                    RG_LOGI("rg_usb: unmounted %s%d", RG_STORAGE_USB_MOUNT_PATH, i);
                    break;
                }
            }
        }
    }
}

int rg_storage_usb_mount_count(void)
{
    return rg_usb_mount_count;
}

bool rg_storage_usb_wait(int64_t timeout_ms)
{
    int64_t deadline = rg_system_timer() + timeout_ms * 1000;
    while (rg_usb_mount_count == 0 && rg_system_timer() < deadline)
        rg_task_delay(10);
    return rg_usb_mount_count > 0;
}

static void rg_storage_usb_init(void)
{
    if (rg_usb_queue)
        return; // Already initialized

    rg_usb_queue = xQueueCreate(8, sizeof(rg_usb_msg_t));
    if (!rg_usb_queue)
    {
        RG_LOGE("rg_usb: failed to create event queue");
        return;
    }

    if (xTaskCreate(rg_usb_host_task, "rg_usb_host", 4096, NULL, 2, NULL) != pdPASS)
        RG_LOGE("rg_usb: failed to create host task");
    if (xTaskCreate(rg_usb_event_task, "rg_usb_event", 8192, NULL, 5, NULL) != pdPASS)
        RG_LOGE("rg_usb: failed to create event task");
}

static void rg_storage_usb_deinit(void)
{
    for (int i = 0; i < RG_USB_MAX_DEVICES; i++)
    {
        rg_usb_device_t *dev = rg_usb_devices[i];
        if (!dev)
            continue;
        if (dev->vfs_handle)
            msc_host_vfs_unregister(dev->vfs_handle);
        if (dev->msc_device)
            msc_host_uninstall_device(dev->msc_device);
        free(dev);
        rg_usb_devices[i] = NULL;
    }
    rg_usb_mount_count = 0;

    msc_host_uninstall();
    /* usb_host_uninstall() must be called from the host task itself (see the
     * example), which keeps running; the hardware resets on the upcoming
     * reboot anyway. */

    if (rg_usb_queue)
    {
        vQueueDelete(rg_usb_queue);
        rg_usb_queue = NULL;
    }
}

#endif /* RG_STORAGE_USBOTG_HOST */
