// CRT-free Windows test executable, run under Wine with test-runtime.py's mock headset.
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#define IMPORT __declspec(dllimport)
#define CALL __attribute__((stdcall))
IMPORT void* CALL LoadLibraryA(const char*);
IMPORT void* CALL GetProcAddress(void*, const char*);
IMPORT int CALL QueryPerformanceCounter(long long*);
IMPORT void CALL ExitProcess(unsigned int);
void* memset(void* p, int c, unsigned long long n) { unsigned char* b=p; while(n--) *b++=c; return p; }
#define CHECK(x) do { if (!(x)) ExitProcess(__LINE__); } while(0)
typedef XrResult (XRAPI_PTR *ToTime)(XrInstance, const long long*, XrTime*);
typedef XrResult (XRAPI_PTR *ToCounter)(XrInstance, XrTime, long long*);
void test_entry(void) {
    void* dll = LoadLibraryA("gamenative_openxr_runtime64.dll");
    CHECK(dll);
    PFN_xrGetInstanceProcAddr get = (PFN_xrGetInstanceProcAddr)GetProcAddress(dll, "xrGetInstanceProcAddr");
    CHECK(get);
    PFN_xrEnumerateInstanceExtensionProperties enumerate;
    CHECK(get(XR_NULL_HANDLE,"xrEnumerateInstanceExtensionProperties",(PFN_xrVoidFunction*)&enumerate)==XR_SUCCESS);
    uint32_t count=0;
    CHECK(enumerate(0,0,&count,0)==XR_SUCCESS && count==5);
    XrExtensionProperties properties[5] = {{0}};
    for (unsigned i=0;i<5;i++) properties[i].type=XR_TYPE_EXTENSION_PROPERTIES;
    CHECK(enumerate(0,4,&count,properties)==XR_ERROR_SIZE_INSUFFICIENT);
    CHECK(enumerate(0,5,&count,properties)==XR_SUCCESS && properties[4].extensionVersion==1);
    PFN_xrCreateInstance create;
    CHECK(get(XR_NULL_HANDLE,"xrCreateInstance",(PFN_xrVoidFunction*)&create)==XR_SUCCESS);
    const char* extensions[]={"XR_KHR_win32_convert_performance_counter_time"};
    XrInstanceCreateInfo info={0}; info.type=XR_TYPE_INSTANCE_CREATE_INFO;
    info.applicationInfo.apiVersion=XR_API_VERSION_1_0;
    info.enabledExtensionCount=1; info.enabledExtensionNames=extensions;
    XrInstance instance=XR_NULL_HANDLE;
    CHECK(create(&info,&instance)==XR_SUCCESS);
    ToTime toTime; ToCounter toCounter;
    CHECK(get(instance,"xrConvertWin32PerformanceCounterToTimeKHR",(PFN_xrVoidFunction*)&toTime)==XR_SUCCESS);
    CHECK(get(instance,"xrConvertTimeToWin32PerformanceCounterKHR",(PFN_xrVoidFunction*)&toCounter)==XR_SUCCESS);
    long long counter, roundTrip; XrTime time, later;
    CHECK(QueryPerformanceCounter(&counter));
    CHECK(toTime(instance,&counter,&time)==XR_SUCCESS && time>0);
    CHECK(toCounter(instance,time,&roundTrip)==XR_SUCCESS);
    CHECK(roundTrip-counter>=-1 && roundTrip-counter<=1);
    counter+=10000000;
    CHECK(toTime(instance,&counter,&later)==XR_SUCCESS && later>time);
    CHECK(toTime(instance,0,&time)==XR_ERROR_VALIDATION_FAILURE);
    CHECK(toCounter(instance,0,&counter)==XR_ERROR_TIME_INVALID);
    CHECK(toCounter(XR_NULL_HANDLE,time,&counter)==XR_ERROR_HANDLE_INVALID);
    PFN_xrDestroyInstance destroy;
    CHECK(get(instance,"xrDestroyInstance",(PFN_xrVoidFunction*)&destroy)==XR_SUCCESS);
    CHECK(destroy(instance)==XR_SUCCESS);
    info.enabledExtensionCount=0;
    CHECK(create(&info,&instance)==XR_SUCCESS);
    PFN_xrVoidFunction disabled=(PFN_xrVoidFunction)1;
    CHECK(get(instance,"xrConvertTimeToWin32PerformanceCounterKHR",&disabled)==XR_ERROR_FUNCTION_UNSUPPORTED && !disabled);
    CHECK(toCounter(instance,time,&counter)==XR_ERROR_FUNCTION_UNSUPPORTED);
    CHECK(destroy(instance)==XR_SUCCESS);
    ExitProcess(0);
}
