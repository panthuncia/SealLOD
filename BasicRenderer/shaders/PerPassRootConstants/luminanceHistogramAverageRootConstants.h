#include "rootConstantEncoding.h"

#ifndef __LUMINANCE_HISTOGRAM_AVERAGE_ROOT_CONSTANTS_H__
#define __LUMINANCE_HISTOGRAM_AVERAGE_ROOT_CONSTANTS_H__

#define MIN_LOG_LUMINANCE ROOT_CONSTANT_AS_FLOAT(UintRootConstant0)
#define LOG_LUMINANCE_RANGE ROOT_CONSTANT_AS_FLOAT(UintRootConstant1)
// UintRootConstant2 is unused: the adaptation rate is the per-frame buffer's deltaTime (M6 step 4).
#define NUM_PIXELS ROOT_CONSTANT_AS_FLOAT(UintRootConstant3)

#endif // __LUMINANCE_HISTOGRAM_AVERAGE_ROOT_CONSTANTS_H__