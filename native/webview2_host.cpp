#include <windows.h>
#include <shellapi.h>
#include <objbase.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <climits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// KeyParty: this is a vendored, patched copy of zero-native's WebView2 host.
// The upstream host (src/platform/windows/webview2_host.cpp) only opens a bare
// Win32 window — it never hosts a WebView2 in the main window and has no JS
// bridge. This copy adds the pieces KeyParty needs to match the macOS shell:
//   * a real WebView2 in the main window, serving the bundled frontend;
//   * the window.zero event bus + window.keyparty control bridge the UI uses;
//   * full-screen kiosk lockdown with a global low-level keyboard hook that
//     swallows every shortcut (Win, Alt+Tab, Alt+F4, Ctrl+Esc, …) and forwards
//     each key to the UI as a synthetic "key" event, plus the grown-up quit
//     chord (Control + Alt + Shift + Q) that returns to the menu.
// It keeps the framework's C ABI (the zero_native_windows_* exports), so the
// rest of zero-native is unchanged. See native/appkit_host.m for the macOS twin.
// ---------------------------------------------------------------------------

#if __has_include(<WebView2.h>) && __has_include(<wrl.h>)
#include <WebView2.h>
#include <wrl.h>
#define ZERO_NATIVE_HAS_WEBVIEW2 1
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
#else
#define ZERO_NATIVE_HAS_WEBVIEW2 0
// Without these headers the host can only open a bare window with no web content
// (a blank window). Make that loud at build time: pass -Dwebview2-include and
// -Dwinrt-include so WebView2.h and wrl.h are found.
#pragma message("KeyParty WARNING: WebView2.h / wrl.h not found -- building a BLANK-WINDOW stub. Pass -Dwebview2-include and -Dwinrt-include.")
#endif

namespace {

enum EventKind {
    kStart = 0,
    kFrame = 1,
    kShutdown = 2,
    kResize = 3,
    kWindowFrame = 4,
};

struct WindowsEvent {
    int kind;
    uint64_t window_id;
    double width;
    double height;
    double scale;
    double x;
    double y;
    int open;
    int focused;
    const char *label;
    size_t label_len;
    const char *title;
    size_t title_len;
};

using EventCallback = void (*)(void *, const WindowsEvent *);
using BridgeCallback = void (*)(void *, uint64_t, const char *, size_t, const char *, size_t, const char *, size_t);

struct Window {
    uint64_t id = 1;
    HWND hwnd = nullptr;
    std::string label;
    std::string title;
    double x = 0;
    double y = 0;
    double width = 720;
    double height = 480;
    // KeyParty: the main-window WebView2 (the upstream host never created one).
    // load_* params are stashed here until the controller finishes its async
    // creation, then applied in applyMainLoad().
    bool webview_started = false;
    int load_kind = -1;        // 0 = html, 1 = url, 2 = bundled assets
    std::string load_source;   // html string or url
    std::string asset_root;    // absolute folder for kind == 2
    std::string asset_entry;   // e.g. "index.html"
    bool spa_fallback = false;
    // Kiosk state, saved so the quit chord can restore the windowed menu.
    LONG_PTR saved_style = 0;
    LONG_PTR saved_exstyle = 0;
    RECT saved_rect = {};
    bool kiosk_active = false;
#if ZERO_NATIVE_HAS_WEBVIEW2
    ComPtr<ICoreWebView2Controller> main_controller;
    ComPtr<ICoreWebView2> main_webview;
#endif
};

struct ChildWebView {
    uint64_t window_id = 1;
    HWND hwnd = nullptr;
    std::string label;
    std::string url;
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    double zoom = 1.0;
    int layer = 0;
    uint64_t creation_order = 0;
    bool transparent = false;
    bool bridge_enabled = false;
#if ZERO_NATIVE_HAS_WEBVIEW2
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
#endif
};

struct HostLifetime {
    std::recursive_mutex mutex;
    bool alive = true;
};

struct Host {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    std::string app_name;
    std::string window_title;
    std::string bundle_id;
    std::string icon_path;
    EventCallback callback = nullptr;
    void *callback_context = nullptr;
    BridgeCallback bridge_callback = nullptr;
    void *bridge_context = nullptr;
    bool running = false;
    std::map<uint64_t, Window> windows;
    std::map<std::string, ChildWebView> webviews;
    uint64_t next_webview_order = 1;
    std::vector<std::string> allowed_origins;
    std::vector<std::string> allowed_external_urls;
    int external_link_action = 0;
    std::shared_ptr<HostLifetime> lifetime = std::make_shared<HostLifetime>();
    // KeyParty: per-process WebView2 user-data folder + kiosk lock state.
    std::wstring user_data_folder;
    bool kiosk_active = false;
};

static std::string slice(const char *bytes, size_t len) {
    return bytes && len > 0 ? std::string(bytes, len) : std::string();
}

static std::wstring widen(const std::string &value) {
    if (value.empty()) return std::wstring();
    int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0);
    std::wstring out((size_t)count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), out.data(), count);
    return out;
}

static std::string narrow(const std::wstring &value) {
    if (value.empty()) return std::string();
    int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), out.data(), count, nullptr, nullptr);
    return out;
}

static std::vector<std::string> parseNewlineList(const char *bytes, size_t len) {
    std::vector<std::string> result;
    if (!bytes || len == 0) return result;
    const char *start = bytes;
    const char *end = bytes + len;
    while (start < end) {
        const char *nl = static_cast<const char *>(memchr(start, '\n', (size_t)(end - start)));
        size_t seg_len = nl ? (size_t)(nl - start) : (size_t)(end - start);
        while (seg_len > 0 && (*start == ' ' || *start == '\t')) {
            ++start;
            --seg_len;
        }
        while (seg_len > 0 && (start[seg_len - 1] == ' ' || start[seg_len - 1] == '\t' || start[seg_len - 1] == '\r')) --seg_len;
        if (seg_len > 0) result.emplace_back(start, seg_len);
        start = nl ? nl + 1 : end;
    }
    return result;
}

static std::string originForUrl(const std::string &url) {
    if (url.empty() || url.rfind("about:", 0) == 0) return "zero://inline";
    size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return "zero://inline";
    if (url.compare(0, scheme_end, "file") == 0) return "file://local";
    size_t host_start = scheme_end + 3;
    size_t host_end = host_start;
    while (host_end < url.size() && url[host_end] != '/' && url[host_end] != '?' && url[host_end] != '#') ++host_end;
    if (host_end == host_start) return url.substr(0, scheme_end) + "://local";
    return url.substr(0, host_end);
}

static bool policyListMatches(const std::vector<std::string> &values, const std::string &url) {
    std::string origin = originForUrl(url);
    for (const std::string &value : values) {
        if (value == "*" || value == origin || value == url) return true;
        if (!value.empty() && value.back() == '*') {
            const std::string prefix = value.substr(0, value.size() - 1);
            if (url.rfind(prefix, 0) == 0 || origin.rfind(prefix, 0) == 0) return true;
        }
    }
    return false;
}

static size_t boundedLen(const char *text, size_t limit) {
    size_t len = 0;
    while (len < limit && text[len] != '\0') ++len;
    return len;
}

static void emit(Host *host, const Window &window, EventKind kind) {
    if (!host || !host->callback) return;
    RECT rect = {};
    if (window.hwnd) GetClientRect(window.hwnd, &rect);
    WindowsEvent event = {};
    event.kind = kind;
    event.window_id = window.id;
    event.width = rect.right > rect.left ? (double)(rect.right - rect.left) : window.width;
    event.height = rect.bottom > rect.top ? (double)(rect.bottom - rect.top) : window.height;
    event.scale = 1.0;
    event.x = window.x;
    event.y = window.y;
    event.open = window.hwnd != nullptr;
    event.focused = window.hwnd && GetFocus() == window.hwnd;
    event.label = window.label.c_str();
    event.label_len = window.label.size();
    event.title = window.title.c_str();
    event.title_len = window.title.size();
    host->callback(host->callback_context, &event);
}

static std::string webViewKey(uint64_t window_id, const std::string &label) {
    return std::to_string(window_id) + ":" + label;
}

static int webViewCoord(double value) {
    return value > 0 ? (int)(value + 0.5) : 0;
}

static int webViewExtent(double value) {
    return value > 1 ? (int)(value + 0.5) : 1;
}

static bool validChildWebViewFrame(double x, double y, double width, double height) {
    return x >= 0 && y >= 0 && width > 0 && height > 0;
}

static void destroyChildWebViewsForWindow(Host *host, uint64_t window_id) {
    if (!host) return;
    for (auto it = host->webviews.begin(); it != host->webviews.end();) {
        if (it->second.window_id == window_id) {
#if ZERO_NATIVE_HAS_WEBVIEW2
            if (it->second.controller) it->second.controller->Close();
#endif
            if (it->second.hwnd) DestroyWindow(it->second.hwnd);
            it = host->webviews.erase(it);
        } else {
            ++it;
        }
    }
}

static void destroyAllWindows(Host *host) {
    if (!host) return;
    for (auto &entry : host->windows) {
        destroyChildWebViewsForWindow(host, entry.first);
        if (entry.second.hwnd) {
            DestroyWindow(entry.second.hwnd);
            entry.second.hwnd = nullptr;
        }
    }
}

#if ZERO_NATIVE_HAS_WEBVIEW2
using CreateEnvironmentFn = HRESULT (STDAPICALLTYPE *)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);

static RECT webViewRect(const ChildWebView &webview) {
    RECT rect = {};
    rect.left = 0;
    rect.top = 0;
    rect.right = webViewExtent(webview.width);
    rect.bottom = webViewExtent(webview.height);
    return rect;
}

static CreateEnvironmentFn webView2Factory() {
    static HMODULE loader = LoadLibraryW(L"WebView2Loader.dll");
    if (!loader) return nullptr;
    return reinterpret_cast<CreateEnvironmentFn>(GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions"));
}

static void cleanupPendingChildWebView(Host *host, const std::string &key) {
    if (!host) return;
    auto found = host->webviews.find(key);
    if (found == host->webviews.end()) return;
    if (found->second.controller) found->second.controller->Close();
    if (found->second.hwnd) DestroyWindow(found->second.hwnd);
    host->webviews.erase(found);
}

static bool createChildWebView(Host *host, const std::string &key) {
    auto factory = webView2Factory();
    if (!factory) return false;
    auto found = host->webviews.find(key);
    if (found == host->webviews.end() || !found->second.hwnd) return false;
    HWND parent = found->second.hwnd;
    std::weak_ptr<HostLifetime> lifetime = host->lifetime;
    HRESULT hr = factory(nullptr, nullptr, nullptr, Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [host, key, parent, lifetime](HRESULT result, ICoreWebView2Environment *environment) -> HRESULT {
            auto token = lifetime.lock();
            if (!token) return S_OK;
            std::lock_guard<std::recursive_mutex> guard(token->mutex);
            if (!token->alive) return S_OK;
            auto found = host->webviews.find(key);
            if (found == host->webviews.end() || found->second.hwnd != parent || !IsWindow(parent)) return S_OK;
            if (FAILED(result) || !environment) {
                cleanupPendingChildWebView(host, key);
                return result;
            }
            return environment->CreateCoreWebView2Controller(parent, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [host, key, lifetime](HRESULT controller_result, ICoreWebView2Controller *controller) -> HRESULT {
                    auto token = lifetime.lock();
                    if (!token) {
                        if (controller) controller->Close();
                        return S_OK;
                    }
                    std::lock_guard<std::recursive_mutex> guard(token->mutex);
                    if (!token->alive) {
                        if (controller) controller->Close();
                        return S_OK;
                    }
                    if (FAILED(controller_result) || !controller) {
                        cleanupPendingChildWebView(host, key);
                        return controller_result;
                    }
                    auto found = host->webviews.find(key);
                    if (found == host->webviews.end()) {
                        controller->Close();
                        return S_OK;
                    }
                    found->second.controller = controller;
                    controller->get_CoreWebView2(&found->second.webview);
                    RECT bounds = webViewRect(found->second);
                    controller->put_Bounds(bounds);
                    controller->put_ZoomFactor(found->second.zoom);
                    controller->put_IsVisible(TRUE);
                    if (found->second.webview) {
                        EventRegistrationToken token = {};
                        found->second.webview->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                            [host, lifetime](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                                auto token = lifetime.lock();
                                if (!token) return S_OK;
                                std::lock_guard<std::recursive_mutex> guard(token->mutex);
                                if (!token->alive) return S_OK;
                                LPWSTR uri_bytes = nullptr;
                                if (!args || FAILED(args->get_Uri(&uri_bytes))) return S_OK;
                                std::wstring uri_wide = uri_bytes ? std::wstring(uri_bytes) : std::wstring();
                                if (uri_bytes) CoTaskMemFree(uri_bytes);
                                std::string uri = narrow(uri_wide);
                                if (uri.empty() || uri.rfind("about:", 0) == 0 || policyListMatches(host->allowed_origins, uri)) return S_OK;
                                if (host->external_link_action == 1 && policyListMatches(host->allowed_external_urls, uri)) {
                                    ShellExecuteW(nullptr, L"open", uri_wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                                }
                                args->put_Cancel(TRUE);
                                return S_OK;
                            }).Get(), &token);
                        std::wstring latest_url = widen(found->second.url);
                        if (!latest_url.empty()) found->second.webview->Navigate(latest_url.c_str());
                    }
                    return S_OK;
                }).Get());
        }).Get());
    return SUCCEEDED(hr);
}
#endif

static void applyChildWebViewLayer(Host *host, uint64_t window_id, const std::string &label) {
    if (!host) return;
    auto found = host->webviews.find(webViewKey(window_id, label));
    if (found == host->webviews.end() || !found->second.hwnd) return;
    HWND insert_after = HWND_TOP;
    bool found_above = false;
    int best_layer = INT_MAX;
    uint64_t best_order = UINT64_MAX;
    for (auto &entry : host->webviews) {
        const ChildWebView &candidate = entry.second;
        if (candidate.window_id != window_id || candidate.label == label || !candidate.hwnd) continue;
        const bool candidate_above = candidate.layer > found->second.layer ||
            (candidate.layer == found->second.layer && candidate.creation_order > found->second.creation_order);
        if (!candidate_above) continue;
        if (!found_above ||
            candidate.layer < best_layer ||
            (candidate.layer == best_layer && candidate.creation_order < best_order)) {
            insert_after = candidate.hwnd;
            found_above = true;
            best_layer = candidate.layer;
            best_order = candidate.creation_order;
        }
    }
    SetWindowPos(found->second.hwnd, insert_after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static Host *hostFromWindow(HWND hwnd) {
    return reinterpret_cast<Host *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

/* ===================================================================== *
 * KeyParty kiosk + bridge layer (not in the upstream zero-native host).
 * ===================================================================== */

// Custom messages the keyboard hook posts to the main window's thread, so the
// (latency-sensitive) low-level hook proc returns immediately.
static constexpr UINT WM_KEYPARTY_KEY = WM_APP + 11;   // lparam = KeyMsg*
static constexpr UINT WM_KEYPARTY_EXIT = WM_APP + 12;  // quit chord -> menu

// The bundled frontend is served from a virtual host mapped to the asset folder.
static const wchar_t kAssetHost[] = L"keyparty.assets";
static const wchar_t kAssetBaseUrl[] = L"https://keyparty.assets/";

// A key event marshalled from the global hook to the UI thread.
struct KeyMsg {
    std::wstring code;
    std::wstring key;
    bool is_down = false;
    bool repeat = false;
    bool ctrl = false;
    bool alt = false;
    bool meta = false;
    bool shift = false;
};

// The active kiosk host (the global keyboard hook can't carry context). KeyParty
// runs a single window, so a single pointer suffices.
static Host *g_kiosk_host = nullptr;
static HHOOK g_keyboard_hook = nullptr;
static std::set<DWORD> g_held_vks;

// Kiosk lockdown is on by default. Set KEYPARTY_NO_KIOSK=1 to develop without
// the screen takeover and keyboard grab (mirrors the macOS host).
static bool kioskEnabled() {
    wchar_t buf[8] = {};
    DWORD n = GetEnvironmentVariableW(L"KEYPARTY_NO_KIOSK", buf, 8);
    if (n == 0) return true;
    std::wstring v(buf, n);
    return v.empty() || v == L"0";
}

// Quote + escape a wide string as a JSON string token.
static std::wstring jsonQuote(const std::wstring &s) {
    static const wchar_t *hex = L"0123456789abcdef";
    std::wstring out;
    out.push_back(L'"');
    for (wchar_t c : s) {
        switch (c) {
            case L'"': out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            default:
                if (c < 0x20) {
                    out += L"\\u00";
                    out.push_back(hex[(c >> 4) & 0xF]);
                    out.push_back(hex[c & 0xF]);
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back(L'"');
    return out;
}

// Format an HRESULT as 0xXXXXXXXX for diagnostic message boxes.
static std::wstring hrHex(long hr) {
    static const wchar_t *hex = L"0123456789abcdef";
    unsigned long v = (unsigned long)hr;
    std::wstring s = L"0x";
    for (int shift = 28; shift >= 0; shift -= 4) s.push_back(hex[(v >> shift) & 0xF]);
    return s;
}

static void executeMainScript(Host *host, uint64_t window_id, const std::wstring &js) {
#if ZERO_NATIVE_HAS_WEBVIEW2
    if (!host) return;
    auto it = host->windows.find(window_id);
    if (it == host->windows.end() || !it->second.main_webview) return;
    // A no-op completion handler (some WebView2 builds reject a null handler).
    it->second.main_webview->ExecuteScript(js.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; }).Get());
#else
    (void)host; (void)window_id; (void)js;
#endif
}

// Push an event onto the page's window.zero bus (native -> JS).
static void emitMainEvent(Host *host, const wchar_t *name, const std::wstring &detail_json) {
    std::wstring js = L"window.zero&&window.zero._emit(\"";
    js += name;
    js += L"\",";
    js += detail_json;
    js += L");";
    executeMainScript(host, 1, js);
}

// Map a Win32 virtual-key to the web "code" the UI's categorizer expects.
static std::wstring vkToCode(DWORD vk, DWORD flags) {
    if (vk >= 'A' && vk <= 'Z') return std::wstring(L"Key") + (wchar_t)vk;
    if (vk >= '0' && vk <= '9') return std::wstring(L"Digit") + (wchar_t)vk;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return std::wstring(L"Numpad") + (wchar_t)(L'0' + (vk - VK_NUMPAD0));
    switch (vk) {
        case VK_SPACE: return L"Space";
        case VK_RETURN: return (flags & LLKHF_EXTENDED) ? L"NumpadEnter" : L"Enter";
        case VK_TAB: return L"Tab";
        case VK_BACK: return L"Backspace";
        case VK_DELETE: return L"Delete";
        case VK_ESCAPE: return L"Escape";
        case VK_LEFT: return L"ArrowLeft";
        case VK_RIGHT: return L"ArrowRight";
        case VK_UP: return L"ArrowUp";
        case VK_DOWN: return L"ArrowDown";
        case VK_LSHIFT: return L"ShiftLeft";
        case VK_RSHIFT: return L"ShiftRight";
        case VK_SHIFT: return L"ShiftLeft";
        case VK_LCONTROL: return L"ControlLeft";
        case VK_RCONTROL: return L"ControlRight";
        case VK_CONTROL: return L"ControlLeft";
        case VK_LMENU: return L"AltLeft";
        case VK_RMENU: return L"AltRight";
        case VK_MENU: return L"AltLeft";
        case VK_LWIN: return L"MetaLeft";
        case VK_RWIN: return L"MetaRight";
        case VK_CAPITAL: return L"CapsLock";
        default: break;
    }
    return L"Other";
}

// A human-readable label; the UI mostly keys off "code", and uses "key" only for
// punctuation glyphs (single chars), so a best-effort character is enough.
static std::wstring vkToKeyLabel(DWORD vk, DWORD flags) {
    if (vk >= 'A' && vk <= 'Z') return std::wstring(1, (wchar_t)vk);
    if (vk >= '0' && vk <= '9') return std::wstring(1, (wchar_t)vk);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return std::wstring(1, (wchar_t)(L'0' + (vk - VK_NUMPAD0)));
    switch (vk) {
        case VK_SPACE: return L" ";
        case VK_RETURN: return L"Enter";
        case VK_TAB: return L"Tab";
        case VK_BACK: return L"Backspace";
        case VK_DELETE: return L"Delete";
        case VK_ESCAPE: return L"Escape";
        case VK_LEFT: return L"ArrowLeft";
        case VK_RIGHT: return L"ArrowRight";
        case VK_UP: return L"ArrowUp";
        case VK_DOWN: return L"ArrowDown";
        case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT: return L"Shift";
        case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL: return L"Control";
        case VK_LMENU: case VK_RMENU: case VK_MENU: return L"Alt";
        case VK_LWIN: case VK_RWIN: return L"Meta";
        case VK_CAPITAL: return L"CapsLock";
        default: break;
    }
    (void)flags;
    UINT mapped = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR);
    wchar_t c = (wchar_t)(mapped & 0x7FFF);
    if (c >= 0x20) return std::wstring(1, c);
    return std::wstring();
}

// Forward a swallowed key to the game UI as a native "key" / "keyup" event.
static void forwardKey(Host *host, const KeyMsg &km) {
    if (km.is_down) {
        std::wstring detail = L"{\"code\":";
        detail += jsonQuote(km.code);
        detail += L",\"key\":";
        detail += jsonQuote(km.key);
        detail += L",\"repeat\":"; detail += km.repeat ? L"true" : L"false";
        detail += L",\"ctrl\":";   detail += km.ctrl ? L"true" : L"false";
        detail += L",\"alt\":";    detail += km.alt ? L"true" : L"false";
        detail += L",\"meta\":";   detail += km.meta ? L"true" : L"false";
        detail += L",\"shift\":";  detail += km.shift ? L"true" : L"false";
        detail += L"}";
        emitMainEvent(host, L"key", detail);
    } else {
        std::wstring detail = L"{\"code\":";
        detail += jsonQuote(km.code);
        detail += L"}";
        emitMainEvent(host, L"keyup", detail);
    }
}

// Windows needs no special permission for the keyboard hook, so it always
// reports "trusted"; kioskEnabled tracks the dev flag (same shape as macOS).
static void emitAccessibilityStatus(Host *host) {
    std::wstring detail = L"{\"trusted\":true,\"kioskEnabled\":";
    detail += kioskEnabled() ? L"true" : L"false";
    detail += L"}";
    emitMainEvent(host, L"keyparty:accessibility", detail);
}

// The global keyboard tap: swallows every key system-wide and forwards it to the
// UI, plus drives the grown-up quit chord. Installed only while in kiosk.
static LRESULT CALLBACK lowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_kiosk_host && g_kiosk_host->kiosk_active) {
        auto *p = reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);
        bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        bool up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
        if (p && (down || up)) {
            DWORD vk = p->vkCode;
            bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            bool meta = ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;

            HWND hwnd = nullptr;
            auto it = g_kiosk_host->windows.find(1);
            if (it != g_kiosk_host->windows.end()) hwnd = it->second.hwnd;

            // Grown-up chord: Control + Alt + Shift + Q -> back to the menu.
            if (down && ctrl && alt && shift && !meta && vk == 'Q') {
                if (hwnd) PostMessageW(hwnd, WM_KEYPARTY_EXIT, 0, 0);
                return 1;
            }

            bool repeat = false;
            if (down) {
                repeat = g_held_vks.count(vk) != 0;
                g_held_vks.insert(vk);
            } else {
                g_held_vks.erase(vk);
            }

            if (hwnd) {
                KeyMsg *km = new KeyMsg();
                km->is_down = down;
                km->code = vkToCode(vk, p->flags);
                km->key = down ? vkToKeyLabel(vk, p->flags) : std::wstring();
                km->repeat = repeat;
                km->ctrl = ctrl;
                km->alt = alt;
                km->meta = meta;
                km->shift = shift;
                PostMessageW(hwnd, WM_KEYPARTY_KEY, 0, reinterpret_cast<LPARAM>(km));
            }
            return 1;  // swallow: nothing reaches another app or the OS
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// Start: turn the windowed menu into the locked-down, full-screen kiosk.
static void enterKiosk(Host *host) {
    if (!host) return;
    auto it = host->windows.find(1);
    if (it == host->windows.end() || !it->second.hwnd) return;
    Window &w = it->second;
    if (w.kiosk_active) return;

    w.saved_style = GetWindowLongPtrW(w.hwnd, GWL_STYLE);
    w.saved_exstyle = GetWindowLongPtrW(w.hwnd, GWL_EXSTYLE);
    GetWindowRect(w.hwnd, &w.saved_rect);

    if (!kioskEnabled()) {
        // Dev / no-kiosk: the UI already switched to the game; don't take over the
        // screen or grab the keyboard (the DOM path handles the quit chord).
        return;
    }

    RECT screen = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    HMONITOR mon = MonitorFromWindow(w.hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) screen = mi.rcMonitor;

    // Borderless + topmost, covering the whole monitor (taskbar included).
    SetWindowLongPtrW(w.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowLongPtrW(w.hwnd, GWL_EXSTYLE, WS_EX_TOPMOST);
    SetWindowPos(w.hwnd, HWND_TOPMOST, screen.left, screen.top,
                 screen.right - screen.left, screen.bottom - screen.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    SetForegroundWindow(w.hwnd);

    w.kiosk_active = true;
    host->kiosk_active = true;
    g_kiosk_host = host;
    g_held_vks.clear();
    if (!g_keyboard_hook) {
        g_keyboard_hook = SetWindowsHookExW(WH_KEYBOARD_LL, lowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);
    }
#if ZERO_NATIVE_HAS_WEBVIEW2
    if (w.main_controller) {
        RECT rc;
        GetClientRect(w.hwnd, &rc);
        w.main_controller->put_Bounds(rc);
    }
#endif
}

// The grown-up quit chord: leave the kiosk and return to the menu (not quit).
static void exitKioskToMenu(Host *host) {
    if (!host) return;
    auto it = host->windows.find(1);
    if (it == host->windows.end()) return;
    Window &w = it->second;
    if (!w.kiosk_active) return;

    w.kiosk_active = false;
    host->kiosk_active = false;
    if (g_keyboard_hook) {
        UnhookWindowsHookEx(g_keyboard_hook);
        g_keyboard_hook = nullptr;
    }
    g_kiosk_host = nullptr;
    g_held_vks.clear();

    if (w.hwnd) {
        LONG_PTR style = w.saved_style ? w.saved_style : (LONG_PTR)(WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowLongPtrW(w.hwnd, GWL_STYLE, style);
        SetWindowLongPtrW(w.hwnd, GWL_EXSTYLE, w.saved_exstyle);
        RECT r = w.saved_rect;
        if (r.right - r.left < 200 || r.bottom - r.top < 150) {
            int sw = GetSystemMetrics(SM_CXSCREEN);
            int sh = GetSystemMetrics(SM_CYSCREEN);
            int width = (int)w.width > 0 ? (int)w.width : 720;
            int height = (int)w.height > 0 ? (int)w.height : 480;
            r.left = (sw - width) / 2;
            r.top = (sh - height) / 2;
            r.right = r.left + width;
            r.bottom = r.top + height;
        }
        SetWindowPos(w.hwnd, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        SetForegroundWindow(w.hwnd);
#if ZERO_NATIVE_HAS_WEBVIEW2
        if (w.main_controller) {
            RECT rc;
            GetClientRect(w.hwnd, &rc);
            w.main_controller->put_Bounds(rc);
        }
#endif
    }

    emitMainEvent(host, L"keyparty:menu", L"{}");
}

// JS -> native control commands from window.keyparty.* .
static void handleControlCommand(Host *host, const std::wstring &cmd) {
    if (!host) return;
    if (cmd == L"start") {
        enterKiosk(host);
    } else if (cmd == L"quit") {
        host->running = false;
        PostQuitMessage(0);
    } else if (cmd == L"requestAccessibility") {
        emitAccessibilityStatus(host);  // no OS permission needed on Windows
    } else if (cmd == L"checkAccessibility") {
        emitAccessibilityStatus(host);
    }
}

// The window.zero event bus (native pushes events; JS posts via chrome.webview).
static const wchar_t *windowsBridgeScript() {
    return LR"JS((function(){
if(window.zero&&window.zero.invoke){return;}
var pending=new Map();var listeners=new Map();var nextId=1;
function post(message){if(window.chrome&&window.chrome.webview&&window.chrome.webview.postMessage){window.chrome.webview.postMessage(message);return;}throw new Error('zero-native bridge transport is unavailable');}
function complete(response){var id=response&&response.id!=null?String(response.id):'';var entry=pending.get(id);if(!entry){return;}pending.delete(id);if(response.ok){entry.resolve(response.result===undefined?null:response.result);return;}var e=response.error||{};var err=new Error(e.message||'Native command failed');err.code=e.code||'internal_error';entry.reject(err);}
function invoke(command,payload){if(typeof command!=='string'||command.length===0){return Promise.reject(new TypeError('command must be a non-empty string'));}var id=String(nextId++);var envelope=JSON.stringify({id:id,command:command,payload:payload===undefined?null:payload});return new Promise(function(resolve,reject){pending.set(id,{resolve:resolve,reject:reject});try{post(envelope);}catch(error){pending.delete(id);reject(error);}});}
function on(name,callback){if(typeof callback!=='function'){throw new TypeError('callback must be a function');}var set=listeners.get(name);if(!set){set=new Set();listeners.set(name,set);}set.add(callback);return function(){off(name,callback);};}
function off(name,callback){var set=listeners.get(name);if(set){set.delete(callback);if(set.size===0){listeners.delete(name);}}}
function emit(name,detail){var set=listeners.get(name);if(set){Array.from(set).forEach(function(cb){cb(detail);});}window.dispatchEvent(new CustomEvent('zero-native:'+name,{detail:detail}));}
Object.defineProperty(window,'zero',{value:Object.freeze({invoke:invoke,on:on,off:off,_complete:complete,_emit:emit}),configurable:false});
})();)JS";
}

// The one-way control shim the menu uses to drive kiosk entry/exit.
static const wchar_t *keyPartyControlScript() {
    return LR"JS((function(){
if(window.keyparty){return;}
function post(cmd){try{if(window.chrome&&window.chrome.webview&&window.chrome.webview.postMessage){window.chrome.webview.postMessage(cmd);}}catch(e){}}
Object.defineProperty(window,'keyparty',{value:Object.freeze({start:function(){post('start');},quit:function(){post('quit');},requestAccessibility:function(){post('requestAccessibility');},checkAccessibility:function(){post('checkAccessibility');}}),configurable:false});
})();)JS";
}

#if ZERO_NATIVE_HAS_WEBVIEW2
// The asset folder we last mapped, kept for the load-failure diagnostic below.
static std::wstring g_main_asset_folder;

static bool dirExists(const std::wstring &path) {
    if (path.empty()) return false;
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// Directory holding keyparty.exe (absolute, no trailing slash).
static std::wstring exeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    std::wstring path(buf, n);
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

static bool isAbsolutePath(const std::wstring &p) {
    if (p.size() >= 2 && p[1] == L':') return true;                  // C:\...
    if (!p.empty() && (p[0] == L'\\' || p[0] == L'/')) return true;  // \\server or /...
    return false;
}

// Collapse "..", make absolute (relative inputs resolve against the cwd).
static std::wstring fullPath(const std::wstring &p) {
    if (p.empty()) return p;
    wchar_t buf[MAX_PATH];
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH, buf, nullptr);
    return (n == 0 || n >= MAX_PATH) ? p : std::wstring(buf, n);
}

// The runtime hands us the manifest's relative "dist" (frontend.dist). The
// packager lays out the desktop artifact as <pkg>/bin/keyparty.exe alongside
// <pkg>/resources/<dist>/index.html (tooling/package.zig), mirroring macOS's
// Contents/MacOS + Contents/Resources. So resolve a relative root against the
// executable's directory — preferring the packaged ../resources/<root> layout,
// then a couple of dev fallbacks — and hand SetVirtualHostNameToFolderMapping a
// real absolute folder. An absolute root is used as-is.
static std::wstring resolveAssetRoot(const std::string &root_utf8) {
    std::wstring root = widen(root_utf8);
    if (isAbsolutePath(root)) return root;
    const std::wstring dir = exeDir();
    std::vector<std::wstring> candidates;
    if (!dir.empty() && !root.empty()) {
        candidates.push_back(fullPath(dir + L"\\..\\resources\\" + root)); // packaged
        candidates.push_back(fullPath(dir + L"\\resources\\" + root));
        candidates.push_back(fullPath(dir + L"\\" + root));                // beside exe
    }
    if (!root.empty()) candidates.push_back(fullPath(root));               // cwd (dev)
    for (const auto &c : candidates) {
        if (dirExists(c)) return c;
    }
    // None exist yet — return the packaged default so the diagnostic points at
    // where the assets are expected to live.
    return candidates.empty() ? root : candidates.front();
}

// Apply the stashed load request once the main WebView2 exists.
static void applyMainLoad(Host *host, Window &w) {
    (void)host;
    if (!w.main_webview) return;
    if (w.load_kind == 1) {
        std::wstring url = widen(w.load_source);
        if (!url.empty()) w.main_webview->Navigate(url.c_str());
    } else if (w.load_kind == 2) {
        ComPtr<ICoreWebView2_3> wv3;
        if (SUCCEEDED(w.main_webview.As(&wv3)) && wv3) {
            std::wstring folder = resolveAssetRoot(w.asset_root);
            g_main_asset_folder = folder;
            wv3->SetVirtualHostNameToFolderMapping(kAssetHost, folder.c_str(),
                                                   COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        }
        std::wstring entry = widen(w.asset_entry.empty() ? std::string("index.html") : w.asset_entry);
        std::wstring url = std::wstring(kAssetBaseUrl) + entry;
        w.main_webview->Navigate(url.c_str());
    } else if (w.load_kind == 0) {
        std::wstring html = widen(w.load_source);
        w.main_webview->NavigateToString(html.c_str());
    }
}

// Create the main-window WebView2 (async), wiring up the bridge, the control
// shim, message routing, and the navigation policy before the first load.
static void ensureMainWebView(Host *host, uint64_t window_id) {
    if (!host) return;
    auto it = host->windows.find(window_id);
    if (it == host->windows.end() || !it->second.hwnd) return;
    if (it->second.webview_started) return;
    it->second.webview_started = true;
    auto factory = webView2Factory();
    if (!factory) {
        it->second.webview_started = false;
        MessageBoxW(it->second.hwnd,
                    L"Could not load WebView2Loader.dll. It must sit next to keyparty.exe "
                    L"(it is bundled in the release .zip; a bare \"zig build run\" exe will "
                    L"not have it).",
                    L"KeyParty", MB_OK | MB_ICONERROR);
        return;
    }
    HWND hwnd = it->second.hwnd;
    std::weak_ptr<HostLifetime> lifetime = host->lifetime;
    const wchar_t *user_data = host->user_data_folder.empty() ? nullptr : host->user_data_folder.c_str();
    factory(nullptr, user_data, nullptr, Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [host, window_id, hwnd, lifetime](HRESULT result, ICoreWebView2Environment *environment) -> HRESULT {
            auto token = lifetime.lock();
            if (!token) return S_OK;
            std::lock_guard<std::recursive_mutex> guard(token->mutex);
            if (!token->alive) return S_OK;
            if (FAILED(result) || !environment) {
                MessageBoxW(hwnd,
                            (std::wstring(L"WebView2 environment could not be created (") + hrHex(result) +
                             L").\nIs the WebView2 Runtime installed? It ships with Windows 11 and "
                             L"current Windows 10; otherwise install the Evergreen runtime.").c_str(),
                            L"KeyParty", MB_OK | MB_ICONERROR);
                return result;
            }
            auto found = host->windows.find(window_id);
            if (found == host->windows.end() || found->second.hwnd != hwnd || !IsWindow(hwnd)) return S_OK;
            return environment->CreateCoreWebView2Controller(hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [host, window_id, hwnd, lifetime](HRESULT controller_result, ICoreWebView2Controller *controller) -> HRESULT {
                    auto token = lifetime.lock();
                    if (!token) {
                        if (controller) controller->Close();
                        return S_OK;
                    }
                    std::lock_guard<std::recursive_mutex> guard(token->mutex);
                    if (!token->alive) {
                        if (controller) controller->Close();
                        return S_OK;
                    }
                    if (FAILED(controller_result) || !controller) {
                        MessageBoxW(hwnd,
                                    (std::wstring(L"WebView2 controller could not be created (") +
                                     hrHex(controller_result) + L").").c_str(),
                                    L"KeyParty", MB_OK | MB_ICONERROR);
                        return controller_result;
                    }
                    auto found = host->windows.find(window_id);
                    if (found == host->windows.end() || found->second.hwnd != hwnd) {
                        controller->Close();
                        return S_OK;
                    }
                    Window &w = found->second;
                    w.main_controller = controller;
                    controller->get_CoreWebView2(&w.main_webview);
                    RECT rc;
                    GetClientRect(hwnd, &rc);
                    controller->put_Bounds(rc);
                    controller->put_IsVisible(TRUE);
                    if (w.main_webview) {
                        ComPtr<ICoreWebView2Settings> settings;
                        if (SUCCEEDED(w.main_webview->get_Settings(&settings)) && settings) {
                            settings->put_AreDefaultContextMenusEnabled(FALSE);
                            settings->put_IsStatusBarEnabled(FALSE);
                            settings->put_IsZoomControlEnabled(FALSE);
                            settings->put_AreDevToolsEnabled(FALSE);
                            ComPtr<ICoreWebView2Settings3> settings3;
                            if (SUCCEEDED(settings.As(&settings3)) && settings3) {
                                // Disable F5 / Ctrl+R / Ctrl+P etc. inside the page.
                                settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
                            }
                        }
                        w.main_webview->AddScriptToExecuteOnDocumentCreated(windowsBridgeScript(), nullptr);
                        w.main_webview->AddScriptToExecuteOnDocumentCreated(keyPartyControlScript(), nullptr);

                        EventRegistrationToken message_token = {};
                        w.main_webview->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                            [host, window_id, lifetime](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                                auto token = lifetime.lock();
                                if (!token) return S_OK;
                                std::lock_guard<std::recursive_mutex> guard(token->mutex);
                                if (!token->alive || !args) return S_OK;
                                LPWSTR raw = nullptr;
                                if (FAILED(args->TryGetWebMessageAsString(&raw)) || !raw) return S_OK;
                                std::wstring message(raw);
                                CoTaskMemFree(raw);
                                if (message == L"start" || message == L"quit" ||
                                    message == L"requestAccessibility" || message == L"checkAccessibility") {
                                    handleControlCommand(host, message);
                                    return S_OK;
                                }
                                // Generic zero-native bridge envelope -> forward to Zig.
                                if (host->bridge_callback) {
                                    std::string msg = narrow(message);
                                    std::string origin = "zero://app";
                                    LPWSTR source = nullptr;
                                    if (SUCCEEDED(args->get_Source(&source)) && source) {
                                        origin = originForUrl(narrow(std::wstring(source)));
                                        CoTaskMemFree(source);
                                    }
                                    const std::string label = "main";
                                    host->bridge_callback(host->bridge_context, window_id, label.c_str(), label.size(),
                                                          msg.c_str(), msg.size(), origin.c_str(), origin.size());
                                }
                                return S_OK;
                            }).Get(), &message_token);

                        EventRegistrationToken nav_token = {};
                        w.main_webview->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                            [host, lifetime](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                                auto token = lifetime.lock();
                                if (!token) return S_OK;
                                std::lock_guard<std::recursive_mutex> guard(token->mutex);
                                if (!token->alive || !args) return S_OK;
                                LPWSTR uri_bytes = nullptr;
                                if (FAILED(args->get_Uri(&uri_bytes)) || !uri_bytes) return S_OK;
                                std::wstring uri_wide(uri_bytes);
                                CoTaskMemFree(uri_bytes);
                                std::string uri = narrow(uri_wide);
                                if (uri.empty() || uri.rfind("about:", 0) == 0) return S_OK;
                                if (uri.rfind("https://keyparty.assets/", 0) == 0) return S_OK;
                                if (policyListMatches(host->allowed_origins, uri)) return S_OK;
                                if (host->external_link_action == 1 && policyListMatches(host->allowed_external_urls, uri)) {
                                    ShellExecuteW(nullptr, L"open", uri_wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                                }
                                args->put_Cancel(TRUE);
                                return S_OK;
                            }).Get(), &nav_token);

                        // If the top-level navigation fails, the window goes blank
                        // with no other clue — surface it (usually means the bundled
                        // frontend wasn't found at the mapped asset folder).
                        EventRegistrationToken navc_token = {};
                        w.main_webview->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
                            [lifetime](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                                auto token = lifetime.lock();
                                if (!token) return S_OK;
                                std::lock_guard<std::recursive_mutex> guard(token->mutex);
                                if (!token->alive || !args) return S_OK;
                                BOOL ok = TRUE;
                                args->get_IsSuccess(&ok);
                                if (!ok) {
                                    COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                                    args->get_WebErrorStatus(&status);
                                    std::wstring msg = L"The page failed to load (web error status ";
                                    msg += std::to_wstring((int)status);
                                    msg += L").";
                                    if (!g_main_asset_folder.empty()) {
                                        msg += L"\n\nExpected the bundled frontend at:\n";
                                        msg += g_main_asset_folder;
                                        if (!dirExists(g_main_asset_folder))
                                            msg += L"\n\nThat folder does not exist — the package is "
                                                   L"missing its resources\\dist directory.";
                                    }
                                    MessageBoxW(nullptr, msg.c_str(), L"KeyParty",
                                                MB_OK | MB_ICONWARNING);
                                }
                                return S_OK;
                            }).Get(), &navc_token);
                    }
                    applyMainLoad(host, w);
                    return S_OK;
                }).Get());
        }).Get());
}
#endif

static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
        auto *host = reinterpret_cast<Host *>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
    }
    Host *host = hostFromWindow(hwnd);
    switch (message) {
        case WM_SIZE:
            if (host) {
                for (auto &entry : host->windows) {
                    if (entry.second.hwnd == hwnd) {
#if ZERO_NATIVE_HAS_WEBVIEW2
                        if (entry.second.main_controller) {
                            RECT rc;
                            GetClientRect(hwnd, &rc);
                            entry.second.main_controller->put_Bounds(rc);
                        }
#endif
                        emit(host, entry.second, kResize);
                    }
                }
            }
            return 0;
        case WM_KEYPARTY_KEY:
            if (host) {
                KeyMsg *km = reinterpret_cast<KeyMsg *>(lparam);
                if (km) {
                    forwardKey(host, *km);
                    delete km;
                }
            }
            return 0;
        case WM_KEYPARTY_EXIT:
            if (host) exitKioskToMenu(host);
            return 0;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_MOVE:
            if (host) {
                for (auto &entry : host->windows) {
                    if (entry.second.hwnd == hwnd) emit(host, entry.second, kWindowFrame);
                }
            }
            return 0;
        case WM_TIMER:
            if (host) {
                for (auto &entry : host->windows) emit(host, entry.second, kFrame);
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (host) {
                for (auto &entry : host->windows) {
                    if (entry.second.hwnd == hwnd) {
                        destroyChildWebViewsForWindow(host, entry.first);
                        entry.second.hwnd = nullptr;
                        emit(host, entry.second, kWindowFrame);
                    }
                }
                bool any_open = false;
                for (auto &entry : host->windows) any_open = any_open || entry.second.hwnd;
                if (!any_open) PostQuitMessage(0);
            }
            return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static ATOM registerClass(Host *host) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = windowProc;
    wc.hInstance = host->instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"ZeroNativeWindowsHost";
    return RegisterClassExW(&wc);
}

static bool createNativeWindow(Host *host, Window &window) {
    registerClass(host);
    std::wstring title = widen(window.title.empty() ? host->window_title : window.title);
    HWND hwnd = CreateWindowExW(
        0,
        L"ZeroNativeWindowsHost",
        title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        (int)window.width,
        (int)window.height,
        nullptr,
        nullptr,
        host->instance,
        host);
    if (!hwnd) return false;
    window.hwnd = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetTimer(hwnd, 1, 16, nullptr);
    return true;
}

} // namespace

extern "C" {

void zero_native_windows_load_window_webview(Host *host, uint64_t window_id, const char *source, size_t source_len, int source_kind, const char *asset_root, size_t asset_root_len, const char *asset_entry, size_t asset_entry_len, const char *asset_origin, size_t asset_origin_len, int spa_fallback);
void zero_native_windows_bridge_respond_window(Host *host, uint64_t window_id, const char *response, size_t response_len);

Host *zero_native_windows_create(const char *app_name, size_t app_name_len, const char *window_title, size_t window_title_len, const char *bundle_id, size_t bundle_id_len, const char *icon_path, size_t icon_path_len, const char *window_label, size_t window_label_len, double x, double y, double width, double height, int restore_frame) {
    (void)restore_frame;
    Host *host = new Host();
    host->app_name = slice(app_name, app_name_len);
    host->window_title = slice(window_title, window_title_len);
    host->bundle_id = slice(bundle_id, bundle_id_len);
    host->icon_path = slice(icon_path, icon_path_len);
    // Keep the WebView2 user-data store under %LOCALAPPDATA% (the exe folder may
    // be read-only, e.g. Program Files).
    wchar_t local_app_data[MAX_PATH] = {};
    DWORD local_len = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH);
    if (local_len > 0 && local_len < MAX_PATH) {
        std::wstring app = host->app_name.empty() ? std::wstring(L"zero-native") : widen(host->app_name);
        host->user_data_folder = std::wstring(local_app_data) + L"\\" + app + L"\\WebView2";
    }
    Window window;
    window.id = 1;
    window.label = slice(window_label, window_label_len);
    window.title = host->window_title.empty() ? host->app_name : host->window_title;
    window.x = x;
    window.y = y;
    window.width = width;
    window.height = height;
    host->windows[window.id] = window;
    return host;
}

void zero_native_windows_destroy(Host *host) {
    if (!host) return;
    if (g_kiosk_host == host) {
        if (g_keyboard_hook) {
            UnhookWindowsHookEx(g_keyboard_hook);
            g_keyboard_hook = nullptr;
        }
        g_kiosk_host = nullptr;
    }
    std::shared_ptr<HostLifetime> lifetime = host->lifetime;
    std::lock_guard<std::recursive_mutex> guard(lifetime->mutex);
    lifetime->alive = false;
    destroyAllWindows(host);
    delete host;
}

void zero_native_windows_run(Host *host, EventCallback callback, void *context) {
    if (!host) return;
    // WebView2 needs COM initialized (STA) on the UI thread.
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool co_initialized = SUCCEEDED(co);
    host->callback = callback;
    host->callback_context = context;
    host->running = true;
    if (!host->windows.empty()) createNativeWindow(host, host->windows.begin()->second);
    WindowsEvent start = {};
    start.kind = kStart;
    start.window_id = 1;
    callback(context, &start);
    for (auto &entry : host->windows) {
        emit(host, entry.second, kResize);
        emit(host, entry.second, kWindowFrame);
    }
    MSG message = {};
    while (host->running && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    WindowsEvent shutdown = {};
    shutdown.kind = kShutdown;
    shutdown.window_id = 1;
    callback(context, &shutdown);
    if (co_initialized) CoUninitialize();
}

void zero_native_windows_stop(Host *host) {
    if (!host) return;
    host->running = false;
    PostQuitMessage(0);
}

void zero_native_windows_load_webview(Host *host, const char *source, size_t source_len, int source_kind, const char *asset_root, size_t asset_root_len, const char *asset_entry, size_t asset_entry_len, const char *asset_origin, size_t asset_origin_len, int spa_fallback) {
    zero_native_windows_load_window_webview(host, 1, source, source_len, source_kind, asset_root, asset_root_len, asset_entry, asset_entry_len, asset_origin, asset_origin_len, spa_fallback);
}

void zero_native_windows_load_window_webview(Host *host, uint64_t window_id, const char *source, size_t source_len, int source_kind, const char *asset_root, size_t asset_root_len, const char *asset_entry, size_t asset_entry_len, const char *asset_origin, size_t asset_origin_len, int spa_fallback) {
    (void)asset_origin;
    (void)asset_origin_len;
    if (!host) return;
    auto found = host->windows.find(window_id);
    if (found == host->windows.end()) return;
    Window &w = found->second;
    w.load_kind = source_kind;
    w.load_source = slice(source, source_len);
    w.asset_root = slice(asset_root, asset_root_len);
    w.asset_entry = slice(asset_entry, asset_entry_len);
    w.spa_fallback = spa_fallback != 0;
#if ZERO_NATIVE_HAS_WEBVIEW2
    if (w.main_webview) {
        applyMainLoad(host, w);
    } else {
        ensureMainWebView(host, window_id);
    }
#else
    // This binary was compiled without WebView2 support, so there is no web view
    // to show — say so instead of leaving a silently blank window.
    MessageBoxW(w.hwnd,
                L"KeyParty was built without WebView2 support (WebView2.h / wrl.h were "
                L"not found at compile time), so it can only show a blank window.",
                L"KeyParty", MB_OK | MB_ICONERROR);
#endif
    emit(host, w, kWindowFrame);
}

void zero_native_windows_set_bridge_callback(Host *host, BridgeCallback callback, void *context) {
    if (!host) return;
    host->bridge_callback = callback;
    host->bridge_context = context;
}

void zero_native_windows_bridge_respond(Host *host, const char *response, size_t response_len) {
    zero_native_windows_bridge_respond_window(host, 1, response, response_len);
}

void zero_native_windows_bridge_respond_window(Host *host, uint64_t window_id, const char *response, size_t response_len) {
    if (!host) return;
    std::string resp = (response && response_len > 0) ? std::string(response, response_len) : std::string("{}");
    std::wstring js = L"window.zero&&window.zero._complete(" + widen(resp) + L");";
    executeMainScript(host, window_id, js);
}

void zero_native_windows_bridge_respond_webview(Host *host, uint64_t window_id, const char *webview_label, size_t webview_label_len, const char *response, size_t response_len) {
    std::string label = slice(webview_label, webview_label_len);
    if (label.empty() || label == "main") {
        zero_native_windows_bridge_respond_window(host, window_id, response, response_len);
        return;
    }
    // Child-webview bridges are not supported on this host (matches the runtime,
    // which returns UnsupportedWebViewBridge for non-main labels).
    (void)host;
}

void zero_native_windows_emit_window_event(Host *host, uint64_t window_id, const char *name, size_t name_len, const char *detail_json, size_t detail_json_len) {
    if (!host) return;
    std::string event_name = slice(name, name_len);
    std::string detail = (detail_json && detail_json_len > 0) ? std::string(detail_json, detail_json_len) : std::string("null");
    std::wstring js = L"window.zero&&window.zero._emit(" + jsonQuote(widen(event_name)) + L"," + widen(detail) + L");";
    executeMainScript(host, window_id, js);
}

void zero_native_windows_set_security_policy(Host *host, const char *allowed_origins, size_t allowed_origins_len, const char *external_urls, size_t external_urls_len, int external_action) {
    if (!host) return;
    host->allowed_origins = parseNewlineList(allowed_origins, allowed_origins_len);
    host->allowed_external_urls = parseNewlineList(external_urls, external_urls_len);
    host->external_link_action = external_action;
}

int zero_native_windows_create_window(Host *host, uint64_t window_id, const char *window_title, size_t window_title_len, const char *window_label, size_t window_label_len, double x, double y, double width, double height, int restore_frame) {
    (void)restore_frame;
    if (!host || host->windows.find(window_id) != host->windows.end()) return 0;
    Window window;
    window.id = window_id;
    window.title = slice(window_title, window_title_len);
    window.label = slice(window_label, window_label_len);
    window.x = x;
    window.y = y;
    window.width = width;
    window.height = height;
    bool ok = createNativeWindow(host, window);
    if (!ok) return 0;
    host->windows[window_id] = window;
    return 1;
}

int zero_native_windows_focus_window(Host *host, uint64_t window_id) {
    if (!host) return 0;
    auto found = host->windows.find(window_id);
    if (found == host->windows.end() || !found->second.hwnd) return 0;
    SetForegroundWindow(found->second.hwnd);
    SetFocus(found->second.hwnd);
    return 1;
}

int zero_native_windows_close_window(Host *host, uint64_t window_id) {
    if (!host) return 0;
    auto found = host->windows.find(window_id);
    if (found == host->windows.end() || !found->second.hwnd) return 0;
    destroyChildWebViewsForWindow(host, window_id);
    DestroyWindow(found->second.hwnd);
    return 1;
}

int zero_native_windows_create_webview(Host *host, uint64_t window_id, const char *label, size_t label_len, const char *url, size_t url_len, double x, double y, double width, double height, int layer, int transparent, int bridge_enabled) {
#if !ZERO_NATIVE_HAS_WEBVIEW2
    (void)host;
    (void)window_id;
    (void)label;
    (void)label_len;
    (void)url;
    (void)url_len;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)layer;
    (void)transparent;
    (void)bridge_enabled;
    return 0;
#else
    if (!host || label_len == 0 || url_len == 0 || !validChildWebViewFrame(x, y, width, height) || bridge_enabled) return 0;
    auto window = host->windows.find(window_id);
    if (window == host->windows.end() || !window->second.hwnd) return 0;
    std::string label_string = slice(label, label_len);
    std::string key = webViewKey(window_id, label_string);
    if (host->webviews.find(key) != host->webviews.end()) return 0;

    std::string url_string = slice(url, url_len);
    HWND hwnd = CreateWindowExW(
        0,
        L"STATIC",
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        webViewCoord(x),
        webViewCoord(y),
        webViewExtent(width),
        webViewExtent(height),
        window->second.hwnd,
        nullptr,
        host->instance,
        nullptr);
    if (!hwnd) return 0;

    ChildWebView webview;
    webview.window_id = window_id;
    webview.hwnd = hwnd;
    webview.label = label_string;
    webview.url = url_string;
    webview.x = x;
    webview.y = y;
    webview.width = width;
    webview.height = height;
    webview.layer = layer;
    webview.creation_order = host->next_webview_order++;
    webview.transparent = transparent != 0;
    webview.bridge_enabled = bridge_enabled != 0;
    host->webviews[key] = webview;
    applyChildWebViewLayer(host, window_id, label_string);
    if (!createChildWebView(host, key)) {
        DestroyWindow(hwnd);
        host->webviews.erase(key);
        return 0;
    }
    return 1;
#endif
}

int zero_native_windows_set_webview_frame(Host *host, uint64_t window_id, const char *label, size_t label_len, double x, double y, double width, double height) {
    if (!host || label_len == 0 || !validChildWebViewFrame(x, y, width, height)) return 0;
    if (slice(label, label_len) == "main") return 1;
    auto found = host->webviews.find(webViewKey(window_id, slice(label, label_len)));
    if (found == host->webviews.end() || !found->second.hwnd) return 0;
    found->second.x = x;
    found->second.y = y;
    found->second.width = width;
    found->second.height = height;
    MoveWindow(found->second.hwnd, webViewCoord(x), webViewCoord(y), webViewExtent(width), webViewExtent(height), TRUE);
#if ZERO_NATIVE_HAS_WEBVIEW2
    if (found->second.controller) {
        RECT bounds = webViewRect(found->second);
        found->second.controller->put_Bounds(bounds);
    }
#endif
    return 1;
}

int zero_native_windows_navigate_webview(Host *host, uint64_t window_id, const char *label, size_t label_len, const char *url, size_t url_len) {
#if !ZERO_NATIVE_HAS_WEBVIEW2
    (void)host;
    (void)window_id;
    (void)label;
    (void)label_len;
    (void)url;
    (void)url_len;
    return 0;
#else
    if (!host || label_len == 0 || url_len == 0) return 0;
    auto found = host->webviews.find(webViewKey(window_id, slice(label, label_len)));
    if (found == host->webviews.end() || !found->second.hwnd) return 0;
    found->second.url = slice(url, url_len);
    if (found->second.webview) {
        std::wstring target = widen(found->second.url);
        found->second.webview->Navigate(target.c_str());
        return 1;
    }
    // WebView2 initializes asynchronously; keep the newest URL and apply it in the creation callback.
    return 1;
#endif
}

int zero_native_windows_set_webview_zoom(Host *host, uint64_t window_id, const char *label, size_t label_len, double zoom) {
#if !ZERO_NATIVE_HAS_WEBVIEW2
    (void)host;
    (void)window_id;
    (void)label;
    (void)label_len;
    (void)zoom;
    return 0;
#else
    if (!host || label_len == 0 || zoom < 0.25 || zoom > 5.0) return 0;
    std::string label_string = slice(label, label_len);
    if (label_string == "main") return 0;
    auto found = host->webviews.find(webViewKey(window_id, label_string));
    if (found == host->webviews.end() || !found->second.hwnd) return 0;
    found->second.zoom = zoom;
    if (found->second.controller) {
        found->second.controller->put_ZoomFactor(zoom);
    }
    return 1;
#endif
}

int zero_native_windows_set_webview_layer(Host *host, uint64_t window_id, const char *label, size_t label_len, int layer) {
    if (!host || label_len == 0) return 0;
    std::string label_string = slice(label, label_len);
    if (label_string == "main") return 0;
    auto found = host->webviews.find(webViewKey(window_id, label_string));
    if (found == host->webviews.end() || !found->second.hwnd) return 0;
    found->second.layer = layer;
    applyChildWebViewLayer(host, window_id, label_string);
    return 1;
}

int zero_native_windows_close_webview(Host *host, uint64_t window_id, const char *label, size_t label_len) {
    if (!host || label_len == 0) return 0;
    auto found = host->webviews.find(webViewKey(window_id, slice(label, label_len)));
    if (found == host->webviews.end()) return 0;
#if ZERO_NATIVE_HAS_WEBVIEW2
    if (found->second.controller) found->second.controller->Close();
#endif
    if (found->second.hwnd) DestroyWindow(found->second.hwnd);
    host->webviews.erase(found);
    return 1;
}

size_t zero_native_windows_clipboard_read(Host *host, char *buffer, size_t buffer_len) {
    (void)host;
    if (!buffer || buffer_len == 0 || !OpenClipboard(nullptr)) return 0;
    HANDLE handle = GetClipboardData(CF_TEXT);
    if (!handle) {
        CloseClipboard();
        return 0;
    }
    const char *text = static_cast<const char *>(GlobalLock(handle));
    if (!text) {
        CloseClipboard();
        return 0;
    }
    size_t len = boundedLen(text, buffer_len);
    memcpy(buffer, text, len);
    GlobalUnlock(handle);
    CloseClipboard();
    return len;
}

void zero_native_windows_clipboard_write(Host *host, const char *text, size_t text_len) {
    (void)host;
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, text_len + 1);
    if (handle) {
        char *dest = static_cast<char *>(GlobalLock(handle));
        memcpy(dest, text, text_len);
        dest[text_len] = '\0';
        GlobalUnlock(handle);
        SetClipboardData(CF_TEXT, handle);
    }
    CloseClipboard();
}

}
