#include "graphics_request_job.h"
#include "direct_compute_cache.h"
#include "shader_compute_request.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <optional>
#include <ostream>

namespace fs = std::filesystem;

void x2w::publish_atomic(const fs::path &destination, const void *data, size_t size) {
    static std::atomic<unsigned long long> serial{};
    auto temp = destination;
    temp += L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(serial++) + L".tmp";
    try {
        std::ofstream file(temp, std::ios::binary);
        file.exceptions(std::ios::badbit | std::ios::failbit);
        if (size) file.write(static_cast<const char *>(data), size);
        file.close();
        if (!MoveFileExW(temp.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING))
            throw std::runtime_error("could not publish output");
    } catch (...) {
        std::error_code ignored;
        fs::remove(temp, ignored);
        throw;
    }
}

std::vector<unsigned char> x2w::compile_to_dxil(ShaderDxilCompiler &compiler, const std::string &source,
                                                const std::wstring &profile, const fs::path &cache, bool direct,
                                                std::ostream &report) {
    const auto start = std::chrono::steady_clock::now();
    auto output = compiler.compile(source, profile, cache, direct ? ShaderSourceFormat::dxil_ir : ShaderSourceFormat::hlsl);
    report << (output.cache_hit ? "cache_hit_ms=" : "cold_compile_and_publish_ms=")
           << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
           << " bytes=" << output.dxil.size() << '\n';
    return std::move(output.dxil);
}

// The pair translator does not say which stage it refused. A container stage that is refused on its own for the
// same reason names it; the vertex stage is lowered first, so it is asked first.
static const char *refused_stage(const x2w::GraphicsRequest &input, const char *reason) {
    const auto alone = [&](const std::string &bytes, bool pixel) {
        try {
            x2w::translate_graphics_container(bytes, pixel);
            return false;
        } catch (const std::exception &error) {
            return !std::strcmp(error.what(), reason);
        }
    };
    if (input.a.compare(0, 4, "DXBC") || input.b.compare(0, 4, "DXBC")) return "pair";
    return alone(input.a, false) ? "vertex" : alone(input.b, true) ? "pixel" : "pair";
}

std::string x2w::compile_graphics_request(ShaderDxilCompiler &compiler, const fs::path &marker, const fs::path &stem,
                                          const fs::path &cache, GraphicsRequestMode mode, std::ostream &report) {
    const bool rectangles = mode == GraphicsRequestMode::rectangles, runtime = mode == GraphicsRequestMode::runtime;
    const auto part = [&](const wchar_t *suffix) { return fs::path(stem) += suffix; };
    const char *stage = "request";
    GraphicsRequest input;
    try {
        std::error_code ignored;
        if (!runtime) fs::remove(part(L".txt"), ignored);
        input = read_graphics_request(marker);
        stage = "pair";
        bool hit = false;
        auto pair = prepare_direct_graphics(input.a, input.b, cache, hit, input.root, input.input_layout, input.pixel_entry);
        report << "graphics_pair frontend_cache_hit=" << hit << '\n';
        auto manifest = rectangles ? std::nullopt : format_graphics_runtime_manifest(pair);
        if (!manifest && runtime)
            throw std::runtime_error("the pair's vertex inputs, target types or bindings have no runtime manifest");
        std::vector<unsigned char> gs_dxil;
        if (rectangles)
            gs_dxil = compile_to_dxil(compiler, native_rectangle_geometry(pair.vertex.outputs, pair.pixel.inputs), L"gs_6_6",
                                      cache, true, report);
        stage = "vertex";
        auto vs_dxil = compile_to_dxil(compiler, pair.vertex.dxil_ir, L"vs_6_6", cache, true, report);
        stage = "pixel";
        auto ps_dxil = compile_to_dxil(compiler, pair.pixel.dxil_ir, L"ps_6_6", cache, true, report);
        stage = "publish";
        if (rectangles) publish_atomic(part(L".gs.cso"), gs_dxil.data(), gs_dxil.size());
        publish_atomic(part(L".vs.cso"), vs_dxil.data(), vs_dxil.size());
        publish_atomic(part(L".ps.cso"), ps_dxil.data(), ps_dxil.size());
        if (!manifest) return {};
        if (!input.pixel_input_description.empty())
            publish_atomic(part(L".psdep"), input.pixel_input_description.data(), input.pixel_input_description.size());
        publish_atomic(part(L".txt"), manifest->data(), manifest->size());
        return *manifest;
    } catch (const std::exception &error) {
        if (runtime && !std::strcmp(stage, "pair")) stage = refused_stage(input, error.what());
        throw GraphicsRequestRefusal(stage, error.what());
    }
}
