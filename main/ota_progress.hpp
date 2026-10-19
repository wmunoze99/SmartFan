#pragma once

#include <cstdint>

enum class OtaStage : uint8_t {
    idle,
    downloading,
    verifying,
    restarting,
};

struct OtaProgress {
    OtaStage stage = OtaStage::idle;
    uint32_t received_bytes = 0;
    uint32_t total_bytes = 0; // Zero means the server did not provide a size.
};
