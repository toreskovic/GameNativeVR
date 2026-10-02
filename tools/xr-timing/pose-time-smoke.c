// Compile the production runtime into a CRT-free test process; only the GPU session is stubbed.
#include "../../app/src/main/windows/openxr_runtime/gamenative_openxr_runtime.c"
__declspec(dllimport) void __attribute__((stdcall)) ExitProcess(unsigned int);
#define CHECK(x) do { if (!(x)) ExitProcess(__LINE__); } while (0)
void test_entry(void) {
    XrInstance instance;
    XrInstanceCreateInfo create = {XR_TYPE_INSTANCE_CREATE_INFO};
    create.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    CHECK(gn_xrCreateInstance(&create, &instance) == XR_SUCCESS);
    gn_session = (XrSession)1;
    XrReferenceSpaceCreateInfo spaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    spaceInfo.poseInReferenceSpace.orientation.w = 1;
    XrSpace space;
    CHECK(gn_xrCreateReferenceSpace(gn_session, &spaceInfo, &space) == XR_SUCCESS);
    XrViewLocateInfo locate = {XR_TYPE_VIEW_LOCATE_INFO};
    locate.space = space;
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    XrViewState state = {XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    gn_uint32 count;
    locate.displayTime = 1000000000;
    CHECK(gn_xrLocateViews(gn_session, &locate, &state, 2, &count, views) == XR_SUCCESS);
    CHECK(count == 2 && views[0].pose.position.x == 1.0f);
    CHECK(state.viewStateFlags == XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    // Same requested time must re-query instead of reusing FRAME_SYNC's cached pose.
    CHECK(gn_xrLocateViews(gn_session, &locate, &state, 2, &count, views) == XR_SUCCESS);
    CHECK(views[0].pose.position.x == 2.0f);
    locate.displayTime = 2000000000;
    CHECK(gn_xrLocateViews(gn_session, &locate, &state, 2, &count, views) == XR_SUCCESS);
    CHECK(views[0].pose.position.x == 3.0f);
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XrSpace head;
    CHECK(gn_xrCreateReferenceSpace(gn_session, &spaceInfo, &head) == XR_SUCCESS);
    XrSpaceLocation headLocation = {XR_TYPE_SPACE_LOCATION};
    CHECK(gn_xrLocateSpace(head, space, 2500000000LL, &headLocation) == XR_SUCCESS);
    CHECK(headLocation.pose.position.x == 2.0f); // midpoint: left x=4, right x=0
    CHECK(headLocation.locationFlags == XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    locate.displayTime = 0;
    CHECK(gn_xrLocateViews(gn_session, &locate, &state, 2, &count, views) == XR_ERROR_TIME_INVALID);
    locate.displayTime = 3000000000;
    CHECK(gn_xrLocateViews(gn_session, &locate, &state, 2, &count, views) == XR_ERROR_RUNTIME_FAILURE);
    ExitProcess(0);
}
