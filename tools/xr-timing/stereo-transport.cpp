#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <cassert>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <poll.h>
// Exercise the production transport, with only Android handles/logging stubbed.
#define private public
#include "../../app/src/main/cpp/xrimmersive/xr_windows_transport.h"
#undef private
#include "../../app/src/main/cpp/xrimmersive/xr_windows_transport.cpp"
using namespace xrimmersive::windowsvr;
int main() {
    // Probe negotiation must not replace a registered image or take a frame
    // lease. Rejection must leave the socket usable for the copy fallback.
    {
        WindowsFrameTransport probe;
        int channel[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, channel));
        std::string reply;
        const std::string request = "PROBE_BUFFER eye=0 index=0 w=100 h=100 layer=1 packing=1";
        assert(probe.handleBufferLine(channel[0], request));
        assert(readLine(channel[1], reply) && reply == "ERR unsupported");
        bool accept = false;
        int probes = 0;
        probe.probeHardwareBuffer = [&](const EyeFrame &f) {
            ++probes; assert(f.bufferLayer == 1 && f.width == 100 && f.height == 100 && f.foveatedPacked);
            return accept;
        };
        for (bool supported : {false, true}) {
            AHardwareBuffer buffer;
            accept = supported; testReceivedHardwareBuffer = &buffer;
            assert(probe.handleBufferLine(channel[0], request));
            assert(readLine(channel[1], reply) && reply == "OK");
            assert(readLine(channel[1], reply) && reply == (supported ? "OK stored" : "ERR unsupported"));
            assert(buffer.refs == 0 && probe.buffers_[0][0].kind == BufferKind::None);
            assert(!probe.releasePending_[0][0]);
        }
        assert(probes == 2);
        AHardwareBuffer fallback;
        testReceivedHardwareBuffer = &fallback;
        assert(probe.handleBufferLine(channel[0], "BUFFER eye=0 index=0 w=100 h=100"));
        assert(readLine(channel[1], reply) && reply == "OK");
        assert(readLine(channel[1], reply) && reply == "OK stored");
        assert(probe.buffers_[0][0].bufferLayer == 0 && fallback.refs == 1);
        probe.releaseEye(0); assert(fallback.refs == 0);
        AHardwareBuffer array;
        testReceivedHardwareBuffer = &array;
        assert(probe.handleBufferLine(channel[0], "BUFFER eye=1 index=3 w=100 h=100 layer=1 packing=1"));
        assert(readLine(channel[1], reply) && reply == "OK");
        assert(readLine(channel[1], reply) && reply == "OK stored");
        assert(probe.buffers_[1][3].bufferLayer == 1 && probe.buffers_[1][3].foveatedPacked);
        probe.releaseEye(1); assert(array.refs == 0);
        close(channel[0]); close(channel[1]);
    }

    WindowsFrameTransport transport;
    int wake=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK); assert(wake>=0);
    assert(transport.setFrameWakeFd(wake));
    auto woke=[&] { pollfd fd{wake,POLLIN,0}; return poll(&fd,1,0)==1; };
    AHardwareBuffer buffers[2][3];
    for (int eye = 0; eye < 2; ++eye)
        for (int index = 0; index < 3; ++index)
            transport.storeEyeBuffer(eye, &buffers[eye][index], 100, 100, index, false);
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    auto frame = [&](int eye, int index, int id) {
        std::string line = "FRAME eye=" + std::to_string(eye) + " index=" + std::to_string(index) +
            " frame=" + std::to_string(id) + " projection=1 target=1000000000";
        if(!transport.handleFrameLine(sockets[0], line)) { fprintf(stderr,"frame eye=%d id=%d fd=%d: %s\n",eye,id,sockets[0],strerror(errno)); abort(); }
        char reply[64]; assert(read(sockets[1], reply, sizeof(reply)) > 0);
    };
    std::array<EyeFrame, 2> pair{};
    assert(!transport.pollStereo(pair));
    frame(0, 0, 1);
    assert(!woke());
    assert(!transport.pollStereo(pair));
    assert(!transport.latestClaimed_[0]);
    frame(1, 0, 1);
    assert(woke());
    uint64_t notifications=0; assert(read(wake,&notifications,sizeof(notifications))==sizeof(notifications));
    assert(notifications==1 && !woke());
    int fence = open("/dev/null", O_RDONLY); assert(fence >= 0);
    transport.latest_[0].acquireFenceFd = fence;
    assert(transport.pollStereo(pair));
    assert(pair[0].frameId == 1 && pair[1].frameId == 1);
    assert(pair[0].receivedAt>0 && pair[1].receivedAt>=pair[0].receivedAt);
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
    assert(pair[0].arrivalInterval > 0 && pair[0].arrivalInterval == pair[1].arrivalInterval);
    assert(buffers[0][0].refs == 1 && buffers[1][0].refs == 1);
    assert(!transport.pollStereo(pair));
    transport.resetEye(0);
    assert(!transport.pollStereo(pair) && !transport.hasStereoContent());
    transport.resetEye(1);
    frame(1, 0, 4); // Either arrival order is valid.
    assert(!transport.pollStereo(pair));
    frame(0, 0, 4);
    assert(transport.pollStereo(pair) && pair[0].frameId == 4 && pair[1].frameId == 4);
    assert(pair[0].arrivalInterval == 0 && pair[1].arrivalInterval == 0); // reset clears cadence
    // Multiple complete pairs arrive without consumption. The latest snapshot
    // still carries an arrival-based cadence, measured before pollStereo.
    frame(0, 1, 5); frame(1, 1, 5);
    assert(transport.latest_[0].arrivalInterval > 0);
    frame(1, 2, 6); frame(0, 2, 6);
    const auto measured = transport.latest_[0].arrivalInterval;
    assert(transport.pollStereo(pair) && pair[0].frameId == 6);
    assert(measured > 0 && pair[0].arrivalInterval == measured && pair[1].arrivalInterval == measured);
    // Early announcements preserve the newest *ready* pair behind unfinished
    // work. Test actual SCM_RIGHTS delivery, both arrival orders, and retirement.
    {
        WindowsFrameTransport early;
        AHardwareBuffer images[2][6];
        for (int e=0;e<2;++e) for (int i=0;i<6;++i)
            early.storeEyeBuffer(e,&images[e][i],100,100,i,false);
        int pipe[2]; assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pipe));
        auto announce = [&](int e,int i,int id,int fd) {
            assert(sendFd(pipe[1],fd));
            std::string line="FRAME eye="+std::to_string(e)+" index="+std::to_string(i)+
                " frame="+std::to_string(id)+" fence=1";
            assert(early.handleFrameLine(pipe[0],line));
            std::string reply; assert(readLine(pipe[1],reply)&&reply=="OK");
            assert(readLine(pipe[1],reply)&&reply=="OK stored");
        };
        int ready=eventfd(1,EFD_CLOEXEC|EFD_NONBLOCK);
        int waiting=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);
        assert(ready>=0 && waiting>=0);
        announce(0,0,100,ready); announce(1,0,100,ready);
        announce(0,1,101,waiting);
        assert(early.pollStereo(pair,true) && pair[0].frameId==100); // partial newer pair
        for(int e=0;e<2;++e) { close(pair[e].acquireFenceFd); early.publishReleaseFence(e,0,-1); }
        announce(1,1,101,waiting);
        assert(!early.pollStereo(pair,true));
        announce(1,2,102,waiting); announce(0,2,102,waiting);
        assert(!early.pollStereo(pair,true));
        uint64_t one=1; assert(write(waiting,&one,sizeof(one))==sizeof(one));
        assert(early.pollStereo(pair,true) && pair[0].frameId==102);
        for(int e=0;e<2;++e) {
            close(pair[e].acquireFenceFd); early.publishReleaseFence(e,2,-1);
            assert(early.pending_[e].empty() && !early.releasePending_[e][1]);
        }
        // Bounded history drops old unclaimed ownership; reset closes all FDs.
        for(int i=0;i<6;++i) { announce(0,i,103+i,ready); announce(1,i,103+i,ready); }
        assert(early.pending_[0].size()==4 && !early.releasePending_[0][0]);
        assert(early.pollStereo(pair,true) && pair[0].frameId==108);
        for(int e=0;e<2;++e) { close(pair[e].acquireFenceFd); early.resetEye(e); }
        assert(early.pending_[0].empty() && early.pending_[1].empty());
        early.stop();
        for (auto &eyes : images) for (auto &image : eyes) assert(image.refs==0);
        close(ready); close(waiting); close(pipe[0]); close(pipe[1]);
    }
    // Non-blocking presentation leaves both acquire fences and ownership intact.
    frame(0, 0, 7); frame(1, 0, 7);
    int left = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    int right = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    assert(left >= 0 && right >= 0);
    transport.latest_[0].acquireFenceFd = left;
    transport.latest_[1].acquireFenceFd = right;
    assert(!transport.pollStereo(pair, true));
    assert(!transport.latestClaimed_[0] && !transport.latestClaimed_[1]);
    assert(transport.latest_[0].acquireFenceFd == left);
    uint64_t signal = 1;
    assert(write(left, &signal, sizeof(signal)) == sizeof(signal));
    assert(!transport.pollStereo(pair, true)); // Both eyes must be complete.
    assert(write(right, &signal, sizeof(signal)) == sizeof(signal));
    assert(transport.pollStereo(pair, true) && pair[0].frameId == 7);
    assert(pair[0].acquireFenceFd == left && pair[1].acquireFenceFd == right);
    close(left); close(right);
    transport.publishReleaseFence(0, 0, -1); transport.publishReleaseFence(1, 0, -1);
    assert(!transport.pollStereo(pair, true));
    frame(0, 1, 8); frame(1, 1, 8);
    left = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    transport.latest_[0].acquireFenceFd = left;
    assert(!transport.pollStereo(pair, true));
    frame(1, 2, 9); frame(0, 2, 9); // Supersede an unready pair without leaking its fence.
    assert(fcntl(left, F_GETFD) == -1 && errno == EBADF);
    assert(!transport.releasePending_[0][1] && !transport.releasePending_[1][1]);
    assert(transport.pollStereo(pair, true) && pair[0].frameId == 9);
    assert(transport.setFrameWakeFd(-1));
    close(wake); // unregister owns its duplicate; no use of a closed caller descriptor
    transport.stop();
    for (auto &eyes : buffers) for (auto &buffer : eyes) assert(buffer.refs == 0);
    close(sockets[0]); close(sockets[1]);
}
