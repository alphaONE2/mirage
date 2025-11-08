#include <d3d11.h>
#include <dxgi1_2.h>
#include <imgui.h>
#include <ini.h>
#include <reshade.hpp>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

extern "C" decltype(*std::declval<HINSTANCE>()) __ImageBase; // NOLINT(*-reserved-identifier)

namespace
{

constexpr auto CLASS_NAME = L"mirage-2c0da0a5-8d8c-44eb-9e4a-f9230ed96691";
constexpr auto WINDOW_TITLE = L"Mirage";

// global state
HWND owner_window;
HWND mirror_window;
IDXGISwapChain1* mirror_swap_chain;
std::int32_t hidden_x;
std::int32_t hidden_y;
bool multisampled;

// settings
std::filesystem::path settings_path;
bool after_effects = true;
bool hide_window = true;
std::int32_t visible_x = 0;
std::int32_t visible_y = 0;

HINSTANCE current_instance() { return &__ImageBase; }

void save_settings()
{
    reshade::log::message(reshade::log::level::info, "Saving settings...");
    ini::File ini_file;

    auto& section = ini_file["mirage"];
    section.set("after_effects", after_effects);
    section.set("hide_window", hide_window);
    section.set("window_x", visible_x);
    section.set("window_y", visible_y);

    ini_file.write(settings_path);
}

void load_settings(HMODULE addon_module)
{
    reshade::log::message(reshade::log::level::info, "Loading settings...");

    std::vector<WCHAR> buffer;
    DWORD result;
    do {
        buffer.resize(buffer.size() + MAX_PATH);
        result = GetModuleFileNameW(addon_module, buffer.data(), buffer.size());
    }
    while (result >= buffer.size());
    settings_path = buffer.data();
    settings_path.replace_extension(L".ini");

    auto const message = "Settings path: " + settings_path.generic_string();
    reshade::log::message(reshade::log::level::info, message.c_str());

    try
    {
        auto ini_file = ini::open(settings_path);

        auto const& section = ini_file["mirage"];
        if (section.has_key("after_effects")) { after_effects = section.get<bool>("after_effects"); }
        if (section.has_key("hide_window")) { hide_window = section.get<bool>("hide_window"); }
        if (section.has_key("window_x")) { visible_x = section.get<int>("window_x"); }
        if (section.has_key("window_y")) { visible_y = section.get<int>("window_y"); }
    }
    catch (std::invalid_argument&)
    {}

    save_settings();
}

void display_changed()
{
    RECT display_rect;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR, HDC, LPRECT rect, LPARAM param) -> BOOL
        {
            auto const param_rect = reinterpret_cast<RECT*>(param);
            UnionRect(param_rect, param_rect, rect);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&display_rect));
    hidden_x = display_rect.left;
    hidden_y = display_rect.bottom + 100;

    if (hide_window)
    {
        SetWindowPos(mirror_window, HWND_BOTTOM, hidden_x, hidden_y, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

extern "C" LRESULT wnd_proc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CLOSE: return 0;
    case WM_DISPLAYCHANGE:
        if (hWnd == owner_window) { display_changed(); }
        return 0;
    default: return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

void init_device(reshade::api::device*);
void destroy_device(reshade::api::device*);
void init_swapchain(reshade::api::swapchain*, bool);
void reshade_begin_effects(reshade::api::effect_runtime*, reshade::api::command_list*, reshade::api::resource_view,
    reshade::api::resource_view);
void reshade_finish_effects(reshade::api::effect_runtime*, reshade::api::command_list*, reshade::api::resource_view,
    reshade::api::resource_view);
void draw_settings(reshade::api::effect_runtime* runtime);

void init_device(reshade::api::device* device)
{
    if (device->get_api() != reshade::api::device_api::d3d11) { return; }
    if (owner_window != nullptr || mirror_window != nullptr) { return; }

    WNDCLASSEXW class_descriptor{
        .cbSize = sizeof class_descriptor,
        .lpfnWndProc = &wnd_proc,
        .hInstance = current_instance(),
        .hCursor = LoadCursorW(nullptr, IDC_ARROW),
        .hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)),
        .lpszClassName = CLASS_NAME,
    };
    RegisterClassExW(&class_descriptor);

    owner_window = CreateWindowExW(
        0, CLASS_NAME, WINDOW_TITLE, WS_POPUP, 0, 0, 0, 0, HWND_MESSAGE, nullptr, current_instance(), nullptr);
    mirror_window = CreateWindowExW(
        0, CLASS_NAME, WINDOW_TITLE, WS_POPUP, 0, 0, 0, 0, owner_window, nullptr, current_instance(), nullptr);

    display_changed();

    auto const window_x = hide_window ? hidden_x : visible_x;
    auto const window_y = hide_window ? hidden_y : visible_y;
    auto const flags = SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW | (hide_window ? SWP_NOACTIVATE : 0);
    SetWindowPos(mirror_window, HWND_BOTTOM, window_x, window_y, 0, 0, flags);

    reshade::register_event<reshade::addon_event::destroy_device>(destroy_device);
    reshade::register_event<reshade::addon_event::init_swapchain>(init_swapchain);
    reshade::register_event<reshade::addon_event::reshade_begin_effects>(reshade_begin_effects);
    reshade::register_event<reshade::addon_event::reshade_finish_effects>(reshade_finish_effects);
    reshade::register_overlay(nullptr, draw_settings);
}

void destroy_device(reshade::api::device*)
{
    mirror_swap_chain->Release();

    DestroyWindow(owner_window);

    UnregisterClassW(CLASS_NAME, current_instance());

    save_settings();
}

void init_swapchain(reshade::api::swapchain* swapchain, bool resize)
{
    if (swapchain->get_hwnd() == mirror_window) { return; }

    auto const source_swap_chain = reinterpret_cast<IDXGISwapChain1*>(swapchain->get_native());

    DXGI_SWAP_CHAIN_DESC1 source_desc;
    source_swap_chain->GetDesc1(&source_desc);

    if (resize) { mirror_swap_chain->ResizeBuffers(0, source_desc.Width, source_desc.Height, source_desc.Format, 0); }
    else
    {
        ID3D11Device* device;
        IDXGIFactory2* factory;

        source_swap_chain->GetDevice(IID_ID3D11Device, reinterpret_cast<void**>(&device));
        CreateDXGIFactory1(IID_IDXGIFactory2, reinterpret_cast<void**>(&factory));

        auto mirror_desc = source_desc;
        mirror_desc.Stereo = FALSE;
        mirror_desc.SampleDesc = {.Count = 1};
        mirror_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        mirror_desc.BufferCount = 2;
        mirror_desc.Scaling = DXGI_SCALING_NONE;
        mirror_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        mirror_desc.Flags = 0;

        factory->CreateSwapChainForHwnd(device, mirror_window, &mirror_desc, nullptr, nullptr, &mirror_swap_chain);
        multisampled = source_desc.SampleDesc.Count > 1;

        factory->Release();
        device->Release();
    }

    RECT client_rect;
    GetClientRect(static_cast<HWND>(swapchain->get_hwnd()), &client_rect);
    SetWindowPos(mirror_window, HWND_BOTTOM, 0, 0, client_rect.right, client_rect.bottom,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void reshade_begin_effects(reshade::api::effect_runtime* runtime, reshade::api::command_list*,
    reshade::api::resource_view rtv, reshade::api::resource_view)
{
    if (after_effects || runtime->get_hwnd() == mirror_window) { return; }

    auto const context = reinterpret_cast<ID3D11DeviceContext*>(runtime->get_command_queue()->get_native());
    auto const source_view = reinterpret_cast<ID3D11View*>(rtv.handle);

    ID3D11Resource* source;
    ID3D11Resource* mirror;
    source_view->GetResource(&source);
    mirror_swap_chain->GetBuffer(0, IID_ID3D11Resource, reinterpret_cast<void**>(&mirror));

    context->CopyResource(mirror, source);

    mirror->Release();
    source->Release();

    mirror_swap_chain->Present(0, 0);
}

void reshade_finish_effects(reshade::api::effect_runtime* runtime, reshade::api::command_list*,
    reshade::api::resource_view rtv, reshade::api::resource_view)
{
    if (!after_effects || runtime->get_hwnd() == mirror_window) { return; }

    auto const context = reinterpret_cast<ID3D11DeviceContext*>(runtime->get_command_queue()->get_native());
    auto const source_view = reinterpret_cast<ID3D11View*>(rtv.handle);

    ID3D11Resource* source;
    ID3D11Resource* mirror;
    source_view->GetResource(&source);
    mirror_swap_chain->GetBuffer(0, IID_ID3D11Resource, reinterpret_cast<void**>(&mirror));

    context->CopyResource(mirror, source);

    mirror->Release();
    source->Release();

    mirror_swap_chain->Present(0, 0);
}

void draw_settings(reshade::api::effect_runtime* runtime)
{
    auto settings_changed = ImGui::Checkbox("Mirror after effects", &after_effects);
    auto update_window = ImGui::Checkbox("Hide window", &hide_window);
    bool const activate = update_window && hide_window;
    ImGui::Indent();
    ImGui::BeginDisabled(hide_window);
    ImGui::Text("Window position");
    update_window |= ImGui::InputInt("X", &visible_x);
    update_window |= ImGui::InputInt("Y", &visible_y);
    ImGui::EndDisabled();
    ImGui::Unindent();
    settings_changed |= update_window;

    if (update_window)
    {
        auto const window_x = hide_window ? hidden_x : visible_x;
        auto const window_y = hide_window ? hidden_y : visible_y;
        auto const flags = SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW | (activate ? 0 : SWP_NOACTIVATE);
        SetWindowPos(mirror_window, HWND_BOTTOM, window_x, window_y, 0, 0, flags);
    }

    if (settings_changed) { save_settings(); }
}

} // namespace

extern "C"
{
    __declspec(dllexport) auto NAME = "Mirage";
    __declspec(dllexport) auto DESCRIPTION =
        "Mirrors the game to a hidden window to aid with screen recording via OBS without overlays.";

    __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module)
    {
        if (!reshade::register_addon(addon_module, reshade_module)) { return false; }

        load_settings(addon_module);

        reshade::register_event<reshade::addon_event::init_device>(init_device);

        return true;
    }

    __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE reshade_module)
    {
        reshade::unregister_addon(addon_module, reshade_module);
    }
}
