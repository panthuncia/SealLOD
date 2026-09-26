#pragma once

#include <BasicRenderer/Pipeline/DrawWorkload.h>

struct WorkloadCountUpdate {
    DrawWorkloadKey workloadKey;
    unsigned int count = 0;
};
