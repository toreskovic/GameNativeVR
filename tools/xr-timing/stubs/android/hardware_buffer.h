#pragma once
#include <cassert>
struct AHardwareBuffer { int refs = 1; };
inline void AHardwareBuffer_acquire(AHardwareBuffer *p) { ++p->refs; }
inline void AHardwareBuffer_release(AHardwareBuffer *p) { assert(p->refs > 0); --p->refs; }
inline int AHardwareBuffer_recvHandleFromUnixSocket(int, AHardwareBuffer **) { return -1; }
