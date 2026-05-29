#define WLR_USE_UNSTABLE

#include <unistd.h>
#include <vector>

#include <hyprland/src/includes.hpp>
#include <any>
#include <sstream>

#define private public
#define protected public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/desktop/rule/Engine.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRule.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/event/EventBus.hpp>
#undef private
#undef protected

#include <unordered_map>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/config/values/ConfigValues.hpp>
#include "globals.hpp"

// Do NOT change this function
APICALL EXPORT std::string PLUGIN_API_VERSION()
{
    return HYPRLAND_API_VERSION;
}

// hooks
inline CFunctionHook *subsurfaceHook = nullptr;
inline CFunctionHook *commitHook = nullptr;
typedef void (*origCommitSubsurface)(Desktop::View::CSubsurface *thisptr);
typedef void (*origCommit)(void *owner, void *data);

std::vector<PHLWINDOWREF> bgWindows;
std::vector<SP<Desktop::Rule::IRule>> bgRules;
std::unordered_map<PHLWINDOW, bool> interactableStates;
bool anyInteractive = false;

static SP<Config::Values::CStringValue> gCfgClass;
static SP<Config::Values::CStringValue> gCfgTitle;
static SP<Config::Values::CStringValue> gCfgSizeX;
static SP<Config::Values::CStringValue> gCfgSizeY;
static SP<Config::Values::CStringValue> gCfgPosX;
static SP<Config::Values::CStringValue> gCfgPosY;

static SP<Desktop::Rule::CWindowRule> makeWindowRule(const std::string &name, const Desktop::Rule::eRuleProperty prop, const std::string &match)
{
    auto rule = makeShared<Desktop::Rule::CWindowRule>(name);
    rule->registerMatch(prop, "^(" + match + ")$");
    rule->addEffect(Desktop::Rule::WINDOW_RULE_EFFECT_FLOAT, "1");
    rule->addEffect(Desktop::Rule::WINDOW_RULE_EFFECT_SIZE, "100% 100%");
    rule->addEffect(Desktop::Rule::WINDOW_RULE_EFFECT_NO_DIM, "1");      // prevent interactive switch causing any brightness change from focus
    rule->addEffect(Desktop::Rule::WINDOW_RULE_EFFECT_BORDER_SIZE, "0"); // prevent border flash when focused interactively
    rule->addEffect(Desktop::Rule::WINDOW_RULE_EFFECT_NO_SHADOW, "1");
    return rule;
}

static void clearWindowRules()
{
    for (auto &rule : bgRules)
    {
        if (rule)
            Desktop::Rule::ruleEngine()->unregisterRule(rule);
    }
    bgRules.clear();
}

static void applyBgWindowGeometry(PHLWINDOW pWindow)
{
    const auto PMONITOR = pWindow->m_monitor.lock();
    if (!PMONITOR)
        return;

    float sx = 100.f, sy = 100.f, px = 0.f, py = 0.f;
    try
    {
        sx = std::stof(gCfgSizeX->value());
    }
    catch (...)
    {
    }
    try
    {
        sy = std::stof(gCfgSizeY->value());
    }
    catch (...)
    {
    }
    try
    {
        px = std::stof(gCfgPosX->value());
    }
    catch (...)
    {
    }
    try
    {
        py = std::stof(gCfgPosY->value());
    }
    catch (...)
    {
    }

    sx = std::clamp(sx, 1.f, 100.f);
    sy = std::clamp(sy, 1.f, 100.f);
    px = std::clamp(px, 0.f, 100.f);
    py = std::clamp(py, 0.f, 100.f);

    if (px + sx > 100.f)
        sx = 100.f - px;
    if (py + sy > 100.f)
        sy = 100.f - py;

    const Vector2D monitorSize = PMONITOR->m_size;
    const Vector2D monitorPos = PMONITOR->m_position;

    const Vector2D newSize = {static_cast<int>(monitorSize.x * (sx / 100.f)), static_cast<int>(monitorSize.y * (sy / 100.f))};
    const Vector2D newPos = {static_cast<int>(monitorPos.x + (monitorSize.x * (px / 100.f))), static_cast<int>(monitorPos.y + (monitorSize.y * (py / 100.f)))};

    const CBox b(newPos.x, newPos.y, newSize.x, newSize.y);
    auto target = pWindow->layoutTarget();
    target->setPositionGlobal(b);
    target->warpPositionSize();

    pWindow->m_realSize->setValueAndWarp(newSize);
    pWindow->m_realPosition->setValueAndWarp(newPos);
    pWindow->sendWindowSize(true);
}

void onNewWindow(PHLWINDOW pWindow)
{
    const std::string classRule(gCfgClass->value());
    const std::string titleRule(gCfgTitle->value());

    const bool classMatches = !classRule.empty() && pWindow->m_initialClass == classRule;
    const bool titleMatches = !titleRule.empty() && pWindow->m_title == titleRule;

    if (!classMatches && !titleMatches)
        return;

    const auto PMONITOR = pWindow->m_monitor.lock();
    if (!PMONITOR)
        return;

    auto target = pWindow->layoutTarget();
    if (!target->floating())
    {
        target->setFloating(true);
        pWindow->m_isFloating = true;
    }

    applyBgWindowGeometry(pWindow);

    pWindow->m_size = pWindow->m_realSize->value();
    pWindow->m_position = pWindow->m_realPosition->value();
    pWindow->m_pinned = true;

    interactableStates[pWindow] = false;
    bgWindows.push_back(pWindow);
    pWindow->m_hidden = true;

    pWindow->m_ruleApplicator->noFocusOverride(Desktop::Types::COverridableVar<bool>(true, Desktop::Types::PRIORITY_SET_PROP));

    g_pInputManager->refocus();
}

void onCloseWindow(PHLWINDOW pWindow)
{
    std::erase_if(bgWindows, [pWindow](const auto &ref)
                  { return ref.expired() || ref.lock() == pWindow; });
    interactableStates.erase(pWindow);
}

void onRenderStage(eRenderStage stage)
{
    if (stage != RENDER_POST_WALLPAPER)
        return;

    for (auto &bg : bgWindows)
    {
        const auto bgw = bg.lock();

        if (bgw->m_monitor != g_pHyprRenderer->m_renderData.pMonitor)
            continue;

        const bool interactable = interactableStates.contains(bgw) ? interactableStates[bgw] : false;
        if (interactable)
            continue; // pinned-always-above Hypr pass handles it on top

        // cant use setHidden cuz that sends suspended and stuff that would be laggy
        bgw->m_hidden = false;
        g_pHyprRenderer->renderWindow(bgw, g_pHyprRenderer->m_renderData.pMonitor.lock(), Time::steadyNow(), false, Render::RENDER_PASS_ALL, false, true);
        bgw->m_hidden = true;
    }
}

void onCommitSubsurface(Desktop::View::CSubsurface *thisptr)
{
    const auto PWINDOW = Desktop::View::CWindow::fromView(thisptr->wlSurface()->view());

    if (!PWINDOW || std::find_if(bgWindows.begin(), bgWindows.end(), [PWINDOW](const auto &ref)
                                 { return ref.lock() == PWINDOW; }) == bgWindows.end())
    {
        ((origCommitSubsurface)subsurfaceHook->m_original)(thisptr);
        return;
    }

    // cant use setHidden cuz that sends suspended and stuff that would be laggy
    PWINDOW->m_hidden = false;

    ((origCommitSubsurface)subsurfaceHook->m_original)(thisptr);
    if (const auto MON = PWINDOW->m_monitor.lock(); MON)
        g_pHyprRenderer->damageMonitor(MON);

    const bool interactable = interactableStates.contains(PWINDOW) ? interactableStates[PWINDOW] : false;
    PWINDOW->m_hidden = !interactable;
}

void onCommit(void *owner, void *data)
{
    const auto PWINDOW = ((Desktop::View::CWindow *)owner)->m_self.lock();

    if (std::find_if(bgWindows.begin(), bgWindows.end(), [PWINDOW](const auto &ref)
                     { return ref.lock() == PWINDOW; }) == bgWindows.end())
    {
        ((origCommit)commitHook->m_original)(owner, data);
        return;
    }

    // cant use setHidden cuz that sends suspended and stuff that would be laggy
    PWINDOW->m_hidden = false;

    ((origCommit)commitHook->m_original)(owner, data);
    if (const auto MON = PWINDOW->m_monitor.lock(); MON)
        g_pHyprRenderer->damageMonitor(MON);

    const bool interactable = interactableStates.contains(PWINDOW) ? interactableStates[PWINDOW] : false;
    PWINDOW->m_hidden = !interactable;
}

SDispatchResult dispatchToggleInteractivity(std::string)
{
    if (bgWindows.empty())
    {
        HyprlandAPI::addNotification(PHANDLE, "[hyprwinwrap] No bg windows",
                                     CHyprColor{1.0, 1.0, 0.2, 1.0}, 2500);
        return SDispatchResult{.success = false, .error = "no background windows"};
    }

    int toggled = 0;
    for (auto &bg : bgWindows)
    {
        const auto bgw = bg.lock();
        if (!bgw)
            continue;

        auto it = interactableStates.find(bgw);
        if (it == interactableStates.end())
            continue;

        it->second = !it->second;
        bgw->m_hidden = !it->second;
        toggled++;

        // TODO: Way to select which bg window to focus? Works fine if you only have one bg window :)
        if (it->second)
        {
            bgw->m_ruleApplicator->noFocusOverride(Desktop::Types::COverridableVar<bool>(false, Desktop::Types::PRIORITY_SET_PROP));
            Desktop::focusState()->fullWindowFocus(bgw, Desktop::FOCUS_REASON_OTHER);
        }
        else
        {
            bgw->m_ruleApplicator->noFocusOverride(Desktop::Types::COverridableVar<bool>(true, Desktop::Types::PRIORITY_SET_PROP));
        }
        anyInteractive = it->second;
    }

    // May not be setting keyboard focus to right window if there are multiple bg windows
    if (toggled > 1)
        HyprlandAPI::addNotification(PHANDLE,
                                     std::string{"[hyprwinwrap] toggled "} + std::to_string(toggled) + " window(s)",
                                     CHyprColor{0.2, 0.8, 1.0, 1.0}, 2500);
    return SDispatchResult{};
}

void onConfigReloaded()
{
    clearWindowRules();

    const std::string classRule(gCfgClass->value());
    if (!classRule.empty())
    {
        auto rule = makeWindowRule("hyprwinwrap-class", Desktop::Rule::RULE_PROP_CLASS, classRule);
        bgRules.emplace_back(rule);
        Desktop::Rule::ruleEngine()->registerRule(SP<Desktop::Rule::IRule>{rule});
    }

    const std::string titleRule(gCfgTitle->value());
    if (!titleRule.empty())
    {
        auto rule = makeWindowRule("hyprwinwrap-title", Desktop::Rule::RULE_PROP_TITLE, titleRule);
        bgRules.emplace_back(rule);
        Desktop::Rule::ruleEngine()->registerRule(SP<Desktop::Rule::IRule>{rule});
    }

    Desktop::Rule::ruleEngine()->updateAllRules();
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle)
{
    PHANDLE = handle;

    const std::string HASH = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH)
    {
        HyprlandAPI::addNotification(PHANDLE, "[hyprwinwrap] Failure in initialization: Version mismatch (headers ver is not equal to running hyprland ver)",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hww] Version mismatch");
    }

    static auto P = Event::bus()->m_events.window.open.listen([&](PHLWINDOW w)
                                                              { onNewWindow(w); });
    static auto P2 = Event::bus()->m_events.window.close.listen([&](PHLWINDOW w)
                                                                { onCloseWindow(w); });
    static auto P3 = Event::bus()->m_events.render.stage.listen([&](eRenderStage stage)
                                                                { onRenderStage(stage); });
    static auto P4 = Event::bus()->m_events.config.reloaded.listen([&]
                                                                   { onConfigReloaded(); });

    // Workspace changes can re-apply gap and border settings, shifting the window slightly
    static auto P5 = Event::bus()->m_events.workspace.active.listen([&](PHLWORKSPACE ws)
                                                                    {
        for (auto &bg : bgWindows) {
            const auto bgw = bg.lock();
            if (!bgw)
                continue;
            applyBgWindowGeometry(bgw);
            //HyprlandAPI::addNotification(PHANDLE, "set pos: " + std::to_string((int)bgw->m_realPosition->value().x) + "," + std::to_string((int)bgw->m_realPosition->value().y), CHyprColor{0.2, 1.0, 0.2, 1.0}, 3000);
        } });

    // Interacting with bg window and change focus, 'toggle' state again
    static auto P6 = Event::bus()->m_events.window.active.listen([&](PHLWINDOW w, Desktop::eFocusReason)
                                                                 {
        if (!anyInteractive)
            return;
        for (auto &bg : bgWindows) {
            const auto bgw = bg.lock();
            if (!bgw || bgw == w)
                continue;
            auto it = interactableStates.find(bgw);
            if (it == interactableStates.end() || !it->second)
                continue;
            it->second = false;
            bgw->m_hidden = true;
            bgw->m_ruleApplicator->noFocusOverride(Desktop::Types::COverridableVar<bool>(true, Desktop::Types::PRIORITY_SET_PROP));
        }
        anyInteractive = false; });
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprwinwrap_interactivity", dispatchToggleInteractivity);

    auto fns = HyprlandAPI::findFunctionsByName(PHANDLE, "_ZN7Desktop4View11CSubsurface8onCommitEv");
    if (fns.size() < 1)
        throw std::runtime_error("hyprwinwrap: onCommit not found");
    subsurfaceHook = HyprlandAPI::createFunctionHook(PHANDLE, fns[0].address, (void *)&onCommitSubsurface);

    fns = HyprlandAPI::findFunctionsByName(PHANDLE, "_ZN7Desktop4View7CWindow12commitWindowEv");
    if (fns.size() < 1)
        throw std::runtime_error("hyprwinwrap: listener_commitWindow not found");
    commitHook = HyprlandAPI::createFunctionHook(PHANDLE, fns[0].address, (void *)&onCommit);

    bool hkResult = subsurfaceHook->hook();
    hkResult = hkResult && commitHook->hook();

    if (!hkResult)
        throw std::runtime_error("hyprwinwrap: hooks failed");

    gCfgClass = makeShared<Config::Values::CStringValue>("plugin:hyprwinwrap:class", "window class to use as background", "kitty-bg");
    HyprlandAPI::addConfigValueV2(PHANDLE, gCfgClass);
    gCfgTitle = makeShared<Config::Values::CStringValue>("plugin:hyprwinwrap:title", "window title to use as background", "");
    HyprlandAPI::addConfigValueV2(PHANDLE, gCfgTitle);

    gCfgSizeX = makeShared<Config::Values::CStringValue>("plugin:hyprwinwrap:size_x", "background window width as percentage (1-100)", "100");
    HyprlandAPI::addConfigValueV2(PHANDLE, gCfgSizeX);
    gCfgSizeY = makeShared<Config::Values::CStringValue>("plugin:hyprwinwrap:size_y", "background window height as percentage (1-100)", "100");
    HyprlandAPI::addConfigValueV2(PHANDLE, gCfgSizeY);
    gCfgPosX = makeShared<Config::Values::CStringValue>("plugin:hyprwinwrap:pos_x", "background window horizontal offset as percentage (0-100)", "0");
    HyprlandAPI::addConfigValueV2(PHANDLE, gCfgPosX);
    gCfgPosY = makeShared<Config::Values::CStringValue>("plugin:hyprwinwrap:pos_y", "background window vertical offset as percentage (0-100)", "0");
    HyprlandAPI::addConfigValueV2(PHANDLE, gCfgPosY);

    onConfigReloaded();

    HyprlandAPI::addNotification(PHANDLE, "[hyprwinwrap] Initialized successfully!", CHyprColor{0.2, 1.0, 0.2, 1.0}, 5000);

    return {"hyprwinwrap", "A clone of xwinwrap for Hyprland", "Genny", "1.1"};
}

APICALL EXPORT void PLUGIN_EXIT()
{
    clearWindowRules();
}
