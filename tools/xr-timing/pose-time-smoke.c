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
    // Controller tracking is refreshed independently of cached button/action state.
    gn_actions[0].used = 1;
    gn_actions[0].type = XR_ACTION_TYPE_POSE_INPUT;
    gn_actions[0].active_hands = 3;
    gn_actions[0].component[0] = GN_COMP_GRIP_POSE;
    gn_actions[0].component[1] = GN_COMP_AIM_POSE;
    gn_action_count = 1;
    gn_action_spaces[0].used = 1;
    gn_action_spaces[0].action_idx = 0;
    gn_action_spaces[0].hand = 0;
    gn_action_spaces[0].pose_in_action_space.orientation.w = 1;
    gn_action_spaces[0].pose_in_action_space.position.y = 1;
    gn_action_space_count = 1;
    XrSpace controller = (XrSpace)(GN_ACTSPACE_BASE | 0);
    gn_hands[0].buttons = 123;
    gn_hands[0].grip[4] = 99;
    XrSpaceVelocity velocity = {XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation handLocation = {XR_TYPE_SPACE_LOCATION, &velocity};
    CHECK(gn_xrLocateSpace(controller, space, 4000000000LL, &handLocation) == XR_SUCCESS);
    CHECK(handLocation.pose.position.x == 1 && handLocation.pose.position.y == 1);
    CHECK(handLocation.locationFlags == 15);
    CHECK(velocity.velocityFlags == 3 && velocity.linearVelocity.x == -1); // v=1, omega.z=2, offset.y=1
    CHECK(gn_hands[0].buttons == 123 && gn_hands[0].grip[4] == 99);
    CHECK(gn_xrLocateSpace(controller, space, 4000000000LL, &handLocation) == XR_SUCCESS);
    CHECK(handLocation.pose.position.x == 2); // same-time queries must remain fresh
    gn_action_spaces[0].hand = 1;
    CHECK(gn_xrLocateSpace(controller, space, 4100000000LL, &handLocation) == XR_SUCCESS);
    CHECK(handLocation.pose.position.x == 3);
    CHECK(gn_xrLocateSpace(controller, space, 4200000000LL, &handLocation) == XR_SUCCESS);
    CHECK(handLocation.locationFlags == 1 && velocity.velocityFlags == 2);
    CHECK(gn_xrLocateSpace(controller, space, 4300000000LL, &handLocation) == XR_SUCCESS);
    CHECK(handLocation.locationFlags == 0 && velocity.velocityFlags == 0);
    CHECK(gn_xrLocateSpace(controller, space, 4400000000LL, &handLocation) == XR_ERROR_RUNTIME_FAILURE);
    gn_actions[0].active_hands = 0;
    CHECK(gn_xrLocateSpace(controller, space, 4500000000LL, &handLocation) == XR_SUCCESS);
    CHECK(handLocation.locationFlags == 0); // inactive actions do not issue tracking queries
    // A bounded-submission timeout must not silently disable frame sync and
    // return an unpaced synthetic frame. The next request may recover.
    gn_unix_control_state = -1;
    gn_frame_sync_supported = 1;
    XrFrameWaitInfo wait = {XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame = {XR_TYPE_FRAME_STATE};
    CHECK(gn_xrWaitFrame(gn_session, &wait, &frame) == XR_ERROR_RUNTIME_FAILURE);
    CHECK(gn_frame_sync_supported == 1);
    CHECK(gn_xrWaitFrame(gn_session, &wait, &frame) == XR_SUCCESS);
    CHECK(frame.predictedDisplayTime == 5000000000LL && frame.predictedDisplayPeriod == 11111111);
    ExitProcess(0);
}
