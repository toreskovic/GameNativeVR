#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <cassert>
#include <fcntl.h>
// Exercise the production transport, with only Android handles/logging stubbed.
#define private public
#include "../../app/src/main/cpp/xrimmersive/xr_windows_transport.h"
#undef private
#include "../../app/src/main/cpp/xrimmersive/xr_windows_transport.cpp"
using namespace xrimmersive::windowsvr;
int main() {
    WindowsFrameTransport transport;
    AHardwareBuffer buffers[2][3];
    for (int eye = 0; eye < 2; ++eye)
        for (int index = 0; index < 3; ++index)
            transport.storeEyeBuffer(eye, &buffers[eye][index], 100, 100, index, false);
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    auto frame = [&](int eye, int index, int id) {
        std::string line = "FRAME eye=" + std::to_string(eye) + " index=" + std::to_string(index) +
            " frame=" + std::to_string(id) + " projection=1 target=1000000000";
        assert(transport.handleFrameLine(sockets[0], line));
        char reply[64]; assert(read(sockets[1], reply, sizeof(reply)) > 0);
    };
    std::array<EyeFrame, 2> pair{};
    assert(!transport.pollStereo(pair));
    frame(0, 0, 1);
    assert(!transport.pollStereo(pair));
    assert(!transport.latestClaimed_[0]);
    frame(1, 0, 1);
    int fence = open("/dev/null", O_RDONLY); assert(fence >= 0);
    transport.latest_[0].acquireFenceFd = fence;
    assert(transport.pollStereo(pair));
    assert(pair[0].frameId == 1 && pair[1].frameId == 1);
    assert(pair[0].acquireFenceFd == fence && transport.latest_[0].acquireFenceFd == -1);
    assert(buffers[0][0].refs == 2 && buffers[1][0].refs == 2);
    close(fence);
    transport.publishReleaseFence(0, 0, -1);
    transport.publishReleaseFence(1, 0, -1);
    assert(!transport.pollStereo(pair)); // Never reread released guest buffers.
    frame(0, 1, 2);
    assert(!transport.pollStereo(pair)); // Cannot combine frame 2 left with frame 1 right.
    frame(0, 2, 3); // Supersede an unclaimed partial frame.
    assert(!transport.releasePending_[0][1]);
    frame(1, 1, 2);
    assert(!transport.pollStereo(pair));
    frame(1, 2, 3);
    assert(!transport.releasePending_[1][1]);
    assert(transport.pollStereo(pair));
    assert(pair[0].frameId == 3 && pair[1].frameId == 3);
    assert(buffers[0][0].refs == 1 && buffers[1][0].refs == 1);
    assert(!transport.pollStereo(pair));
    transport.resetEye(0);
    assert(!transport.pollStereo(pair) && !transport.hasStereoContent());
    transport.resetEye(1);
    frame(1, 0, 4); // Either arrival order is valid.
    assert(!transport.pollStereo(pair));
    frame(0, 0, 4);
    assert(transport.pollStereo(pair) && pair[0].frameId == 4 && pair[1].frameId == 4);
    transport.stop();
    for (auto &eyes : buffers) for (auto &buffer : eyes) assert(buffer.refs == 0);
    close(sockets[0]); close(sockets[1]);
}
