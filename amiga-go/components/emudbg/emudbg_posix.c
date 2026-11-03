/*
 * emudbg on a computer (the machines' host builds): a thread serves one
 * client at a time on a TCP port of 127.0.0.1.
 */
#ifndef ESP_PLATFORM

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "emudbg.h"

#define LINE_MAX_BYTES  16384

static pthread_once_t s_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t s_lock;
static pthread_mutex_t s_send_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_wait_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_wait_cond = PTHREAD_COND_INITIALIZER;
static bool s_woken;
static int s_fd = -1;
static int s_port;

static void init_once(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_lock, &attr);
}

void *emudbg_os_alloc(size_t size)
{
    return calloc(1, size);
}

void emudbg_os_lock(void)
{
    pthread_once(&s_once, init_once);
    pthread_mutex_lock(&s_lock);
}

void emudbg_os_unlock(void)
{
    pthread_mutex_unlock(&s_lock);
}

void emudbg_os_wait(int ms)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    struct timespec until = {
        .tv_sec = now.tv_sec + ms / 1000,
        .tv_nsec = (now.tv_usec + (ms % 1000) * 1000L) * 1000L,
    };
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&s_wait_lock);
    if (!s_woken) {
        pthread_cond_timedwait(&s_wait_cond, &s_wait_lock, &until);
    }
    s_woken = false;
    pthread_mutex_unlock(&s_wait_lock);
}

void emudbg_os_wake(void)
{
    pthread_mutex_lock(&s_wait_lock);
    s_woken = true;
    pthread_cond_broadcast(&s_wait_cond);
    pthread_mutex_unlock(&s_wait_lock);
}

void emudbg_os_send(const char *line, size_t len)
{
    pthread_mutex_lock(&s_send_lock);
    if (s_fd >= 0) {
        while (len > 0) {
            ssize_t n = send(s_fd, line, len, 0);
            if (n <= 0) {
                break;
            }
            line += n;
            len -= (size_t)n;
        }
        send(s_fd, "\n", 1, 0);
    }
    pthread_mutex_unlock(&s_send_lock);
}

static void serve_client(int fd)
{
    char *line = malloc(LINE_MAX_BYTES);
    size_t len = 0;
    bool skipping = false;              /* a line too long: dropped */
    char buf[4096];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        for (ssize_t i = 0; i < n; i++) {
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
    free(line);
}

static void *server_thread(void *arg)
{
    int listener = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) {
            continue;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        pthread_mutex_lock(&s_send_lock);
        s_fd = fd;
        pthread_mutex_unlock(&s_send_lock);
        fprintf(stderr, "emudbg: client connected\n");
        emudbg_client(true);
        serve_client(fd);
        emudbg_client(false);
        pthread_mutex_lock(&s_send_lock);
        s_fd = -1;
        pthread_mutex_unlock(&s_send_lock);
        close(fd);
        fprintf(stderr, "emudbg: client gone\n");
    }
    return NULL;
}

bool emudbg_serve(int port)
{
    pthread_once(&s_once, init_once);
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        return false;
    }
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(listener, 1) < 0) {
        perror("emudbg");
        close(listener);
        return false;
    }
    s_port = port;
    pthread_t thread;
    if (pthread_create(&thread, NULL, server_thread, (void *)(intptr_t)listener) != 0) {
        close(listener);
        return false;
    }
    pthread_detach(thread);
    fprintf(stderr, "emudbg: debugger on 127.0.0.1:%d\n", s_port);
    return true;
}

void emudbg_host_loop(void (*frame)(void), int hz)
{
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    long period = 1000000000L / (hz > 0 ? hz : 50);
    for (;;) {
        frame();
        emudbg_frame_end();
        next.tv_nsec += period;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long behind = (now.tv_sec - next.tv_sec) * 1000000000L + (now.tv_nsec - next.tv_nsec);
        if (behind > 100000000L) {
            next = now;                 /* stopped in the debugger: don't race to catch up */
        } else if (behind < 0) {
            struct timespec d = { .tv_sec = -behind / 1000000000L, .tv_nsec = -behind % 1000000000L };
            nanosleep(&d, NULL);
        }
    }
}

void emudbg_host_run(const char *env, void (*frame)(void), int hz)
{
    const char *port = getenv(env);
    if (port == NULL) {
        return;
    }
    if (!emudbg_serve(atoi(port))) {
        exit(1);
    }
    emudbg_host_loop(frame, hz);
}

#endif
