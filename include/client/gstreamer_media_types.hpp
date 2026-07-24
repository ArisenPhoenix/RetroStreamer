#pragma once

namespace archstreamer {

struct H264DecoderChoice {
    const char* element = nullptr;
    // Direct3D11 decoders emit D3D11 memory; pair them with d3d11videosink and skip system videoconvert.
    bool d3d11_zero_copy = false;
};

} // namespace archstreamer
