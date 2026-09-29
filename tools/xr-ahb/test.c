// Mock providers exercise fallback paths even on devices with working AHB APIs.
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int scenario, opens, closes, releases, sends;
static int allocate_mock(const AHardwareBuffer_Desc *desc, AHardwareBuffer **out) {
    assert(desc); *out = (AHardwareBuffer *)(uintptr_t)42; return 0;
}
static void release_mock(AHardwareBuffer *buffer) { assert(buffer); ++releases; }
static int send_mock(const AHardwareBuffer *buffer, int fd) { assert(buffer && fd == 7); ++sends; return 0; }
static void *open_mock(const char *name, int flags) {
    assert(flags == (RTLD_NOW | RTLD_LOCAL)); ++opens;
    int native = strcmp(name, "libnativewindow.so") == 0;
    assert(native || strcmp(name, "libandroid.so") == 0);
    if (scenario == 4 || (scenario == 2 && native) || (scenario == 3 && !native)) return NULL;
    return (void *)(uintptr_t)(native ? 1 : 2);
}
static void *symbol_mock(void *library, const char *name) {
    assert(library);
    if (!strcmp(name, "AHardwareBuffer_allocate")) return (void *)allocate_mock;
    if (!strcmp(name, "AHardwareBuffer_release")) return scenario == 3 ? NULL : (void *)release_mock;
    if (!strcmp(name, "AHardwareBuffer_sendHandleToUnixSocket")) return (void *)send_mock;
    assert(0); return NULL;
}
static int close_mock(void *library) { assert(library); ++closes; return 0; }
static char *error_mock(void) { return "simulated missing library"; }
static void log_line(const char *line) { puts(line); }
#define dlopen open_mock
#define dlsym symbol_mock
#define dlclose close_mock
#define dlerror error_mock
#include "../../app/src/main/windows/openxr_runtime/unix/gamenative_ahb.h"
int main(int argc, char **argv) {
    assert(argc == 2); scenario = atoi(argv[1]); assert(scenario >= 1 && scenario <= 4);
    int available = scenario <= 2;
    assert(gn_ahb_available() == available);
    AHardwareBuffer_Desc desc = {0};
    AHardwareBuffer *buffer = (AHardwareBuffer *)(uintptr_t)1;
    assert(gn_ahb_allocate(&desc, &buffer) == (available ? 0 : -ENOSYS));
    if (available) {
        assert(buffer == (AHardwareBuffer *)(uintptr_t)42);
        assert(gn_ahb_send(buffer, 7) == 0 && sends == 1);
        gn_ahb_release(buffer); assert(releases == 1);
    } else {
        assert(buffer == NULL);
        assert(gn_ahb_send((AHardwareBuffer *)(uintptr_t)42, 7) == -ENOSYS);
        gn_ahb_release((AHardwareBuffer *)(uintptr_t)42); assert(releases == 0);
    }
    assert(gn_ahb_allocate(&desc, NULL) == -EINVAL);
    assert(gn_ahb_send(NULL, 7) == -EINVAL);
    gn_ahb_release(NULL);
    assert(gn_ahb_available() == available);
    assert(opens == (scenario == 1 ? 1 : 2));
    assert(closes == (scenario == 3 ? 1 : 0));
    printf("AHB provider scenario %d passed\n", scenario);
}
