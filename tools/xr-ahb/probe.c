// Runs outside the Android JVM to check native loading and the optional AHB API.
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include "../../app/src/main/windows/openxr_runtime/gamenative_openxr_unix.h"
static void log_line(const char *line) { puts(line); }
#include "../../app/src/main/windows/openxr_runtime/unix/gamenative_ahb.h"
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "bridge dlopen: %s\n", dlerror()); return 3; }
    int32_t (**functions)(void *) = dlsym(library, "__wine_unix_call_funcs");
    if (!functions) return 4;
    struct gn_unix_init_args init = {GN_UNIX_ABI_VERSION, GN_UNIX_ERROR_UNAVAILABLE};
    if (functions[GN_UNIX_INIT](&init) || init.result != GN_UNIX_SUCCESS) return 5;
    puts("Bridge loaded and unix initialization succeeded");
    AHardwareBuffer_Desc desc = {.width=16, .height=16, .layers=1,
        .format=AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage=AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT};
    AHardwareBuffer *buffer = NULL;
    int result = gn_ahb_allocate(&desc, &buffer);
    printf("AHB allocation result=%d buffer=%p\n", result, (void *)buffer);
    if (buffer) {
        printf("AHB send on invalid socket result=%d (expected failure)\n", gn_ahb_send(buffer, -1));
        gn_ahb_release(buffer);
    }
    return 0;
}
