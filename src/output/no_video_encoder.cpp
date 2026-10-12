// Platforms without a native encoder wired up yet (Windows gets Media
// Foundation in plan step A2).
#include "video_encoder.h"

namespace qcv::output {
std::unique_ptr<VideoEncoder> makePlatformVideoEncoder() { return nullptr; }
}
