#pragma once
#include "native_printf.hpp"
#include <functional>
namespace celosia::native {
class BootServices;
class DataMemory;
// High-level SCUS platform bootstrap. memory must be the data owner used to
// construct services; caller serializes compiled game data access. The hooks
// install the real applet lifecycle and synchronize the actual deko3d queue.
// Required provider/I/O failures throw. DBC and USB serial remain unavailable;
// no Source module image, peripheral, RPC server or instruction engine runs.
void initializeScusPlatform(DataMemory &memory,BootServices &services,
    std::function<void()> prepareLifecycle,std::function<void()> synchronizeDisplay,
    PrintSink log={});
}
