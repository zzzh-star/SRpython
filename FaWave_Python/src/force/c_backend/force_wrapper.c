#include "app_ForceOut.h"

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

EXPORT int init_sensor(float* init_v) {
    return ForceSensor_Init(init_v);
}

EXPORT int update_sensor(float* v_in, uint32_t timestamp_ms, ForceResult_t* result) {
    return ForceSensor_Update(v_in, timestamp_ms, result);
}

EXPORT int set_baseline(float* baseline_v) {
    return ForceSensor_SetBaseline(baseline_v);
}

EXPORT int get_baseline(float* baseline_v) {
    return ForceSensor_GetBaseline(baseline_v);
}

EXPORT int set_matrix(float matrix[3][4]) {
    return ForceSensor_SetMatrix(matrix);
}
