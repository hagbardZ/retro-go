/*
 * emudbg on the board: a task on core 0 serves one client at a time on a
 * TCP port (CONFIG_EMUDBG_PORT); the emulation task (core 1) waits in
 * emudbg_hit() on a semaphore while the machine is stopped.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "emudbg.h"

#if EMUDBG_ENABLED

#define LINE_MAX_BYTES  16384

static const char *TAG = "emudbg";
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_send_lock;
static SemaphoreHandle_t s_wake;
static volatile int s_fd = -1;

static void init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutex();
        s_send_lock = xSemaphoreCreateMutex();
        s_wake = xSemaphoreCreateBinary();
    }
}

void *emudbg_os_alloc(size_t size)
{
    void *p = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(1, size);
}

void emudbg_os_lock(void)
{
    init();
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

void emudbg_os_unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

void emudbg_os_wait(int ms)
{
    xSemaphoreTake(s_wake, pdMS_TO_TICKS(ms));
}

void emudbg_os_wake(void)
{
    xSemaphoreGive(s_wake);
}

void emudbg_os_send(const char *line, size_t len)
{
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    int fd = s_fd;
    if (fd >= 0) {
        while (len > 0) {
            int n = send(fd, line, len, 0);
            if (n <= 0) {
                break;
            }
            line += n;
            len -= (size_t)n;
        }
        send(fd, "\n", 1, 0);
    }
    xSemaphoreGive(s_send_lock);
}

static void serve_client(int fd, char *line)
{
    size_t len = 0;
    bool skipping = false;              /* a line too long: dropped */
    char buf[512];
    for (;;) {
        int n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        for (int i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\n') {
                if (!skipping) {
                    line[len] = '\0';
                    emudbg_command(line);
                }
                len = 0;
                skipping = false;
            } else if (len + 1 < LINE_MAX_BYTES) {
                line[len++] = ch;
            } else {
                skipping = true;
            }
        }
    }
}

static void server_task(void *arg)
{
    int port = (int)(intptr_t)arg;
    char *line = emudbg_os_alloc(LINE_MAX_BYTES);
    int listener = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    bool v6 = listener >= 0;
    if (!v6) {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    }
    if (listener < 0 || line == NULL) {
        ESP_LOGE(TAG, "no socket");
        vTaskDelete(NULL);
    }
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    int err;
    if (v6) {
        /* IPv6 and IPv4 both (macOS asks .local names over IPv6 first) */
        int zero = 0;
        setsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
        struct sockaddr_in6 addr = { .sin6_family = AF_INET6, .sin6_port = htons(port) };
        err = bind(listener, (struct sockaddr *)&addr, sizeof(addr));
    } else {
        struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(port),
                                    .sin_addr.s_addr = htonl(INADDR_ANY) };
        err = bind(listener, (struct sockaddr *)&addr, sizeof(addr));
    }
    if (err != 0 || listen(listener, 1) != 0) {
        ESP_LOGE(TAG, "can't listen on port %d", port);
        close(listener);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "debugger on port %d", port);
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        /* A client that went away without a word (the computer slept) */
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        int idle = 10, interval = 5, count = 3;
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
        xSemaphoreTake(s_send_lock, portMAX_DELAY);
        s_fd = fd;
        xSemaphoreGive(s_send_lock);
        ESP_LOGI(TAG, "client connected");
        emudbg_client(true);
        serve_client(fd, line);
        emudbg_client(false);
        xSemaphoreTake(s_send_lock, portMAX_DELAY);
        s_fd = -1;
        xSemaphoreGive(s_send_lock);
        close(fd);
        ESP_LOGI(TAG, "client gone");
    }
}

/* The server's stack in PSRAM, 12 KB: internal RAM is scarce on the S3 (with
 * 4 KB of it here the Commodore's setup screen task didn't fit any more),
 * and a command may go deep: Musashi's disassembler builds its table on
 * this stack the first time (a copy of its opcode list; 4 KB overflowed).
 * The task never touches the flash, which a PSRAM stack wouldn't allow.
 */
#define SERVER_STACK    12288

bool emudbg_serve(int port)
{
    init();
    return xTaskCreatePinnedToCoreWithCaps(server_task, "emudbg", SERVER_STACK, (void *)(intptr_t)port, 3,
                                           NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
}

#endif
