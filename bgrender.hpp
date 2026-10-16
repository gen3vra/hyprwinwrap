#pragma once

#include <optional>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

// renderWindow(standalone = true) discards every per-window render value, so draw overrides have to be injected into the render pass itself
namespace BgRender
{
    struct SWindowOverrides
    {
        std::optional<double> opacity;
    };

    void installSurfacePassHook(HANDLE handle);
    void setWindowOverrides(const PHLWINDOW &window, const SWindowOverrides &overrides);
    void clearWindowOverrides(const PHLWINDOW &window);
    void reapplyWindowRulesToHiddenWindow(const PHLWINDOW &window);
}
