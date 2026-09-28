#pragma once

#include <ORGModuleServices/Async/SerializedTaskPump.h>

namespace br {
// Source-compatible facade; coordination semantics are shared with plugin consumers.
using SerializedTaskPump = org::async::SerializedTaskPump;
}
