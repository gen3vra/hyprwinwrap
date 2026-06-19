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
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/config/values/ConfigValues.hpp>
#include "globals.hpp"

extern "C"
{
#include <lua.h>
#include <lauxlib.h>
}

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

// bg window object
struct SBgWindowSpec
{
    std::string className; // exact match
    std::string title;     // exact match
    int layer = 0;         // render order
    float posX = 0.f, posY = 0.f, sizeX = 100.f, sizeY = 100.f;
};

std::vector<PHLWINDOWREF> bgWindows;
std::vector<SP<Desktop::Rule::IRule>> bgRules;
std::unordered_map<PHLWINDOW, bool> interactableStates;
bool anyInteractive = false;

// Specs registered via the Lua window() function during config evaluation.
static std::vector<SBgWindowSpec> gLuaSpecs;
// Legacy + Lua specs merged into one list, rebuilt on each config reload.
static std::vector<SBgWindowSpec> gMergedSpecs;
// Geometry of each matched background window, so it can be re-applied later.
static std::unordered_map<PHLWINDOW, SBgWindowSpec> gWindowSpecs;

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

// Parse a CStringValue into a float, leaving the fallback untouched on failure.
static float parseFloatOr(const SP<Config::Values::CStringValue> &val, float fallback)
{
    try
    {
        return std::stof(val->value());
    }
    catch (...)
    {
        return fallback;
    }
}

// Build the unified list of background window specs: the legacy hyprlang config followed by every spec registered via the Lua API.
static std::vector<SBgWindowSpec> collectSpecs()
{
    std::vector<SBgWindowSpec> specs;

    const std::string legacyClass(gCfgClass->value());
    const std::string legacyTitle(gCfgTitle->value());
    if (!legacyClass.empty() || !legacyTitle.empty())
    {
        SBgWindowSpec legacy;
        legacy.className = legacyClass;
        legacy.title = legacyTitle;
        legacy.sizeX = parseFloatOr(gCfgSizeX, 100.f);
        legacy.sizeY = parseFloatOr(gCfgSizeY, 100.f);
        legacy.posX = parseFloatOr(gCfgPosX, 0.f);
        legacy.posY = parseFloatOr(gCfgPosY, 0.f);
        specs.push_back(legacy);
    }

    specs.insert(specs.end(), gLuaSpecs.begin(), gLuaSpecs.end());
    return specs;
}

static void applyBgWindowGeometry(PHLWINDOW pWindow)
{
    const auto PMONITOR = pWindow->m_monitor.lock();
    if (!PMONITOR)
        return;

    static const SBgWindowSpec kDefault{};
    const auto it = gWindowSpecs.find(pWindow);
    const SBgWindowSpec &spec = it != gWindowSpecs.end() ? it->second : kDefault;

    float sx = std::clamp(spec.sizeX, 1.f, 100.f);
    float sy = std::clamp(spec.sizeY, 1.f, 100.f);
    float px = std::clamp(spec.posX, 0.f, 100.f);
    float py = std::clamp(spec.posY, 0.f, 100.f);

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
    const SBgWindowSpec *matched = nullptr;
    for (const auto &spec : gMergedSpecs)
    {
        const bool classMatches = !spec.className.empty() && pWindow->m_initialClass == spec.className;
        const bool titleMatches = !spec.title.empty() && pWindow->m_title == spec.title;
        if (classMatches || titleMatches)
        {
            matched = &spec;
            break;
        }
    }

    if (!matched)
        return;

    const auto PMONITOR = pWindow->m_monitor.lock();
    if (!PMONITOR)
        return;

    gWindowSpecs[pWindow] = *matched;

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

    // Insert into bgWindows keeping it sorted ascending by layer, so onRenderStage can iterate as is
    const int newLayer = matched->layer;
    const auto pos = std::upper_bound(bgWindows.begin(), bgWindows.end(), newLayer,
                                      [](int layer, const PHLWINDOWREF &ref)
                                      {
                                          const auto w = ref.lock();
                                          if (!w)
                                              return false; // expired: keep scanning past it
                                          const auto it = gWindowSpecs.find(w);
                                          return layer < (it != gWindowSpecs.end() ? it->second.layer : 0);
                                      });
    bgWindows.insert(pos, pWindow);

    pWindow->m_hidden = true;

    pWindow->m_ruleApplicator->noFocusOverride(Desktop::Types::COverridableVar<bool>(true, Desktop::Types::PRIORITY_SET_PROP));

    g_pInputManager->refocus();
}

void onCloseWindow(PHLWINDOW pWindow)
{
    std::erase_if(bgWindows, [pWindow](const auto &ref)
                  { return ref.expired() || ref.lock() == pWindow; });
    interactableStates.erase(pWindow);
    gWindowSpecs.erase(pWindow);
}

// Hypr already has solitary rendering path but if we are in solitary rendering (fullscreen) and send a notification for instance, it will exit solitary rendering branch and thus we'll render again without this
static bool shouldPauseBg(const PHLMONITOR &mon)
{
    if (g_pSessionLockManager->isSessionLocked())
        return true;
    if (!mon || !mon->m_dpmsStatus)
        return true;
    const auto ws = mon->m_activeWorkspace;
    return ws && ws->m_fullscreenMode == FSMODE_FULLSCREEN;
}

void onRenderStage(eRenderStage stage)
{
    if (stage != RENDER_POST_WALLPAPER)
        return;

    const bool pause = shouldPauseBg(g_pHyprRenderer->m_renderData.pMonitor.lock());

    for (auto &bg : bgWindows)
    {
        const auto bgw = bg.lock();

        if (bgw->m_monitor != g_pHyprRenderer->m_renderData.pMonitor)
            continue;

        const bool interactable = interactableStates.contains(bgw) ? interactableStates[bgw] : false;
        if (interactable)
            continue; // pinned-always-above Hypr pass handles it on top

        // remove if anyone reports issues with windows freezing in weird scenarios
        if (pause)
        {
            if (!bgw->m_suspended)
                bgw->setSuspended(true);
            continue;
        }
        // Hyprland's updateSuspendedStates() sees our perma-hidden window and sends xdg-toplevel.suspended
        if (bgw->m_suspended)
            bgw->setSuspended(false);

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
    g_pHyprRenderer->damageWindow(PWINDOW);

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
    g_pHyprRenderer->damageWindow(PWINDOW);

    const bool interactable = interactableStates.contains(PWINDOW) ? interactableStates[PWINDOW] : false;
    PWINDOW->m_hidden = !interactable;
}

// TODO: DEPRECATED: Lua focus function idea is better, I'll remove after hyprlang dropped
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

// hl.plugin.hyprwinwrap.window({ class = "...", title = "...", layer = 0,
//                                pos_x = 0, pos_y = 0, size_x = 100, size_y = 100 })
static int luaWindow(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    SBgWindowSpec s;

    const auto getStr = [&](const char *key, std::string &out)
    {
        lua_getfield(L, 1, key);
        if (lua_isstring(L, -1))
            out = lua_tostring(L, -1);
        lua_pop(L, 1);
    };
    const auto getNum = [&](const char *key, float &out)
    {
        lua_getfield(L, 1, key);
        if (lua_isnumber(L, -1))
            out = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    };

    getStr("class", s.className);
    getStr("title", s.title);

    lua_getfield(L, 1, "layer");
    if (lua_isinteger(L, -1))
        s.layer = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);

    getNum("pos_x", s.posX);
    getNum("pos_y", s.posY);
    getNum("size_x", s.sizeX);
    getNum("size_y", s.sizeY);

    if (s.className.empty() && s.title.empty())
        return luaL_error(L, "hyprwinwrap.window: requires a 'class' or 'title' field");

    gLuaSpecs.push_back(s);
    return 0;
}

// Toggle one bg window's interactivity (focus it / hand focus back).
static void toggleBgWindow(const PHLWINDOW &bgw)
{
    auto it = interactableStates.find(bgw);
    if (it == interactableStates.end())
        return;

    it->second = !it->second;
    bgw->m_hidden = !it->second;

    if (it->second)
    {
        bgw->m_ruleApplicator->noFocusOverride(Desktop::Types::COverridableVar<bool>(false, Desktop::Types::PRIORITY_SET_PROP));
        Desktop::focusState()->fullWindowFocus(bgw, Desktop::FOCUS_REASON_OTHER);
    }
    else
        bgw->m_ruleApplicator->noFocusOverride(Desktop::Types::COverridableVar<bool>(true, Desktop::Types::PRIORITY_SET_PROP));

    anyInteractive = it->second;
}

// hl.plugin.hyprwinwrap.focus("window-bg")
// hl.bind("SUPER + B", function() hl.plugin.hyprwinwrap.focus("window-bg") end)
// hyprctl dispatch 'hl.plugin.hyprwinwrap.focus("window-bg")'
static int luaFocus(lua_State *L)
{
    const std::string match = luaL_checkstring(L, 1);

    // First pass class, then title; toggle whichever matches
    for (const auto prop : {&Desktop::View::CWindow::m_initialClass, &Desktop::View::CWindow::m_title})
    {
        int toggled = 0;
        for (auto &bg : bgWindows)
        {
            const auto bgw = bg.lock();
            if (bgw && bgw.get()->*prop == match)
            {
                toggleBgWindow(bgw);
                toggled++;
            }
        }
        if (toggled > 0)
            return 0;
    }

    return luaL_error(L, "hyprwinwrap.focus: no background window matching '%s'", match.c_str());
}

void onConfigReloaded()
{
    clearWindowRules();

    gMergedSpecs = collectSpecs();

    int idx = 0;
    for (const auto &spec : gMergedSpecs)
    {
        const auto addRule = [&](const char *kind, Desktop::Rule::eRuleProperty prop, const std::string &match)
        {
            if (match.empty())
                return;
            auto rule = makeWindowRule("hyprwinwrap-" + std::string(kind) + "-" + std::to_string(idx), prop, match);
            bgRules.emplace_back(rule);
            Desktop::Rule::ruleEngine()->registerRule(SP<Desktop::Rule::IRule>{rule});
        };

        addRule("class", Desktop::Rule::RULE_PROP_CLASS, spec.className);
        addRule("title", Desktop::Rule::RULE_PROP_TITLE, spec.title);

        idx++;
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
    // rebuilt each reload; clear before the config is evaluated.
    static auto P4b = Event::bus()->m_events.config.preReload.listen([&]
                                                                     { gLuaSpecs.clear(); });

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

        // If the newly focused window is itself an interactive bg window (e.g. during a drag, rawWindowFocus fires with w == bgw), keep anyInteractive set so that the next real focus-away still triggers the untoggle.
        // TODO: Is this the best way to handle this? I think so: if we drag it to see another window, it goes back to background, and we don't reset pos. then we could toggle it again if wanting to switch back and forth, and it resets on workspace change when assumedly done. side effect resets pos on ws change even if focused
        const auto wit = interactableStates.find(w);
        if (wit == interactableStates.end() || !wit->second)
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

    // TODO: DEPRECATED: Probably remove these in favor of just using the Lua API for everything
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

    // Multiple background windows yay
    //   hl.plugin.hyprwinwrap.window({ class = "...", layer = 0, pos_x = 0, ... })
    // Returns false when not running a Lua config
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprwinwrap", "window", &luaWindow);

    // Toggle interactivity of a bg window, matched by class then title:
    //   hl.plugin.hyprwinwrap.focus("window-bg")
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprwinwrap", "focus", &luaFocus);

    onConfigReloaded();

    HyprlandAPI::addNotification(PHANDLE, "[hyprwinwrap] Initialized successfully!", CHyprColor{0.2, 1.0, 0.2, 1.0}, 5000);

    return {"hyprwinwrap", "Display any window as a background wallpaper. Can be layered on top of existing wallpapers and supports multiple windows.", "Genny", "1.2"};
}

APICALL EXPORT void PLUGIN_EXIT()
{
    clearWindowRules();
}
