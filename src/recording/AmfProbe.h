#pragma once

#include <string>

namespace fastrecord::recording {

struct AmfProbeResult {
    bool runtimeLibraryFound{false};
    bool sdkBindingsCompiled{false};
    bool contextInitialized{false};
    bool h264Supported{false};
    bool av1Supported{false};
    std::wstring description;
};

AmfProbeResult probeAmf();

} // namespace fastrecord::recording

