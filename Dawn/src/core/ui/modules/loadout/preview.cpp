// SPDX-License-Identifier: GPL-3.0-only
// Icon layer layouts adapted from Sundial by KyleThmpsn; see vendor/sundial/NOTICE.md.
#include "preview.h"
#include <Windows.h>
#include "client/content/items/packages/internal.h"
#include "client/hooks/bootflow/bc7_codec.h"
#include "core/filesystem/path.h"
#include "state/editor/localized_strings.h"
#include <d3d11.h>
#include <wincodec.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dawn::core::ui::modules::loadout::preview {
namespace {
namespace packages = client::content::items::packages;
namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;
namespace strings = state::editor::strings;
namespace bc7 = client::hooks::bootflow::bc7;
struct Layer { unsigned width{}, height{}, format{}, pitch{}; std::vector<std::byte> data; };
// The primary layer's size is the icon's own: every other layer is drawn over the same box.
struct Result { std::uint32_t tag{}; std::vector<Layer> layers; bool primary{}; unsigned width{}, height{}; };
struct Texture { std::array<ID3D11ShaderResourceView*, 4> views{}; std::size_t count{}; int lastFrame{}; bool pending{}, failed{}; unsigned width{}, height{}; };
// The last export the worker finished, which the page takes once.
struct Exported { std::uint32_t tag{}; bool saved{}, ready{}; std::string message; };
ID3D11Device* g_device{};
std::unordered_map<std::uint32_t, Texture> g_textures;
std::mutex g_lock;
std::condition_variable g_wake;
std::deque<std::uint32_t> g_requests;
std::deque<Result> g_results;
std::deque<std::uint32_t> g_exports;
Exported g_exported;
// Dawn skips its teardown when a hook cannot come off, and a joinable std::thread destroyed at exit
// calls std::terminate, so a worker `shutdown` never joined is detached instead.
struct Worker { std::thread thread; ~Worker() { if (thread.joinable()) thread.detach(); } };
Worker g_worker;
bool g_stop{};
bool valid_tag(std::uint32_t tag) { return tables::package_of(tag) != tables::kAbsentPackageId; }
bool layer(const reader::Source& source, reader::Scratch& scratch, std::span<const std::byte> container, std::size_t at, Layer& out) {
    std::uint32_t tag{}, textureTag{}, dataTag{}; std::size_t resource{};
    tables::Array lanes{}, textures{};
    std::vector<std::byte> definition, header;
    if (!strings::read(container, at, tag) || !valid_tag(tag)
        || !reader::read_tag(source, scratch, tag, definition)
        || !strings::relative(definition, 0x10, resource)
        || !tables::find_array_at(definition, resource, lanes) || lanes.count > 32
        || !tables::find_array_at(definition, lanes.dataOffset, textures) || textures.count > 32
        || !strings::read(std::span<const std::byte>(definition), textures.dataOffset, textureTag)
        || !reader::read_tag(source, scratch, textureTag, header, dataTag) || !valid_tag(dataTag)
        || !reader::read_tag(source, scratch, dataTag, out.data)) return false;
    std::uint16_t width{}, height{};
    if (!strings::read(std::span<const std::byte>(header), 4, out.format)
        || !strings::read(std::span<const std::byte>(header), 0x0E, width)
        || !strings::read(std::span<const std::byte>(header), 0x10, height)
        || !width || !height || width > 2048 || height > 2048) return false;
    out.width = width; out.height = height;
    unsigned rows = height;
    switch (out.format) {
    case 28: case 29: case 87: case 91: out.pitch = width * 4U; break;
    case 71: case 72: out.pitch = ((width + 3U) / 4U) * 8U; rows = (height + 3U) / 4U; break;
    case 74: case 75: case 77: case 78: case 98: case 99:
        out.pitch = ((width + 3U) / 4U) * 16U; rows = (height + 3U) / 4U; break;
    default: return false;
    }
    const auto length = static_cast<std::size_t>(out.pitch) * rows;
    if (out.data.size() < length) return false;
    out.data.resize(length);
    return true;
}
Result read_icon(std::uint32_t tag, reader::Scratch& scratch) {
    Result result; result.tag = tag;
    reader::BlockKeys keys{};
    struct CleanKeys { reader::BlockKeys& keys; ~CleanKeys() { SecureZeroMemory(&keys, sizeof keys); } } clean{keys};
    core::path::Buffer path{};
    if (!packages::package_directory(path) || !packages::collect_keys(keys)) return result;
    reader::Source source{path.chars.data(), &keys};
    std::vector<std::byte> container;
    if (!reader::read_tag(source, scratch, tag, container)) return result;
    for (const auto at : {0x1CU, 0x14U, 0x20U, 0x24U}) {
        Layer decoded;
        if (layer(source, scratch, container, at, decoded)) {
            if (at == 0x14U) { result.primary = true; result.width = decoded.width; result.height = decoded.height; }
            result.layers.push_back(std::move(decoded));
        }
    }
    return result;
}

// Export: every layer decoded to straight-alpha RGBA8, stacked as the draw list blends them.
struct Image { unsigned width{}, height{}; std::vector<std::uint8_t> rgba; };
void unpack565(unsigned value, std::uint8_t* rgb) {
    rgb[0] = static_cast<std::uint8_t>(((value >> 11U) & 31U) * 255U / 31U);
    rgb[1] = static_cast<std::uint8_t>(((value >> 5U) & 63U) * 255U / 63U);
    rgb[2] = static_cast<std::uint8_t>((value & 31U) * 255U / 31U);
}
// The colour half of a BC1, BC2 or BC3 block. Only BC1 has the three-colour mode with a clear texel.
void decode_color(const std::uint8_t* block, bool punchThrough, std::uint8_t* rgba) {
    const unsigned c0 = block[0] | (static_cast<unsigned>(block[1]) << 8U), c1 = block[2] | (static_cast<unsigned>(block[3]) << 8U);
    std::uint8_t palette[4][4]{};
    unpack565(c0, palette[0]); unpack565(c1, palette[1]);
    const bool four = !punchThrough || c0 > c1;
    for (int channel = 0; channel < 3; ++channel) {
        const unsigned a = palette[0][channel], b = palette[1][channel];
        palette[2][channel] = static_cast<std::uint8_t>(four ? (2U * a + b) / 3U : (a + b) / 2U);
        palette[3][channel] = static_cast<std::uint8_t>(four ? (a + 2U * b) / 3U : 0U);
    }
    palette[0][3] = palette[1][3] = palette[2][3] = 255; palette[3][3] = static_cast<std::uint8_t>(four ? 255U : 0U);
    const std::uint32_t indices = block[4] | (static_cast<std::uint32_t>(block[5]) << 8U)
        | (static_cast<std::uint32_t>(block[6]) << 16U) | (static_cast<std::uint32_t>(block[7]) << 24U);
    for (unsigned texel = 0; texel < 16; ++texel) std::memcpy(rgba + texel * 4U, palette[(indices >> (texel * 2U)) & 3U], 4);
}
// BC3's interpolated alpha block.
void decode_alpha(const std::uint8_t* block, std::uint8_t* rgba) {
    std::uint8_t values[8]{block[0], block[1]};
    const unsigned a = block[0], b = block[1];
    if (a > b) for (unsigned i = 1; i < 7; ++i) values[i + 1] = static_cast<std::uint8_t>(((7U - i) * a + i * b) / 7U);
    else { for (unsigned i = 1; i < 5; ++i) values[i + 1] = static_cast<std::uint8_t>(((5U - i) * a + i * b) / 5U); values[6] = 0; values[7] = 255; }
    std::uint64_t bits = 0;
    for (unsigned i = 0; i < 6; ++i) bits |= static_cast<std::uint64_t>(block[2 + i]) << (8U * i);
    for (unsigned texel = 0; texel < 16; ++texel) rgba[texel * 4U + 3U] = values[(bits >> (3U * texel)) & 7U];
}
bool decode(const Layer& source, Image& image) {
    image.width = source.width; image.height = source.height;
    image.rgba.assign(static_cast<std::size_t>(source.width) * source.height * 4U, 0);
    const auto* data = reinterpret_cast<const std::uint8_t*>(source.data.data());
    if (source.format == 28 || source.format == 29 || source.format == 87 || source.format == 91) {
        const bool bgra = source.format == 87 || source.format == 91;
        for (unsigned y = 0; y < source.height; ++y) for (unsigned x = 0; x < source.width; ++x) {
            const std::uint8_t* in = data + static_cast<std::size_t>(y) * source.pitch + x * 4U;
            std::uint8_t* out = image.rgba.data() + (static_cast<std::size_t>(y) * source.width + x) * 4U;
            out[0] = in[bgra ? 2 : 0]; out[1] = in[1]; out[2] = in[bgra ? 0 : 2]; out[3] = in[3];
        }
        return true;
    }
    const bool bc1 = source.format == 71 || source.format == 72, bc2 = source.format == 74 || source.format == 75;
    const bool bc7 = source.format == 98 || source.format == 99;
    const std::size_t blockBytes = bc1 ? 8U : 16U;
    std::uint8_t texels[bc7::kBlockTexelBytes]{};
    for (unsigned by = 0; by < (source.height + 3U) / 4U; ++by) for (unsigned bx = 0; bx < (source.width + 3U) / 4U; ++bx) {
        const std::uint8_t* block = data + static_cast<std::size_t>(by) * source.pitch + bx * blockBytes;
        if (bc7) bc7::decode_block(block, texels);
        else if (bc1) decode_color(block, true, texels);
        else {
            decode_color(block + 8, false, texels);
            if (bc2) for (unsigned texel = 0; texel < 16; ++texel)
                texels[texel * 4U + 3U] = static_cast<std::uint8_t>(((block[texel / 2U] >> ((texel & 1U) * 4U)) & 15U) * 17U);
            else decode_alpha(block, texels);
        }
        for (unsigned texel = 0; texel < 16; ++texel) {
            const unsigned x = bx * 4U + (texel & 3U), y = by * 4U + (texel >> 2U);
            if (x < source.width && y < source.height)
                std::memcpy(image.rgba.data() + (static_cast<std::size_t>(y) * source.width + x) * 4U, texels + texel * 4U, 4);
        }
    }
    return true;
}
// Lays one layer over what is below it, stretched to the canvas as the draw list stretches it.
void blend(Image& canvas, const Image& layer) {
    for (unsigned y = 0; y < canvas.height; ++y) for (unsigned x = 0; x < canvas.width; ++x) {
        const unsigned sx = x * layer.width / canvas.width, sy = y * layer.height / canvas.height;
        const std::uint8_t* s = layer.rgba.data() + (static_cast<std::size_t>(sy) * layer.width + sx) * 4U;
        std::uint8_t* d = canvas.rgba.data() + (static_cast<std::size_t>(y) * canvas.width + x) * 4U;
        const float sa = static_cast<float>(s[3]) / 255.0F, da = static_cast<float>(d[3]) / 255.0F;
        const float out = sa + (da * (1.0F - sa));
        if (out <= 0.0F) { std::memset(d, 0, 4); continue; }
        for (int c = 0; c < 3; ++c)
            d[c] = static_cast<std::uint8_t>(std::lround(((s[c] * sa) + (d[c] * da * (1.0F - sa))) / out));
        d[3] = static_cast<std::uint8_t>(std::lround(out * 255.0F));
    }
}
template <typename Interface> void release_com(Interface*& object) { if (object) { object->Release(); object = nullptr; } }
// WIC's PNG encoder takes BGRA on every Windows it runs on, so the image is swizzled for it.
bool write_png(const wchar_t* path, const Image& image) {
    std::vector<std::uint8_t> bgra(image.rgba);
    for (std::size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    IWICImagingFactory* factory{}; IWICStream* stream{}; IWICBitmapEncoder* encoder{}; IWICBitmapFrameEncode* frame{};
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    const bool written = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
        && SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE))
        && SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))
        && SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache))
        && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr))
        && SUCCEEDED(frame->SetSize(image.width, image.height)) && SUCCEEDED(frame->SetPixelFormat(&format))
        && IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)
        && SUCCEEDED(frame->WritePixels(image.height, image.width * 4U, static_cast<UINT>(bgra.size()), bgra.data()))
        && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    release_com(frame); release_com(encoder); release_com(stream); release_com(factory);
    return written;
}
bool make_folder(const wchar_t* relative) {
    core::path::Buffer folder{};
    return core::path::artifact_file(relative, folder)
        && (CreateDirectoryW(folder.chars.data(), nullptr) != FALSE || GetLastError() == ERROR_ALREADY_EXISTS);
}
// Reads one icon again, stacks its layers and saves it. Runs on the worker; the page shows `message`.
bool export_icon(std::uint32_t tag, reader::Scratch& scratch, std::string& message) {
    const Result icon = read_icon(tag, scratch);
    if (!icon.primary || icon.width == 0 || icon.height == 0) { message = "This icon has no artwork to export."; return false; }
    Image canvas; canvas.width = icon.width; canvas.height = icon.height;
    canvas.rgba.assign(static_cast<std::size_t>(icon.width) * icon.height * 4U, 0);
    for (const Layer& part : icon.layers) {
        Image decoded;
        if (decode(part, decoded)) blend(canvas, decoded);
    }
    wchar_t relative[64]{};
    (void)std::swprintf(relative, std::size(relative), L"exports\\icons\\0x%08X.png", tag);
    core::path::Buffer file{};
    if (!make_folder(L"exports") || !make_folder(L"exports\\icons") || !core::path::artifact_file(relative, file)
        || !write_png(file.chars.data(), canvas)) { message = "Could not write the PNG to the Dawn folder."; return false; }
    char shown[96]{};
    (void)std::snprintf(shown, sizeof shown, "Saved to Dawn\\exports\\icons\\0x%08X.png", tag);
    message = shown;
    return true;
}

void run() noexcept {
    try {
        // The PNG encoder is a COM object, and this worker is the thread that makes it.
        struct ComScope { HRESULT result; ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); } } com{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
        auto scratch = std::make_unique<reader::Scratch>();
        struct CloseFiles { reader::Scratch& scratch; ~CloseFiles() { reader::close_files(scratch); } } close{*scratch};
        for (;;) {
            std::uint32_t tag{};
            bool exporting = false;
            { std::unique_lock lock(g_lock);
                g_wake.wait(lock, [] { return g_stop || !g_requests.empty() || !g_exports.empty(); });
                if (g_stop) break;
                exporting = !g_exports.empty();
                auto& queue = exporting ? g_exports : g_requests;
                tag = queue.front(); queue.pop_front();
            }
            if (exporting) {
                Exported done{tag, false, true, {}};
                try { done.saved = export_icon(tag, *scratch, done.message); } catch (...) { done.message = "The export failed."; }
                std::lock_guard lock(g_lock); g_exported = std::move(done);
                continue;
            }
            Result result; result.tag = tag;
            try { result = read_icon(tag, *scratch); } catch (...) { /* Return a failed preview for this item. */ }
            { std::unique_lock lock(g_lock);
                g_wake.wait(lock, [] { return g_stop || g_results.size() < 48; });
                if (g_stop) break;
                g_results.push_back(std::move(result));
            }
        }
    } catch (...) {
        std::lock_guard lock(g_lock); g_stop = true; g_requests.clear(); g_exports.clear();
    }
}
void free(Texture& texture) { for (auto*& view : texture.views) if (view) { view->Release(); view = nullptr; } }
void drain() {
    std::deque<Result> results;
    { std::lock_guard lock(g_lock); results.swap(g_results); }
    g_wake.notify_all();
    for (auto& result : results) {
        auto it = g_textures.find(result.tag);
        if (it == g_textures.end() || !g_device) continue;
        auto& texture = it->second; texture.pending = false;
        if (!result.primary) { texture.failed = true; continue; }
        // A device reset can drop an in-flight read and let the same tag be requested again, so a
        // second result can arrive for a tag that already has its views. Those views may be in this
        // frame's draw list already, and freeing them left the renderer drawing released textures;
        // the second result is the same artwork, so it is dropped instead.
        if (texture.count != 0) continue;
        texture.width = result.width; texture.height = result.height;
        for (const auto& data : result.layers) {
            if (texture.count >= texture.views.size()) break;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = data.width; desc.Height = data.height; desc.MipLevels = 1; desc.ArraySize = 1;
            desc.Format = static_cast<DXGI_FORMAT>(data.format); desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA pixels{data.data.data(), data.pitch, 0};
            ID3D11Texture2D* image{}; ID3D11ShaderResourceView* view{};
            if (SUCCEEDED(g_device->CreateTexture2D(&desc, &pixels, &image))) {
                const auto created = g_device->CreateShaderResourceView(image, nullptr, &view);
                image->Release();
                if (SUCCEEDED(created)) texture.views[texture.count++] = view;
            }
        }
        texture.failed = texture.count == 0;
    }
}
void ensure_worker() { if (!g_worker.thread.joinable()) { { std::lock_guard lock(g_lock); g_stop = false; } g_worker.thread = std::thread(run); } }
// Looks one icon up, asking the worker for it the first time. Null until its artwork is in.
const Texture* acquire(std::uint32_t tag) {
    drain();
    ensure_worker();
    const int frame = ImGui::GetFrameCount();
    if (!g_textures.contains(tag) && g_textures.size() >= 256) {
        auto oldest = g_textures.end();
        for (auto it = g_textures.begin(); it != g_textures.end(); ++it)
            if (!it->second.pending && it->second.lastFrame < frame - 1
                && (oldest == g_textures.end() || it->second.lastFrame < oldest->second.lastFrame)) oldest = it;
        if (oldest == g_textures.end()) return nullptr;
        free(oldest->second); g_textures.erase(oldest);
    }
    auto [it, added] = g_textures.try_emplace(tag);
    auto& texture = it->second; texture.lastFrame = frame;
    if (added) {
        std::lock_guard lock(g_lock);
        if (!g_stop && g_requests.size() < 128) { g_requests.push_back(tag); texture.pending = true; g_wake.notify_one(); }
        else { g_textures.erase(it); return nullptr; }
    }
    return texture.count != 0 ? &texture : nullptr;
}
void paint(const Texture& texture, ImVec2 min, ImVec2 max, ImU32 tint) {
    for (std::size_t i = 0; i < texture.count; ++i)
        ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(texture.views[i]), min, max, {0.0F, 0.0F}, {1.0F, 1.0F}, tint);
}
}
void attach(ID3D11Device* device) noexcept { release(); g_device = device; }
bool unavailable(std::uint32_t tag) noexcept {
    const auto it = g_textures.find(tag);
    if (it != g_textures.end() && it->second.failed) return true;
    std::lock_guard lock(g_lock); return g_stop;
}
void release() noexcept {
    for (auto& [tag, texture] : g_textures) { (void)tag; free(texture); }
    g_textures.clear(); g_device = nullptr;
    std::lock_guard lock(g_lock); g_requests.clear(); g_results.clear();
    g_wake.notify_all();
}
void shutdown() noexcept {
    { std::lock_guard lock(g_lock); g_stop = true; }
    g_wake.notify_all();
    if (g_worker.thread.joinable()) g_worker.thread.join();
    { std::lock_guard lock(g_lock); g_requests.clear(); g_results.clear(); g_exports.clear(); }
}
bool draw(std::uint32_t tag, ImVec2 position, float size, ImU32 tint) noexcept {
    if (!g_device || !valid_tag(tag)) return false;
    try {
        const Texture* texture = acquire(tag);
        if (texture == nullptr) return false;
        paint(*texture, position, {position.x + size, position.y + size}, tint);
        return true;
    } catch (...) { return false; }
}
bool draw_fitted(std::uint32_t tag, ImVec2 origin, ImVec2 box, float texel, float largest, ImU32 tint) noexcept {
    if (!g_device || !valid_tag(tag)) return false;
    try {
        const Texture* texture = acquire(tag);
        if (texture == nullptr || texture->width == 0 || texture->height == 0) return false;
        const float width = static_cast<float>(texture->width) * texel, height = static_cast<float>(texture->height) * texel;
        const float scale = (std::min)({box.x / width, box.y / height, largest});
        const ImVec2 size{width * scale, height * scale};
        const ImVec2 min{std::floor(origin.x + ((box.x - size.x) * 0.5F)), std::floor(origin.y + ((box.y - size.y) * 0.5F))};
        paint(*texture, min, {min.x + size.x, min.y + size.y}, tint);
        return true;
    } catch (...) { return false; }
}
bool details(std::uint32_t tag, Details& output) noexcept {
    const auto it = g_textures.find(tag);
    if (it == g_textures.end() || it->second.count == 0) return false;
    output = {it->second.width, it->second.height, static_cast<unsigned>(it->second.count)};
    return true;
}
bool request_export(std::uint32_t tag) noexcept {
    if (!valid_tag(tag)) return false;
    try {
        ensure_worker();
        std::lock_guard lock(g_lock);
        if (g_stop || g_exports.size() >= 8) return false;
        g_exports.push_back(tag);
        g_wake.notify_one();
        return true;
    } catch (...) { return false; }
}
bool take_export(std::uint32_t& tag, bool& saved, std::string& message) noexcept {
    try {
        std::lock_guard lock(g_lock);
        if (!g_exported.ready) return false;
        tag = g_exported.tag; saved = g_exported.saved; message = std::move(g_exported.message);
        g_exported = {};
        return true;
    } catch (...) { return false; }
}
}
