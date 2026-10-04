#include "../../app/src/main/cpp/lsfg/lsfg_common.hpp"
#include <cassert>
#include <iostream>
VkDispatch vkd{};
int main() {
#ifdef LSFG_STEREO_2X
    static_assert(lsfg::LSFG_GENERATION_SLOTS == 1 && lsfg::LSFG_MAX_TARGETS == 1);
    assert(lsfg::LsfgGenerationSlot(1, 0) == 0);
    assert(lsfg::LsfgSlotTimestamp(0) == .5f);
#endif
    // Borrowed AHB image/view must survive move-assignment, replacement, and
    // chain destruction. No Vulkan destroy callback is installed intentionally.
    auto image = reinterpret_cast<VkImage>(uintptr_t(1));
    auto view = reinterpret_cast<VkImageView>(uintptr_t(2));
    lsfg::LsfgImage a(image, view, {640, 480}, VK_FORMAT_R8G8B8A8_UNORM);
    assert(a.Valid() && a.Layout() == VK_IMAGE_LAYOUT_GENERAL);
    lsfg::LsfgImage b(std::move(a));
    assert(!a.Valid() && b.Handle() == image && b.View() == view);
    lsfg::LsfgImage c;
    c = std::move(b);
    assert(!b.Valid() && c.Extent().width == 640);
    c = lsfg::LsfgImage(image, view, {320, 240}, VK_FORMAT_R8G8B8A8_UNORM);
    assert(c.Valid() && c.Extent().height == 240);
    std::cout << "Borrowed LSFG image lifetime tests passed\n";
}
