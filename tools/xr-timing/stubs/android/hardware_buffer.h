#pragma once
#include <cassert>
struct AHardwareBuffer { int refs = 1; };
inline void AHardwareBuffer_acquire(AHardwareBuffer *p) { ++p->refs; }
inline void AHardwareBuffer_release(AHardwareBuffer *p) { assert(p->refs > 0); --p->refs; }
inline AHardwareBuffer *testReceivedHardwareBuffer = nullptr;
inline int AHardwareBuffer_recvHandleFromUnixSocket(int, AHardwareBuffer **out) {
    if (!testReceivedHardwareBuffer) return -1;
    *out = testReceivedHardwareBuffer; testReceivedHardwareBuffer = nullptr; return 0;
}
