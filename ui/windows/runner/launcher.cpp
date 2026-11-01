#include "launcher.hpp"
#include "flutter_window.h"
#include "utils.h"

#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>
#include <windows.h>
#include <memory>

std::optional<LauncherConfig> ShowLauncherDialog() {
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool co_initialized = SUCCEEDED(hr);

    std::optional<LauncherConfig> output = std::nullopt;

    {
        flutter::DartProject project(L"data");
        FlutterWindow window(project);

        Win32Window::Point origin(100, 100);
        Win32Window::Size size(420, 360);

        if (!window.Create(L"RDMA Remote Control", origin, size)) {
            if (co_initialized) ::CoUninitialize();
            return std::nullopt;
        }
        window.SetQuitOnClose(true);

        auto messenger = window.GetEngine()->messenger();
        auto channel = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
            messenger, "com.app.launcher/control", &flutter::StandardMethodCodec::GetInstance()
        );

        channel->SetMethodCallHandler([&](const auto& call, auto result) {
            const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());

            if (call.method_name() == "submitAndExit") {
                if (args) {
                    LauncherConfig cfg;

                    auto ip_it = args->find(flutter::EncodableValue("ip"));
                    if (ip_it != args->end() && std::holds_alternative<std::string>(ip_it->second)) {
                        cfg.ip = std::get<std::string>(ip_it->second);
                    }

                    auto mode_it = args->find(flutter::EncodableValue("mode"));
                    if (mode_it != args->end() && std::holds_alternative<bool>(mode_it->second)) {
                        cfg.isServer = std::get<bool>(mode_it->second);
                    }

                    auto test_it = args->find(flutter::EncodableValue("isTest"));
                    if (test_it != args->end() && std::holds_alternative<bool>(test_it->second)) {
                        cfg.isTest = std::get<bool>(test_it->second);
                    }

                    auto compress_it = args->find(flutter::EncodableValue("isCompress"));
                    if (compress_it != args->end() && std::holds_alternative<bool>(compress_it->second)) {
                        cfg.isCompress = std::get<bool>(compress_it->second);
                    }

                    output = cfg;
                }
                result->Success();
                window.Destroy();
            } else {
                result->NotImplemented();
            }
        });

        ::MSG msg;
        while (::GetMessage(&msg, nullptr, 0, 0)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
        }
    }
    if (co_initialized) ::CoUninitialize();

    return output;
}