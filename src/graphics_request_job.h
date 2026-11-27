#pragma once

#include "shader_dxil_compiler.h"
#include <filesystem>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <vector>

// One graphics request (req_<key>.a and its parts) compiled to the runtime cache entry the stand-in loads:
// <stem>.vs.cso, <stem>.ps.cso and, last, <stem>.txt. Shared by tools/startup/compile_dxil.cpp and the stand-in
// (src/startup_stubs/d3d12_draw.inl), so both publish the same bytes.
namespace x2w {
void publish_atomic(const std::filesystem::path &destination, const void *data, size_t size);
std::vector<unsigned char> compile_to_dxil(ShaderDxilCompiler &compiler, const std::string &source,
                                           const std::wstring &profile, const std::filesystem::path &cache,
                                           bool direct, std::ostream &report);

// what() is the underlying reason, unchanged. stage is where it arose: "request" (the files or their key), "vertex",
// "pixel", "pair" (linkage, root signature or manifest) or "publish". The pair translator's own refusals are "pair"
// too, except in runtime mode for two containers, where the stage that is refused on its own is named.
struct GraphicsRequestRefusal : std::runtime_error {
    GraphicsRequestRefusal(const char *stage, const std::string &reason) : std::runtime_error(reason), stage(stage) {}
    const char *stage;
};

// batch: a stale <stem>.txt is removed first, and the stages are published even when the pair has no runtime
// manifest (the tool's behaviour).
// rectangles: batch, plus <stem>.gs.cso, and never a manifest.
// runtime: nothing is removed, and a pair without a runtime manifest is refused before anything is compiled or
// published. The caller has found no <stem>.txt.
enum class GraphicsRequestMode { batch, rectangles, runtime };

// Returns the published manifest text, empty when none was published. Throws GraphicsRequestRefusal.
std::string compile_graphics_request(ShaderDxilCompiler &compiler, const std::filesystem::path &marker,
                                     const std::filesystem::path &stem, const std::filesystem::path &cache,
                                     GraphicsRequestMode mode, std::ostream &report);
} // namespace x2w
