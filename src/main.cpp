#include "IdeApplication.hpp"

#include <windows.h>
#include <d3d11.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <exception>
#include <memory>

namespace
{
    ID3D11Device* g_device = nullptr;
    ID3D11DeviceContext* g_deviceContext = nullptr;
    IDXGISwapChain* g_swapChain = nullptr;
    ID3D11RenderTargetView* g_renderTarget = nullptr;

    bool g_closeRequested = false;

    void CreateRenderTarget()
    {
        ID3D11Texture2D* backBuffer = nullptr;

        g_swapChain->GetBuffer(
            0,
            IID_PPV_ARGS(&backBuffer)
        );

        if (backBuffer)
        {
            g_device->CreateRenderTargetView(
                backBuffer,
                nullptr,
                &g_renderTarget
            );

            backBuffer->Release();
        }
    }

    void CleanupRenderTarget()
    {
        if (g_renderTarget)
        {
            g_renderTarget->Release();
            g_renderTarget = nullptr;
        }
    }

    bool CreateDevice(HWND window)
    {
        DXGI_SWAP_CHAIN_DESC description{};
        description.BufferCount = 2;
        description.BufferDesc.Format =
            DXGI_FORMAT_R8G8B8A8_UNORM;
        description.BufferUsage =
            DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.OutputWindow = window;
        description.SampleDesc.Count = 1;
        description.Windowed = TRUE;
        description.SwapEffect =
            DXGI_SWAP_EFFECT_DISCARD;

        constexpr D3D_FEATURE_LEVEL featureLevels[] =
        {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_0
        };

        D3D_FEATURE_LEVEL featureLevel{};

        const HRESULT result = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            featureLevels,
            static_cast<UINT>(std::size(featureLevels)),
            D3D11_SDK_VERSION,
            &description,
            &g_swapChain,
            &g_device,
            &featureLevel,
            &g_deviceContext
        );

        if (FAILED(result))
            return false;

        CreateRenderTarget();
        return true;
    }

    void CleanupDevice()
    {
        CleanupRenderTarget();

        if (g_swapChain)
        {
            g_swapChain->Release();
            g_swapChain = nullptr;
        }

        if (g_deviceContext)
        {
            g_deviceContext->Release();
            g_deviceContext = nullptr;
        }

        if (g_device)
        {
            g_device->Release();
            g_device = nullptr;
        }
    }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam
);

LRESULT WINAPI WindowProcedure(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(
        window,
        message,
        wParam,
        lParam))
    {
        return true;
    }

    switch (message)
    {
    case WM_SIZE:
        if (g_device &&
            wParam != SIZE_MINIMIZED)
        {
            CleanupRenderTarget();

            g_swapChain->ResizeBuffers(
                0,
                static_cast<UINT>(LOWORD(lParam)),
                static_cast<UINT>(HIWORD(lParam)),
                DXGI_FORMAT_UNKNOWN,
                0
            );

            CreateRenderTarget();
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU)
            return 0;
        break;

    case WM_CLOSE:
        g_closeRequested = true;
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(
        window,
        message,
        wParam,
        lParam
    );
}

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int showCommand)
{
    ImGui_ImplWin32_EnableDpiAwareness();

    const HRESULT comResult = CoInitializeEx(
        nullptr,
        COINIT_APARTMENTTHREADED
    );

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor =
        LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName =
        L"LuaIDE.ImGui.Window";

    RegisterClassExW(&windowClass);

    HWND window = CreateWindowExW(
        0,
        windowClass.lpszClassName,
        L"LuaIDE 2.0",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1400,
        900,
        nullptr,
        nullptr,
        instance,
        nullptr
    );

    if (!window || !CreateDevice(window))
    {
        MessageBoxW(
            nullptr,
            L"Не удалось инициализировать DirectX 11.",
            L"LuaIDE",
            MB_OK | MB_ICONERROR
        );

        CleanupDevice();

        if (window)
            DestroyWindow(window);

        UnregisterClassW(
            windowClass.lpszClassName,
            instance
        );

        if (SUCCEEDED(comResult))
            CoUninitialize();

        return 1;
    }

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();

    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    io.IniFilename = "LuaIDE.ini";

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 3.0f;
    style.FrameRounding = 3.0f;
    style.TabRounding = 3.0f;

    // Consolas содержит кириллицу и подходит для редактора.
    ImFont* font = io.Fonts->AddFontFromFileTTF(
        "C:/Windows/Fonts/segoeui.ttf",
        17.0f,
        nullptr,
        io.Fonts->GetGlyphRangesCyrillic()
    );

    if (font)
        io.FontDefault = font;

    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device, g_deviceContext);

    bool finished = false;
    int exitCode = 0;

    try
    {
        IdeApplication application;

        while (!finished)
        {
            MSG message{};

            while (PeekMessageW(
                &message,
                nullptr,
                0,
                0,
                PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);

                if (message.message == WM_QUIT)
                    finished = true;
            }

            if (finished)
                break;

            if (g_closeRequested)
            {
                g_closeRequested = false;

                if (application.RequestClose())
                {
                    finished = true;
                    continue;
                }
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            application.Render();

            ImGui::Render();

            constexpr float clearColor[4] =
            {
                0.08f,
                0.08f,
                0.10f,
                1.0f
            };

            g_deviceContext->OMSetRenderTargets(
                1,
                &g_renderTarget,
                nullptr
            );

            g_deviceContext->ClearRenderTargetView(
                g_renderTarget,
                clearColor
            );

            ImGui_ImplDX11_RenderDrawData(
                ImGui::GetDrawData()
            );

            g_swapChain->Present(1, 0);
        }
    }
    catch (const std::exception& error)
    {
        MessageBoxA(
            window,
            error.what(),
            "LuaIDE - Error",
            MB_OK | MB_ICONERROR
        );

        exitCode = 1;
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDevice();
    DestroyWindow(window);

    UnregisterClassW(
        windowClass.lpszClassName,
        instance
    );

    if (SUCCEEDED(comResult))
        CoUninitialize();

    return exitCode;
}