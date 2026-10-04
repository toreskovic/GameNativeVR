#include "../../app/src/main/windows/openxr_runtime/unix/foveated_pack.h"
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
void *test_pack_create(VkDevice device, PFN_vkGetDeviceProcAddr get,
                       VkImage input, VkImage output, VkFormat format,
                       unsigned w, unsigned h, unsigned logical_w, unsigned logical_h) {
  struct gn_pack *p = calloc(1, sizeof(*p));
  if (!gn_pack_init(p, device, get, output, w, h, format, logical_w, logical_h) ||
      !gn_pack_source(p, input, format, 0)) {
    gn_pack_destroy(p);
    free(p);
    return NULL;
  }
  return p;
}
void test_pack_record(void *p, VkCommandBuffer cmd, VkFormat format) {
  gn_pack_record(p, cmd, format);
}
void test_pack_destroy(void *p) {
  gn_pack_destroy(p);
  free(p);
}
