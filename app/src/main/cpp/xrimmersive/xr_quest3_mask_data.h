// SPDX-License-Identifier: CC-BY-SA-4.0
// Adapted from Richard Musil's HMD Geometry Database; Quest 3 capture by knob2001.
// https://risa2000.github.io/hmdgdb/hmd_cfgs/MetaQuest3_Native_R72.html
// Outline digitized from images/MetaQuest3_Native_R72_back.dmx.png (left eye).
// These are traced image coordinates, NOT the original runtime's vertex dump.
// Changes: trace, normalize to the projection rectangle, mirror for right eye.
// No dilation / safety margin. See tools/xr-visibility/README.md and LICENSE-HMDGDB.txt.
#pragma once
#include <openxr/openxr.h>
#include <vector>
#include <algorithm>
namespace xrimmersive {
inline constexpr XrVector2f quest3OutlinePixels[] = {
    {443,319},{667,319},{707,331},{747,338},{803,347},{847,361},{858,375},
    {858,735},{835,757},{805,778},{770,800},{738,818},{702,838},{667,858},
    {627,868},{475,868},{432,847},{398,819},{378,796},{362,774},{352,728},
    {352,540},{361,498},{367,455},{374,417},{384,389},{392,376},{404,355}
};
inline std::vector<XrVector2f> quest3Outline(unsigned eye) {
    std::vector<XrVector2f> result;
    for(auto p:quest3OutlinePixels) {
        const float x=(p.x-352.f)/(858.f-352.f);
        result.push_back({eye?1.f-x:x,1.f-(p.y-319.f)/(868.f-319.f)});
    }
    if(eye) std::reverse(result.begin(),result.end());
    return result;
}
}
