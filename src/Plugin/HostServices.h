#pragma once

#include "Core/Types.h"

namespace stagehand {

// Callbacks a HostedPlugin uses to talk back to the host, passed in at creation.
// Deliberately minimal for now; GUI-resize, timer, and thread-pool callbacks are
// added with their features.
class HostServices {
public:
    virtual ~HostServices() = default;

    // Ask the host to deactivate and reactivate the plugin (e.g. latency change).
    // Thread: main; the host schedules it, not necessarily synchronously.
    virtual void requestRestart() = 0;

    // Ask the host to schedule a process() call (e.g. to flush a tail).
    // Thread: any.
    virtual void requestProcess() = 0;

    // Ask the host to call flushParams() while the plugin is not processing.
    // Thread: any.
    virtual void requestParamFlush() = 0;

    // Host-side logging. Thread: any.
    virtual void log(LogLevel level, const char* message) = 0;
};

} // namespace stagehand
