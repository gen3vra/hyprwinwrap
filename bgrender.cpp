#define WLR_USE_UNSTABLE

#include <hyprland/src/includes.hpp>
#include <any>
#include <sstream>
#include <unordered_map>

#define private public
#define protected public
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#undef private
#undef protected

#include "bgrender.hpp"

namespace
{
    using SRenderData = CSurfacePassElement::SRenderData;
    typedef void (*origSurfacePassElementCtor)(CSurfacePassElement *thisptr, const SRenderData *renderData);

    CFunctionHook *surfacePassElementCtorHook = nullptr;
    std::unordered_map<PHLWINDOW, BgRender::SWindowOverrides> overridesByWindow;

    float resolveOverrideAlpha(const PHLWINDOW &window, const BgRender::SWindowOverrides &overrides)
    {
        if (overrides.opacity)
            return static_cast<float>(*overrides.opacity);

        // The window is never focused while the plugin draws it, so a rule lands in the inactive slot; non-override rules are ignored on purpose
        const auto ruleAlpha = window->m_ruleApplicator->alphaInactive().valueOrDefault();
        return ruleAlpha.overridden ? ruleAlpha.alpha : 1.f;
    }

    void writeOverridesIntoRenderData(SRenderData &renderData, const PHLWINDOW &window, const BgRender::SWindowOverrides &overrides)
    {
        renderData.alpha = resolveOverrideAlpha(window, overrides);
    }

    // renderWindow(standalone = true) resets SRenderData before building pass elements, so the overrides are injected as each element is constructed
    void hookedSurfacePassElementCtor(CSurfacePassElement *thisptr, const SRenderData *renderData)
    {
        const auto originalCtor = (origSurfacePassElementCtor)surfacePassElementCtorHook->m_original;
        const auto found = renderData->pWindow ? overridesByWindow.find(renderData->pWindow) : overridesByWindow.end();
        if (found == overridesByWindow.end())
        {
            originalCtor(thisptr, renderData);
            return;
        }

        SRenderData overridden = *renderData;
        writeOverridesIntoRenderData(overridden, found->first, found->second);
        originalCtor(thisptr, &overridden);
    }
}

void BgRender::installSurfacePassHook(HANDLE handle)
{
    const auto fns = HyprlandAPI::findFunctionsByName(handle, "_ZN19CSurfacePassElementC1ERKNS_11SRenderDataE");
    if (fns.empty())
        throw std::runtime_error("hyprwinwrap: CSurfacePassElement ctor not found");

    surfacePassElementCtorHook = HyprlandAPI::createFunctionHook(handle, fns[0].address, (void *)&hookedSurfacePassElementCtor);
    if (!surfacePassElementCtorHook->hook())
        throw std::runtime_error("hyprwinwrap: CSurfacePassElement ctor hook failed");
}

void BgRender::setWindowOverrides(const PHLWINDOW &window, const SWindowOverrides &overrides)
{
    overridesByWindow[window] = overrides;
}

void BgRender::clearWindowOverrides(const PHLWINDOW &window)
{
    overridesByWindow.erase(window);
}

// Hyprland skips hidden windows in updateAllRules() and propertiesChanged(), and bg windows are always hidden, so rules changed after map need this
void BgRender::reapplyWindowRulesToHiddenWindow(const PHLWINDOW &window)
{
    const bool wasHidden = window->m_hidden;
    window->m_hidden = false;
    window->m_ruleApplicator->propertiesChanged(Desktop::Rule::RULE_PROP_ALL);
    window->m_hidden = wasHidden;
}
