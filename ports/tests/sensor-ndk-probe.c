/* SPDX-License-Identifier: Apache-2.0
 * Stock public opaque C API only. No private SensorInfo/Event layout guesses.
 */
#define _GNU_SOURCE
#include <oh_sensor.h>
#include <dlfcn.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define APIS(X) \
 X(OH_Sensor_GetInfos) X(OH_Sensor_CreateInfos) X(OH_Sensor_DestroyInfos) \
 X(OH_SensorInfo_GetName) X(OH_SensorInfo_GetType) X(OH_SensorInfo_GetMinSamplingInterval) X(OH_SensorInfo_GetMaxSamplingInterval) \
 X(OH_Sensor_CreateSubscriptionId) X(OH_Sensor_DestroySubscriptionId) X(OH_SensorSubscriptionId_SetType) \
 X(OH_Sensor_CreateSubscriptionAttribute) X(OH_Sensor_DestroySubscriptionAttribute) \
 X(OH_SensorSubscriptionAttribute_SetSamplingInterval) X(OH_Sensor_CreateSubscriber) \
 X(OH_Sensor_DestroySubscriber) X(OH_SensorSubscriber_SetCallback) \
 X(OH_Sensor_Subscribe) X(OH_Sensor_Unsubscribe) X(OH_SensorEvent_GetType) \
 X(OH_SensorEvent_GetTimestamp) X(OH_SensorEvent_GetData) X(OH_SensorEvent_GetAccuracy)
#define DECLARE(name) static __typeof__(&name) api_##name;
APIS(DECLARE)
static atomic_uint events, errors;
static void callback(Sensor_Event *event)
{
    Sensor_Type type = 0; Sensor_Accuracy accuracy = 0;
    int64_t stamp = 0; float *data = NULL; uint32_t count = 0;
    int a = api_OH_SensorEvent_GetType(event, &type);
    int b = api_OH_SensorEvent_GetTimestamp(event, &stamp);
    int c = api_OH_SensorEvent_GetData(event, &data, &count);
    int d = api_OH_SensorEvent_GetAccuracy(event, &accuracy);
    if (a || b || c || d || type != SENSOR_TYPE_AMBIENT_LIGHT || stamp <= 0 ||
        !data || !count || !isfinite(data[0]) || data[0] < 0) {
        atomic_fetch_add(&errors, 1); return;
    }
    unsigned n = atomic_fetch_add(&events, 1) + 1;
    printf("SENSOR_NDK_EVENT count=%u type=%d timestamp=%lld lux=%.9g length=%u accuracy=%d\n",
        n, type, (long long)stamp, data[0], count, accuracy);
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int absent = argc == 2 && !strcmp(argv[1], "--absent");
    if (argc != 2 || (!absent && strcmp(argv[1], "--watch")) || getuid() != 2000) return 2;
    printf("SENSOR_NDK_UID=%u mode=%s\n", getuid(), argv[1]);
    void *library = dlopen("/system/lib64/ndk/libohsensor.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "SENSOR_NDK_DLOPEN_ERROR %s\n", dlerror()); return 2; }
#define LOAD(name) do { api_##name = (__typeof__(&name))dlsym(library, #name); if (!api_##name) { fprintf(stderr, "MISSING_API %s\n", #name); return 2; } } while (0);
    APIS(LOAD)
    int error = 0;
    if (api_OH_Sensor_GetInfos(NULL, NULL) != SENSOR_PARAMETER_ERROR ||
        api_OH_Sensor_Subscribe(NULL, NULL, NULL) != SENSOR_PARAMETER_ERROR) error = 1;
    uint32_t count = 0;
    int result = api_OH_Sensor_GetInfos(NULL, &count);
    printf("SENSOR_NDK_COUNT result=%d count=%u\n", result, count);
    if (absent && result == SENSOR_SERVICE_EXCEPTION && !count && !error) {
        puts("SENSOR_NDK_LEGACY_EMPTY_LIST_ERROR typed_hdi_empty_required_by_wrapper=1");
        return 0;
    }
    if (result || count > 64) return 2;
    if (absent) {
        printf("SENSOR_NDK_ABSENT count=%u negatives_error=%d\n", count, error);
        return count == 0 && !error ? 0 : 2;
    }
    if (!count) return 2;
    uint32_t allocated = count;
    Sensor_Info **infos = api_OH_Sensor_CreateInfos(allocated);
    if (!infos) return 2;
    result = api_OH_Sensor_GetInfos(infos, &count);
    int light = 0;
    for (uint32_t i = 0; !result && i < count; ++i) {
        Sensor_Type type = 0; char name[128] = {0}; uint32_t length = sizeof(name);
        int a = api_OH_SensorInfo_GetType(infos[i], &type), b = api_OH_SensorInfo_GetName(infos[i], name, &length);
        printf("SENSOR_NDK_INFO index=%u result=%d,%d type=%d name=%s\n", i, a, b, type, name);
        int64_t minimum = -1, maximum = -1;
        int c = api_OH_SensorInfo_GetMinSamplingInterval(infos[i], &minimum);
        int d = api_OH_SensorInfo_GetMaxSamplingInterval(infos[i], &maximum);
        printf("SENSOR_NDK_INTERVAL_NS result=%d,%d min=%lld max=%lld\n", c, d, (long long)minimum, (long long)maximum);
        if (c || d || minimum <= 0 || maximum < minimum) error = 1;
        if (type == SENSOR_TYPE_AMBIENT_LIGHT && (minimum != 200000000 || maximum != 10000000000LL)) error = 1;
        if (a || b) error = 1;
        if (type == SENSOR_TYPE_AMBIENT_LIGHT) light = 1;
    }
    if (api_OH_Sensor_DestroyInfos(infos, allocated) || result || !light || error) return 2;
    Sensor_SubscriptionId *id = api_OH_Sensor_CreateSubscriptionId();
    Sensor_SubscriptionAttribute *attribute = api_OH_Sensor_CreateSubscriptionAttribute();
    Sensor_Subscriber *subscriber = api_OH_Sensor_CreateSubscriber();
    if (!id || !attribute || !subscriber) return 2;
    if (api_OH_SensorSubscriptionId_SetType(id, SENSOR_TYPE_AMBIENT_LIGHT) ||
        api_OH_SensorSubscriptionAttribute_SetSamplingInterval(attribute, 200000000) ||
        api_OH_SensorSubscriber_SetCallback(subscriber, callback)) error = 1;
    int subscribed = api_OH_Sensor_Subscribe(id, attribute, subscriber);
    printf("SENSOR_NDK_READY subscribe=%d error=%d\n", subscribed, error);
    if (!subscribed && !error) for (int i = 0; i < 100; ++i) usleep(250000);
    if (subscribed || api_OH_Sensor_Unsubscribe(id, subscriber)) error = 1;
    usleep(300000); unsigned stopped = atomic_load(&events); usleep(1000000);
    if (stopped != atomic_load(&events) || !stopped || atomic_load(&errors)) error = 1;
    if (api_OH_Sensor_DestroySubscriber(subscriber) || api_OH_Sensor_DestroySubscriptionAttribute(attribute) ||
        api_OH_Sensor_DestroySubscriptionId(id)) error = 1;
    printf("SENSOR_NDK_EXIT events=%u callback_errors=%u result=%d\n", stopped, atomic_load(&errors), error);
    return error;
}
