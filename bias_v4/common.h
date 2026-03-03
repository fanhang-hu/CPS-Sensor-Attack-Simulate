#ifndef CPS_COMMON_H
#define CPS_COMMON_H

#include <stdint.h>

#define SENSOR_IP "127.0.0.1"
#define SENSOR_PORT 19000
#define META_FILE "/tmp/cps_controller_meta.txt"

typedef struct {
    uint64_t seq;
    double net_value;
    double used_value;
    double control_output;
} controller_snapshot_t;

#endif
