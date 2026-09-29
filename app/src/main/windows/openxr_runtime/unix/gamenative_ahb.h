#ifndef GAMENATIVE_AHB_H
#define GAMENATIVE_AHB_H

#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>

// No link-time libandroid dependency: Android 10 vendor builds may pull in a
// Java runtime that cannot initialize inside Wine. Keep a complete API from one
// provider and retain its handle for as long as buffers/function pointers live.
static int (*gn_ahb_allocate_fn)(const AHardwareBuffer_Desc *, AHardwareBuffer **);
static void (*gn_ahb_release_fn)(AHardwareBuffer *);
static int (*gn_ahb_send_fn)(const AHardwareBuffer *, int);
static pthread_once_t gn_ahb_once = PTHREAD_ONCE_INIT;

static void gn_ahb_load(void)
{
    const char *providers[] = {"libnativewindow.so", "libandroid.so"};
    for (unsigned i = 0; i < sizeof(providers) / sizeof(providers[0]); ++i) {
        void *library = dlopen(providers[i], RTLD_NOW | RTLD_LOCAL);
        char message[512];
        if (!library) {
            const char *error = dlerror();
            snprintf(message, sizeof(message), "AHardwareBuffer provider %s unavailable: %s",
                     providers[i], error ? error : "unknown loader error");
            log_line(message);
            continue;
        }
        int (*allocate)(const AHardwareBuffer_Desc *, AHardwareBuffer **) =
            (int (*)(const AHardwareBuffer_Desc *, AHardwareBuffer **))dlsym(library, "AHardwareBuffer_allocate");
        void (*release)(AHardwareBuffer *) =
            (void (*)(AHardwareBuffer *))dlsym(library, "AHardwareBuffer_release");
        int (*send_handle)(const AHardwareBuffer *, int) =
            (int (*)(const AHardwareBuffer *, int))dlsym(library, "AHardwareBuffer_sendHandleToUnixSocket");
        if (allocate && release && send_handle) {
            gn_ahb_allocate_fn = allocate;
            gn_ahb_release_fn = release;
            gn_ahb_send_fn = send_handle;
            snprintf(message, sizeof(message), "AHardwareBuffer provider ready: %s", providers[i]);
            log_line(message);
            return;
        }
        snprintf(message, sizeof(message), "AHardwareBuffer provider %s has incomplete API", providers[i]);
        log_line(message);
        dlclose(library);
    }
    log_line("AHardwareBuffer API unavailable; using dma-buf transport fallback");
}

static int gn_ahb_available(void)
{
    pthread_once(&gn_ahb_once, gn_ahb_load);
    return gn_ahb_allocate_fn != NULL;
}

static int gn_ahb_allocate(const AHardwareBuffer_Desc *descriptor, AHardwareBuffer **buffer)
{
    if (!buffer) return -EINVAL;
    *buffer = NULL;
    return gn_ahb_available() ? gn_ahb_allocate_fn(descriptor, buffer) : -ENOSYS;
}

static void gn_ahb_release(AHardwareBuffer *buffer)
{
    if (buffer && gn_ahb_available()) gn_ahb_release_fn(buffer);
}

static int gn_ahb_send(const AHardwareBuffer *buffer, int socket_fd)
{
    if (!buffer) return -EINVAL;
    return gn_ahb_available() ? gn_ahb_send_fn(buffer, socket_fd) : -ENOSYS;
}
#endif
