// DualAudio - window + tray app.
// Main headphone = Windows default output (plays directly). Mirror = second headphone, delayed to match.
// Sync is tuned automatically (laptop mic + soft chirps) on start, on reconnect, and when devices change,
// or on demand with the Resync button; the result is remembered per headphone pair. If the mirror turns out to be the slower one, the app
// swaps roles by itself.
#include "calibrate.h"
namespace Gdiplus { using std::min; using std::max; }
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <map>
#include <tuple>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Gdiplus;

enum : UINT { WM_APP_DEVICES = WM_APP + 1, WM_APP_SYNCDONE, WM_APP_TRAY, WM_APP_SHOW };
enum { TM_TICK = 1, TM_RESTART = 2, TM_RETRY = 3 };
static const wchar_t* kClass = L"DualAudioWnd";
static const float kW = 420, kH = 452;

// ---------------------------------------------------------------- palette
static const Color cBg(255, 15, 17, 22), cCard(255, 26, 29, 36), cCardHi(255, 33, 37, 46),
    cLine(255, 44, 49, 60), cText(255, 236, 239, 244), cSub(255, 150, 157, 168), cDim(255, 96, 103, 116),
    cTrack(255, 46, 51, 62), cAccent(255, 92, 141, 255), cGreen(255, 52, 211, 153), cAmber(255, 251, 191, 36),
    cRed(255, 248, 113, 113);

// ---------------------------------------------------------------- config (dualaudio.cfg next to the exe)
struct Profile { std::wstring def, mir; int delay; };
static struct Cfg {
    bool on = true, noneChosen = false, hinted = false;
    std::wstring mirror, lastMain;   // the remembered pair: lastMain plays directly, mirror is delayed
    std::wstring mirrorName, mainName;   // last known names, shown while a headphone is away
    std::vector<Profile> profiles;
} cfg;

static std::wstring ExeDir() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p; return s.substr(0, s.find_last_of(L"\\/") + 1);
}
static std::vector<std::wstring> SplitTabs(const std::wstring& line) {
    std::vector<std::wstring> v; size_t a = 0;
    for (;;) { size_t b = line.find(L'\t', a); v.push_back(line.substr(a, b - a)); if (b == std::wstring::npos) break; a = b + 1; }
    return v;
}
static void SaveCfg() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, (ExeDir() + L"dualaudio.cfg").c_str(), L"w, ccs=UTF-8") || !f) return;
    fwprintf(f, L"on\t%d\nnone\t%d\nhinted\t%d\nmirror\t%s\nmain\t%s\nmirrorname\t%s\nmainname\t%s\n", cfg.on, cfg.noneChosen,
             cfg.hinted, cfg.mirror.c_str(), cfg.lastMain.c_str(), cfg.mirrorName.c_str(), cfg.mainName.c_str());
    for (auto& p : cfg.profiles) fwprintf(f, L"pair\t%d\t%s\t%s\n", p.delay, p.def.c_str(), p.mir.c_str());
    fclose(f);
}
static void LoadCfg(const std::wstring& curDefault) {
    FILE* f = nullptr;
    wchar_t line[2048];
    if (!_wfopen_s(&f, (ExeDir() + L"dualaudio.cfg").c_str(), L"r, ccs=UTF-8") && f) {
        while (fgetws(line, 2048, f)) {
            std::wstring s = line; while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
            auto v = SplitTabs(s);
            if (v[0] == L"on" && v.size() > 1) cfg.on = _wtoi(v[1].c_str()) != 0;
            else if (v[0] == L"none" && v.size() > 1) cfg.noneChosen = _wtoi(v[1].c_str()) != 0;
            else if (v[0] == L"hinted" && v.size() > 1) cfg.hinted = _wtoi(v[1].c_str()) != 0;
            else if (v[0] == L"mirror" && v.size() > 1) cfg.mirror = v[1];
            else if (v[0] == L"main" && v.size() > 1) cfg.lastMain = v[1];
            else if (v[0] == L"mirrorname" && v.size() > 1) cfg.mirrorName = v[1];
            else if (v[0] == L"mainname" && v.size() > 1) cfg.mainName = v[1];
            else if (v[0] == L"pair" && v.size() > 3) cfg.profiles.push_back({v[2], v[3], _wtoi(v[1].c_str())});
        }
        fclose(f);
        return;
    }
    // first run: migrate the console version's dualaudio.ini ("delay<TAB>mirrorId")
    if (!_wfopen_s(&f, (ExeDir() + L"dualaudio.ini").c_str(), L"r, ccs=UTF-8") && f) {
        while (fgetws(line, 2048, f)) {
            std::wstring s = line; while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
            auto v = SplitTabs(s);
            if (v.size() > 1 && !curDefault.empty()) {
                cfg.mirror = v[1]; cfg.lastMain = curDefault;
                cfg.profiles.push_back({curDefault, v[1], _wtoi(v[0].c_str())});
            }
        }
        fclose(f);
        SaveCfg();
    }
}
static Profile* FindProfile(const std::wstring& d, const std::wstring& m) {
    for (auto& p : cfg.profiles) if (p.def == d && p.mir == m) return &p;
    return nullptr;
}
static void SetProfile(const std::wstring& d, const std::wstring& m, int delay) {
    if (Profile* p = FindProfile(d, m)) p->delay = delay; else cfg.profiles.push_back({d, m, delay});
    SaveCfg();
}

// ---------------------------------------------------------------- startup with Windows
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static bool StartupEnabled() {
    HKEY k; if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &k)) return false;
    bool on = RegQueryValueExW(k, L"DualAudio", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k); return on;
}
static void SetStartup(bool on) {
    HKEY k; if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k)) return;
    if (on) {
        wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
        std::wstring v = L"\"" + std::wstring(p) + L"\" --tray";
        RegSetValueExW(k, L"DualAudio", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else RegDeleteValueW(k, L"DualAudio");
    RegCloseKey(k);
}

// ---------------------------------------------------------------- app state (UI thread)
static HWND g_hwnd = nullptr;
static UINT g_dpi = 96, g_taskbarCreated = 0;
static bool g_visible = false, g_startup = false;
static NOTIFYICONDATAW g_nid{};
static HICON g_icoBig = nullptr, g_icoSm = nullptr;
static FontFamily *g_ff = nullptr, *g_ffSemi = nullptr, *g_ffIcon = nullptr;
static bool g_semiIsBold = false;

static std::vector<OutDev> g_devs;
static std::wstring g_defId, g_defName;
static Target* g_t = nullptr;

enum Sync { S_NONE, S_SYNCING, S_SYNCED, S_SAVED, S_UNSYNCED, S_SWAPPING, S_MANUAL, S_FAILED };
static bool g_userSync = false;   // current tuning was started with the Resync button
static Sync g_sync = S_NONE;
static ULONGLONG g_syncedAt = 0;
static double g_residual = NAN;
static int g_retries = 0;
static bool g_swappedOnce = false;
static std::wstring g_swapName;

static std::thread g_syncTh;
static std::atomic<bool> g_syncCancel{false};
static bool g_syncBusy = false;
static UINT g_syncGen = 0;
struct SyncMsg { UINT gen; CalResult r; };

static int g_prevState = -1;
static float g_lvlMain = 0, g_lvlMir = 0;
enum Hot { H_NONE, H_TOGGLE, H_CARD1, H_CARD2, H_MINUS, H_PLUS, H_STARTUP, H_QUIT, H_RESYNC };
static int g_hover = H_NONE, g_press = H_NONE;

// ---------------------------------------------------------------- device helpers
static std::wstring NameOf(const std::wstring& id) {
    for (auto& d : g_devs) if (d.id == id) return d.name;
    return L"";
}
static bool IsActive(const std::wstring& id) {
    if (id.empty()) return false;
    for (auto& d : g_devs) if (d.id == id) return true;
    return false;
}
static bool LooksLikeHeadphone(const std::wstring& n) {
    return n.rfind(L"Headphones", 0) == 0 || n.rfind(L"Headset", 0) == 0 || n.rfind(L"Earphones", 0) == 0 ||
           n.find(L"AirPods") != std::wstring::npos || n.find(L"Buds") != std::wstring::npos;
}
// "Headphones (Ank's AirPods Pro - Find My)" -> "Ank's AirPods Pro"
static std::wstring Pretty(const std::wstring& raw) {
    std::wstring s = raw;
    size_t a = s.find(L" (");
    if (a != std::wstring::npos && !s.empty() && s.back() == L')') {
        std::wstring kind = s.substr(0, a);
        if (kind == L"Headphones" || kind == L"Headset" || kind == L"Speakers" || kind == L"Earphones")
            s = s.substr(a + 2, s.size() - a - 3);
    }
    size_t f = s.find(L" - Find My"); if (f != std::wstring::npos) s.erase(f);
    return s.empty() ? raw : s;
}
static const wchar_t* GlyphFor(const std::wstring& raw) {
    if (raw.empty()) return L"";                       // Add
    return LooksLikeHeadphone(raw) ? L"" : L""; // Headphone / Speakers
}

// ---------------------------------------------------------------- engine control
static void Update();
static void StartAutoSync(bool full = false);

static void CancelSync() {
    if (g_syncBusy) g_syncCancel = true;
    if (g_syncTh.joinable()) g_syncTh.join();
    g_syncBusy = false;
    g_syncGen++;
}
static void EngineStop() {
    CancelSync();
    if (g_t) { StopEngine(); g_t = nullptr; }
    g_prevState = -1;
}
static void RefreshDevices() { g_devs = EnumOutputs(&g_defId, &g_defName); }

// The pair is never forgotten because of a disconnect: we only start when the current Windows default
// is the remembered main (or another headphone, e.g. roles swapped) and the mirror is connected.
static bool PairReady() {
    if (!cfg.on || g_defId.empty() || !IsActive(cfg.mirror) || cfg.mirror == g_defId) return false;
    return cfg.lastMain.empty() || g_defId == cfg.lastMain || LooksLikeHeadphone(g_defName);
}
static void EngineRestart() {
    EngineStop();
    KillTimer(g_hwnd, TM_RETRY);
    RefreshDevices();
    // the mirror became the Windows default (e.g. the main reconnected later, or the user switched):
    // swap roles if the old main is here, otherwise just wait - never drop the remembered pair
    if (!cfg.mirror.empty() && cfg.mirror == g_defId && IsActive(cfg.lastMain) && cfg.lastMain != g_defId) {
        std::swap(cfg.mirror, cfg.lastMain); SaveCfg();
    }
    // no mirror chosen yet -> pick the other headphone automatically
    if (cfg.mirror.empty() && !cfg.noneChosen) {
        for (auto& d : g_devs) if (d.id != g_defId && LooksLikeHeadphone(d.name)) { cfg.mirror = d.id; SaveCfg(); break; }
    }
    if (g_sync != S_SWAPPING) g_sync = S_NONE;
    if (PairReady()) {
        Profile* p = FindProfile(g_defId, cfg.mirror);
        g_t = StartEngine(cfg.mirror, NameOf(cfg.mirror), p ? p->delay : 0);
        if (g_t) {
            cfg.lastMain = g_defId; cfg.mainName = g_defName; cfg.mirrorName = NameOf(cfg.mirror); SaveCfg();
            g_retries = 0; StartAutoSync();
        }
    }
    Update();
}

static void StartAutoSync(bool full) {
    if (!g_t || g_syncBusy) return;
    if (g_syncTh.joinable()) g_syncTh.join();
    bool known = !full && FindProfile(g_defId, cfg.mirror) != nullptr;
    g_userSync = full;
    g_syncBusy = true; g_syncCancel = false; g_sync = S_SYNCING;
    UINT gen = ++g_syncGen; Target* t = g_t; HWND hw = g_hwnd;
    g_syncTh = std::thread([=] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (int i = 0; i < 150 && !g_syncCancel; i++) Sleep(10);   // let the streams settle
        auto* m = new SyncMsg{gen, {}};
        // soft chirps; a known pair gets a short check, a new pair a full measurement + verify
        if (!g_syncCancel) m->r = Calibrate(t, 0.12f, known ? 2 : 3, !known, nullptr, &g_syncCancel);
        CoUninitialize();
        PostMessageW(hw, WM_APP_SYNCDONE, 0, (LPARAM)m);
    });
    Update();
}

static void OnSyncDone(SyncMsg* raw) {
    std::unique_ptr<SyncMsg> m(raw);
    if (m->gen != g_syncGen) return;           // stale (engine restarted meanwhile)
    if (g_syncTh.joinable()) g_syncTh.join();
    g_syncBusy = false;
    if (!g_t) return;
    const CalResult& r = m->r;
    if (r.ok && r.mirrorSlower && !g_swappedOnce) {
        // the mirror lags even with zero delay -> it must be the main one. Swap roles.
        g_swappedOnce = true; g_sync = S_SWAPPING; g_swapName = Pretty(NameOf(cfg.mirror));
        std::wstring newMain = cfg.mirror;
        cfg.mirror = g_defId; cfg.lastMain = newMain; std::swap(cfg.mirrorName, cfg.mainName); SaveCfg();
        SetDefaultOutput(newMain);
        SetTimer(g_hwnd, TM_RESTART, 1500, nullptr);   // device notification usually beats this
        Update();
        return;
    }
    if (r.ok) {
        SetProfile(g_defId, cfg.mirror, g_t->delayMs);
        g_sync = S_SYNCED; g_syncedAt = GetTickCount64(); g_residual = r.residual; g_retries = 0;
        KillTimer(g_hwnd, TM_RETRY);
    } else if (g_userSync && FindProfile(g_defId, cfg.mirror)) {
        g_sync = S_FAILED;   // manual resync couldn't hear both: say so, keep the previous timing
    } else {
        g_sync = FindProfile(g_defId, cfg.mirror) ? S_SAVED : S_UNSYNCED;
        if (g_sync == S_UNSYNCED && g_retries < 4) { g_retries++; SetTimer(g_hwnd, TM_RETRY, 40000, nullptr); }
    }
    Update();
}

// ---------------------------------------------------------------- tray
static void TrayAdd() {
    g_nid = {}; g_nid.cbSize = sizeof(g_nid); g_nid.hWnd = g_hwnd; g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; g_nid.uCallbackMessage = WM_APP_TRAY; g_nid.hIcon = g_icoSm;
    wcscpy_s(g_nid.szTip, L"DualAudio");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
static void TrayTip(const std::wstring& s) {
    wcsncpy_s(g_nid.szTip, s.c_str(), _TRUNCATE);
    g_nid.uFlags = NIF_TIP; Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}
static void TrayBalloon(const wchar_t* title, const wchar_t* text) {
    g_nid.uFlags = NIF_INFO; g_nid.dwInfoFlags = NIIF_NONE;
    wcsncpy_s(g_nid.szInfoTitle, title, _TRUNCATE); wcsncpy_s(g_nid.szInfo, text, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// ---------------------------------------------------------------- status text
static std::wstring Ago(ULONGLONG ms) {
    ULONGLONG s = ms / 1000;
    if (s < 60) return L"just now";
    if (s < 3600) return std::to_wstring(s / 60) + L" min ago";
    return std::to_wstring(s / 3600) + L" h ago";
}
static std::wstring StatusText(Color& dot) {
    std::wstring mir = Pretty(NameOf(cfg.mirror)), mainN = Pretty(g_defName);
    if (!cfg.on) { dot = cDim; return L"Off. Sound plays only on " + mainN + L"."; }
    if (cfg.mirror.empty()) { dot = cDim; return L"Pick a second headphone to share the sound."; }
    if (g_sync == S_SWAPPING) { dot = cAmber; return g_swapName + L" is the slower one, so it becomes the main headphone. Re-tuning…"; }
    if (!PairReady()) {
        dot = cDim;
        std::wstring who = !IsActive(cfg.mirror) ? (cfg.mirrorName.empty() ? L"the second headphone" : Pretty(cfg.mirrorName))
                                                 : (cfg.mainName.empty() ? L"the main headphone" : Pretty(cfg.mainName));
        return L"Waiting for " + who + L" to connect… It will start and tune by itself.";
    }
    if (!g_t) { dot = cRed; return L"Couldn't open the audio devices. Reconnect a headphone to retry."; }
    if (g_t->state == 2) { dot = cAmber; return mir + L" dropped out. It will resume and re-tune when it's back."; }
    switch (g_sync) {
    case S_SYNCING:  dot = cAmber; return L"Tuning the sync… you may hear a few soft chirps.";
    case S_SYNCED: {
        dot = cGreen;
        std::wstring s = L"In sync. Tuned automatically " + Ago(GetTickCount64() - g_syncedAt);
        if (!std::isnan(g_residual)) { wchar_t b[32]; swprintf(b, 32, L" (±%d ms)", (int)lround(fabs(g_residual))); s += b; }
        return s + L".";
    }
    case S_SAVED:    dot = cGreen; return L"In sync, using the timing measured for this pair. Re-tunes by itself whenever it can hear both.";
    case S_MANUAL:   dot = cGreen; return L"In sync, with your fine-tune saved for this pair.";
    case S_FAILED:   dot = cAmber; return L"Couldn't hear both headphones, so the previous timing is kept. Lay them by the laptop and resync.";
    case S_UNSYNCED: dot = cAmber; return L"Not tuned yet: lay both headphones next to the laptop for a moment. It retries by itself.";
    default:         dot = cDim; return L"Starting…";
    }
}
static void Update() {
    std::wstring tip = L"DualAudio · ";
    if (!cfg.on) tip += L"off"; else if (g_t && g_sync != S_SYNCING) tip += L"in sync +" + std::to_wstring(g_t->delayMs.load()) + L" ms"; else tip += L"starting";
    TrayTip(tip);
    if (g_hwnd && g_visible) InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------- drawing helpers
static Font* GetFont(FontFamily* fam, float px, int style) {
    static std::map<std::tuple<void*, int, int>, Font*> cache;
    auto key = std::make_tuple((void*)fam, (int)(px * 10), style);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    Font* f = new Font(fam, px, style, UnitPixel);
    cache[key] = f; return f;
}
static void RRPath(GraphicsPath& p, const RectF& r, float rad) {
    rad = std::min(rad, std::min(r.Width, r.Height) / 2); float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90); p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90); p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}
static void FillRR(Graphics& g, const RectF& r, float rad, const Brush& b) { GraphicsPath p; RRPath(p, r, rad); g.FillPath(&b, &p); }
static void FillRR(Graphics& g, const RectF& r, float rad, const Color& c) { SolidBrush b(c); FillRR(g, r, rad, b); }
static void StrokeRR(Graphics& g, const RectF& r, float rad, const Color& c, float w = 1) { GraphicsPath p; RRPath(p, r, rad); Pen pen(c, w); g.DrawPath(&pen, &p); }
static void Txt(Graphics& g, const std::wstring& s, const RectF& r, float px, const Color& c, bool semi = false,
                StringAlignment ha = StringAlignmentNear, bool wrap = false, StringAlignment va = StringAlignmentNear) {
    Font* f = GetFont(semi ? g_ffSemi : g_ff, px, semi && g_semiIsBold ? FontStyleBold : FontStyleRegular);
    SolidBrush b(c);
    StringFormat sf; sf.SetAlignment(ha); sf.SetLineAlignment(va); sf.SetTrimming(StringTrimmingEllipsisCharacter);
    if (!wrap) sf.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(s.c_str(), -1, f, r, &sf, &b);
}
static void Glyph(Graphics& g, const wchar_t* gl, const RectF& r, float px, const Color& c) {
    Font* f = GetFont(g_ffIcon, px, FontStyleRegular);
    SolidBrush b(c); StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(gl, -1, f, r, &sf, &b);
}
static float TextW(Graphics& g, const std::wstring& s, float px, bool semi) {
    Font* f = GetFont(semi ? g_ffSemi : g_ff, px, semi && g_semiIsBold ? FontStyleBold : FontStyleRegular);
    RectF bb; g.MeasureString(s.c_str(), -1, f, PointF(0, 0), &bb); return bb.Width;
}
static Color WithA(const Color& c, BYTE a) { return Color(a, c.GetR(), c.GetG(), c.GetB()); }

// ---------------------------------------------------------------- layout (DIPs)
static const RectF rToggle(kW - 24 - 48, 30, 48, 26);
static const RectF rCard1(16, 92, kW - 32, 100);
static const RectF rCard2(16, 228, kW - 32, 100);
static const RectF rMinus(kW - 16 - 16 - 30 - 8 - 30, 228 + 22, 30, 30);
static const RectF rPlus(kW - 16 - 16 - 30, 228 + 22, 30, 30);
static const RectF rStatus(16, 344, kW - 32, 60);
static const RectF rStartup(22, kH - 38, 190, 24);
static const RectF rQuit(kW - 22 - 60, kH - 38, 60, 24);

static const RectF rResync(kW - 16 - 14 - 96, 344 + 15, 96, 30);

static bool FineTuneVisible() { return g_t && cfg.on && g_sync != S_SYNCING && g_sync != S_SWAPPING && g_t->state == 1; }
static bool ResyncVisible() { return g_t && cfg.on && g_t->state == 1 && g_sync != S_SWAPPING; }
static bool ResyncEnabled() { return ResyncVisible() && !g_syncBusy; }

static int HitTest(float x, float y) {
    auto in = [&](const RectF& r) { return x >= r.X && x < r.X + r.Width && y >= r.Y && y < r.Y + r.Height; };
    if (in(rToggle)) return H_TOGGLE;
    if (ResyncEnabled() && in(rResync)) return H_RESYNC;
    if (FineTuneVisible()) { if (in(rMinus)) return H_MINUS; if (in(rPlus)) return H_PLUS; }
    if (in(rCard1)) return H_CARD1;
    if (in(rCard2)) return H_CARD2;
    if (in(rStartup)) return H_STARTUP;
    if (in(rQuit)) return H_QUIT;
    return H_NONE;
}

static RectF MeterRect(const RectF& card) { return RectF(card.X + 80, card.Y + card.Height - 22, card.Width - 100, 5); }

static void DrawCard(Graphics& g, const RectF& r, bool hover, bool dim, const wchar_t* glyph, const Color& tint,
                     const std::wstring& tag, const std::wstring& title, const std::wstring& role, float level, float rightPad) {
    FillRR(g, r, 16, hover ? cCardHi : cCard);
    StrokeRR(g, r, 16, Color(22, 255, 255, 255));
    RectF ic(r.X + 18, r.Y + 20, 46, 46);
    SolidBrush icb(WithA(tint, dim ? 20 : 38)); g.FillEllipse(&icb, ic);
    Glyph(g, glyph, ic, 21, dim ? cDim : tint);

    float x = r.X + 80, w = r.Width - 80 - rightPad;
    float tw = TextW(g, tag, 10.5f, true) + 12;
    RectF chip(x, r.Y + 18, tw, 18);
    FillRR(g, chip, 9, WithA(tint, dim ? 18 : 34));
    Txt(g, tag, chip, 10.5f, dim ? cDim : tint, true, StringAlignmentCenter, false, StringAlignmentCenter);
    Txt(g, role, RectF(x + tw + 8, r.Y + 18, w - tw - 8, 18), 12, cSub, false, StringAlignmentNear, false, StringAlignmentCenter);
    Txt(g, title, RectF(x - 1, r.Y + 40, w, 26), 17, dim ? cSub : cText, true);

    (void)level;
    FillRR(g, MeterRect(r), 2.5f, cTrack);
}

static void PaintStatic(Graphics& g) {
    bool on = cfg.on;
    Txt(g, L"DualAudio", RectF(22, 20, 260, 34), 25, cText, true);
    Txt(g, L"One movie, two headphones", RectF(24, 54, 260, 20), 13, cSub);

    {   // power toggle
        bool hv = g_hover == H_TOGGLE;
        FillRR(g, rToggle, 13, on ? (hv ? Color(255, 112, 157, 255) : cAccent) : (hv ? Color(255, 58, 64, 77) : cTrack));
        float kx = on ? rToggle.X + rToggle.Width - 23 : rToggle.X + 3;
        SolidBrush kb(on ? Color(255, 255, 255, 255) : cSub);
        g.FillEllipse(&kb, kx, rToggle.Y + 3, 20.f, 20.f);
    }

    // main card
    DrawCard(g, rCard1, g_hover == H_CARD1, false, GlyphFor(g_defName), cAccent, L"MAIN", Pretty(g_defName),
             L"plays directly", g_t ? g_lvlMain : 0.f, 34);
    Glyph(g, L"", RectF(rCard1.X + rCard1.Width - 40, rCard1.Y + 40, 24, 24), 12, cDim);

    {   // connector + pill
        float cx = kW / 2;
        Pen ln(cLine, 2); ln.SetDashStyle(DashStyleDot);
        g.DrawLine(&ln, cx, rCard1.Y + rCard1.Height, cx, rCard2.Y);
        std::wstring pill; Color pc = cDim;
        if (!on) pill = L"off";
        else if (!g_t) pill = IsActive(cfg.mirror) ? L"…" : L"waiting";
        else if (g_sync == S_SYNCING || g_sync == S_SWAPPING) { pill = L"tuning…"; pc = cAmber; }
        else if (g_t->state == 2) { pill = L"reconnecting"; pc = cAmber; }
        else { pill = L"in sync · +" + std::to_wstring(g_t->delayMs.load()) + L" ms"; pc = g_sync == S_UNSYNCED ? cAmber : cGreen; }
        float pw2 = TextW(g, pill, 12, true) + 26;
        RectF pr(cx - pw2 / 2, rCard1.Y + rCard1.Height + 7, pw2, 22);
        FillRR(g, pr, 11, cBg);
        StrokeRR(g, pr, 11, WithA(pc, 120));
        SolidBrush db(pc); g.FillEllipse(&db, pr.X + 10, pr.Y + 8.5f, 5.f, 5.f);
        Txt(g, pill, RectF(pr.X + 18, pr.Y, pr.Width - 22, pr.Height), 12, pc, true, StringAlignmentCenter, false, StringAlignmentCenter);
    }

    {   // mirror card
        std::wstring raw = NameOf(cfg.mirror);
        bool has = !cfg.mirror.empty();
        bool live = g_t && on;
        std::wstring role;
        if (!has) role = L"click to choose";
        else if (!on) role = L"paused";
        else if (!IsActive(cfg.mirror)) role = L"not connected";
        else if (g_t && g_t->state == 2) role = L"reconnecting";
        else if (g_t) role = L"delayed " + std::to_wstring(g_t->delayMs.load()) + L" ms to match";
        else role = L"starting";
        std::wstring title = has ? Pretty(raw.empty() ? (cfg.mirrorName.empty() ? L"Second headphone" : cfg.mirrorName) : raw)
                                 : L"Add a second headphone";
        bool ft = FineTuneVisible();
        DrawCard(g, rCard2, g_hover == H_CARD2, !live, GlyphFor(has ? (raw.empty() ? L"Headphones" : raw) : L""), cGreen,
                 L"MIRROR", title, role, live ? g_lvlMir : 0.f, ft ? 88 : 20);
        if (ft) {
            const RectF* bs[2] = {&rMinus, &rPlus}; const wchar_t* gl[2] = {L"", L""};
            for (int i = 0; i < 2; i++) {
                bool hv = g_hover == (i ? H_PLUS : H_MINUS);
                SolidBrush bb(hv ? Color(255, 58, 64, 77) : Color(255, 40, 44, 54)); g.FillEllipse(&bb, *bs[i]);
                Glyph(g, gl[i], *bs[i], 11, hv ? cText : cSub);
            }
            Txt(g, L"fine-tune", RectF(rMinus.X - 4, rMinus.Y + rMinus.Height + 2, rPlus.X + rPlus.Width - rMinus.X + 8, 16), 10.5f, cDim,
                false, StringAlignmentCenter);
        }
    }

    {   // status
        FillRR(g, rStatus, 14, cCard);
        Color dot; std::wstring s = StatusText(dot);
        SolidBrush halo(WithA(dot, 50)); g.FillEllipse(&halo, rStatus.X + 14, rStatus.Y + rStatus.Height / 2 - 7, 14.f, 14.f);
        SolidBrush db(dot); g.FillEllipse(&db, rStatus.X + 18, rStatus.Y + rStatus.Height / 2 - 3, 6.f, 6.f);
        bool rv = ResyncVisible();
        Txt(g, s, RectF(rStatus.X + 40, rStatus.Y + 6, rStatus.Width - 54 - (rv ? 104 : 0), rStatus.Height - 12), 12.5f, cText, false,
            StringAlignmentNear, true, StringAlignmentCenter);
        if (rv) {   // Resync button
            bool en = ResyncEnabled(), hv = en && g_hover == H_RESYNC;
            FillRR(g, rResync, 15, hv ? Color(255, 58, 64, 77) : Color(255, 38, 42, 52));
            StrokeRR(g, rResync, 15, en ? WithA(cAccent, hv ? 200 : 110) : cLine);
            Color fc = en ? (hv ? cText : Color(255, 190, 205, 255)) : cDim;
            Glyph(g, L"", RectF(rResync.X + 8, rResync.Y, 24, rResync.Height), 12, fc);
            Txt(g, en ? L"Resync" : L"Tuning…", RectF(rResync.X + 30, rResync.Y, rResync.Width - 36, rResync.Height), 12.5f, fc, true,
                StringAlignmentNear, false, StringAlignmentCenter);
        }
    }

    {   // footer
        bool hv = g_hover == H_STARTUP;
        RectF box(rStartup.X, rStartup.Y + 4, 16, 16);
        if (g_startup) FillRR(g, box, 4, cAccent); else StrokeRR(g, box, 4, hv ? cSub : cDim, 1.4f);
        if (g_startup) {
            Pen ck(Color(255, 255, 255, 255), 1.8f); ck.SetLineJoin(LineJoinRound); ck.SetStartCap(LineCapRound); ck.SetEndCap(LineCapRound);
            PointF pts[3] = {PointF(box.X + 4, box.Y + 8.5f), PointF(box.X + 7, box.Y + 11.5f), PointF(box.X + 12, box.Y + 5)};
            g.DrawLines(&ck, pts, 3);
        }
        Txt(g, L"Start with Windows", RectF(box.X + 24, rStartup.Y, 170, rStartup.Height), 12.5f, hv ? cText : cSub, false,
            StringAlignmentNear, false, StringAlignmentCenter);
        Txt(g, L"Quit", rQuit, 12.5f, g_hover == H_QUIT ? cText : cSub, false, StringAlignmentFar, false, StringAlignmentCenter);
    }
}

// Everything except the two meter fills is rendered once into a cached layer and only re-rendered when
// its content signature changes; each frame just blits that layer and draws the meters (cheap).
static HDC g_sDC = nullptr, g_fDC = nullptr;
static HBITMAP g_sBmp = nullptr, g_fBmp = nullptr;
static int g_bw = 0, g_bh = 0, g_qMain = -1, g_qMir = -1;
static std::wstring g_sig;

static std::wstring Signature() {
    Color d; std::wstring s = StatusText(d);
    s += L"|" + g_defName + L"|" + NameOf(cfg.mirror) + L"|" + cfg.mirror;
    s += L"|" + std::to_wstring(cfg.on) + std::to_wstring(g_hover) + std::to_wstring(g_startup) + std::to_wstring((int)g_sync) +
         std::to_wstring(g_t ? g_t->delayMs.load() : -1) + std::to_wstring(g_t ? g_t->state.load() : -1) +
         std::to_wstring(FineTuneVisible()) + std::to_wstring(g_dpi) + std::to_wstring(IsActive(cfg.mirror)) +
         std::to_wstring(g_syncBusy);
    return s;
}
static void PrepGraphics(Graphics& g) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.ScaleTransform(g_dpi / 96.f, g_dpi / 96.f);
}
static void MeterFill(Graphics& g, const RectF& card, const Color& tint, float level) {
    if (level <= 0.02f) return;
    RectF m = MeterRect(card), f = m;
    f.Width = std::max(6.f, m.Width * std::min(level, 1.f));
    LinearGradientBrush lg(PointF(m.X - 1, 0), PointF(m.X + m.Width + 1, 0), WithA(tint, 150), tint);
    FillRR(g, f, 2.5f, lg);
}
static void Paint(HDC hdc) {
    RECT rc; GetClientRect(g_hwnd, &rc);
    int w = rc.right, h = rc.bottom; if (w <= 0 || h <= 0) return;
    if (w != g_bw || h != g_bh || !g_sDC) {
        if (g_sDC) { DeleteDC(g_sDC); DeleteObject(g_sBmp); DeleteDC(g_fDC); DeleteObject(g_fBmp); }
        g_sDC = CreateCompatibleDC(hdc); g_sBmp = CreateCompatibleBitmap(hdc, w, h); SelectObject(g_sDC, g_sBmp);
        g_fDC = CreateCompatibleDC(hdc); g_fBmp = CreateCompatibleBitmap(hdc, w, h); SelectObject(g_fDC, g_fBmp);
        g_bw = w; g_bh = h; g_sig.clear();
    }
    std::wstring sig = Signature();
    if (sig != g_sig) {
        g_sig = sig;
        Graphics g(g_sDC); g.Clear(cBg); PrepGraphics(g); PaintStatic(g);
    }
    BitBlt(g_fDC, 0, 0, w, h, g_sDC, 0, 0, SRCCOPY);
    {
        Graphics g(g_fDC); PrepGraphics(g);
        if (g_t) MeterFill(g, rCard1, cAccent, g_lvlMain);
        if (g_t && cfg.on) MeterFill(g, rCard2, cGreen, g_lvlMir);
    }
    BitBlt(hdc, 0, 0, w, h, g_fDC, 0, 0, SRCCOPY);
}

// ---------------------------------------------------------------- menus & actions
static void ShowDeviceMenu(bool forMain) {
    RefreshDevices();
    HMENU m = CreatePopupMenu();
    std::vector<std::wstring> ids;
    for (auto& d : g_devs) {
        if (!forMain && d.id == g_defId) continue;
        bool chk = forMain ? d.id == g_defId : d.id == cfg.mirror;
        AppendMenuW(m, MF_STRING | (chk ? MF_CHECKED : 0), 100 + ids.size(), d.name.c_str());
        ids.push_back(d.id);
    }
    if (!forMain) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (cfg.mirror.empty() ? MF_CHECKED : 0), 99, L"None");
    }
    POINT pt; GetCursorPos(&pt);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    if (!cmd) return;
    if (cmd == 99) { cfg.mirror.clear(); cfg.noneChosen = true; SaveCfg(); EngineRestart(); return; }
    std::wstring id = ids[cmd - 100];
    if (forMain) {
        if (id == g_defId) return;
        if (id == cfg.mirror) cfg.mirror = g_defId;   // swap roles
        cfg.lastMain = id; SaveCfg();
        g_swappedOnce = false;
        SetDefaultOutput(id);
        SetTimer(g_hwnd, TM_RESTART, 1200, nullptr);
    } else {
        cfg.mirror = id; cfg.noneChosen = false; g_swappedOnce = false; SaveCfg();
        EngineRestart();
    }
}
static void SetOn(bool on) { cfg.on = on; SaveCfg(); EngineRestart(); }
static void ShowMain() {
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_hwnd);
    g_visible = true;
    SetTimer(g_hwnd, TM_TICK, 33, nullptr);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}
static void HideMain() {
    ShowWindow(g_hwnd, SW_HIDE);
    g_visible = false;
    SetTimer(g_hwnd, TM_TICK, 500, nullptr);
    if (!cfg.hinted) {
        cfg.hinted = true; SaveCfg();
        TrayBalloon(L"DualAudio is still running", L"It keeps both headphones in sync from the tray. Right-click the icon to quit.");
    }
}
static void Click(int h) {
    switch (h) {
    case H_TOGGLE: SetOn(!cfg.on); break;
    case H_CARD1: ShowDeviceMenu(true); break;
    case H_CARD2: ShowDeviceMenu(false); break;
    case H_MINUS: case H_PLUS:
        if (g_t) {
            g_t->delayMs = std::clamp(g_t->delayMs.load() + (h == H_PLUS ? 2 : -2), 0, 1500);
            SetProfile(g_defId, cfg.mirror, g_t->delayMs);
            g_sync = S_MANUAL; Update();
        }
        break;
    case H_RESYNC:
        if (ResyncEnabled()) { CancelSync(); g_retries = 0; StartAutoSync(true); }   // full measurement + verify
        break;
    case H_STARTUP: SetStartup(!g_startup); g_startup = StartupEnabled(); Update(); break;
    case H_QUIT: DestroyWindow(g_hwnd); break;
    }
}

// ---------------------------------------------------------------- device notifications
struct Notifier : IMMNotificationClient {
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** pp) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) { *pp = this; return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    void Ping() { if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DEVICES, 0, 0); }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { Ping(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { Ping(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { Ping(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow f, ERole r, LPCWSTR) override {
        if (f == eRender && r == eConsole) Ping(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};

// ---------------------------------------------------------------- window
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_taskbarCreated && msg == g_taskbarCreated) { TrayAdd(); Update(); return 0; }
    switch (msg) {
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); Paint(dc); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_TIMER:
        if (wp == TM_TICK) {
            if (g_t) {
                int st = g_t->state;
                if (g_prevState == 2 && st == 1) { CancelSync(); g_retries = 0; StartAutoSync(); }   // reconnected -> re-tune
                g_prevState = st;
            }
            float a = sqrtf(g_srcPeak.exchange(0) / 100.f), b = g_t ? sqrtf(g_t->outPeak.exchange(0) / 100.f) : 0.f;
            g_lvlMain = std::max(a, g_lvlMain * 0.86f); g_lvlMir = std::max(b, g_lvlMir * 0.86f);
            static ULONGLONG lastTip = 0;
            if (GetTickCount64() - lastTip > 2000) { lastTip = GetTickCount64(); Update(); }
            if (g_visible) {
                // repaint only when something visible changed (meters quantized to ~2 %)
                int qm = (int)(g_lvlMain * 50), qr = (int)(g_lvlMir * 50);
                if (qm != g_qMain || qr != g_qMir || Signature() != g_sig) { g_qMain = qm; g_qMir = qr; InvalidateRect(h, nullptr, FALSE); }
            }
        } else if (wp == TM_RESTART) {
            KillTimer(h, TM_RESTART);
            RefreshDevices();
            if (!g_t || g_defId != g_engDefId || g_sync == S_SWAPPING) EngineRestart();
            else Update();
        } else if (wp == TM_RETRY) {
            KillTimer(h, TM_RETRY);
            if (g_t && g_sync == S_UNSYNCED) StartAutoSync();
        }
        return 0;
    case WM_APP_DEVICES: SetTimer(h, TM_RESTART, 800, nullptr); return 0;
    case WM_APP_SYNCDONE: OnSyncDone((SyncMsg*)lp); return 0;
    case WM_APP_SHOW: ShowMain(); return 0;
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) ShowMain();
        else if (LOWORD(lp) == WM_RBUTTONUP) {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, L"Open DualAudio");
            AppendMenuW(m, MF_STRING, 2, cfg.on ? L"Turn off" : L"Turn on");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, 3, L"Quit");
            SetMenuDefaultItem(m, 1, FALSE);
            POINT pt; GetCursorPos(&pt); SetForegroundWindow(h);
            int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
            DestroyMenu(m);
            if (c == 1) ShowMain(); else if (c == 2) SetOn(!cfg.on); else if (c == 3) DestroyWindow(h);
        }
        return 0;
    case WM_MOUSEMOVE: {
        float x = GET_X_LPARAM(lp) * 96.f / g_dpi, y = GET_Y_LPARAM(lp) * 96.f / g_dpi;
        int ht = HitTest(x, y);
        if (ht != g_hover) { g_hover = ht; InvalidateRect(h, nullptr, FALSE); }
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0}; TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE: g_hover = H_NONE; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: g_press = g_hover; SetCapture(h); return 0;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        int p = g_press; g_press = H_NONE;
        if (p != H_NONE && p == g_hover) Click(p);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && g_hover != H_NONE) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
        break;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        RECT* r = (RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE: HideMain(); return 0;
    case WM_DESTROY:
        EngineStop();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HICON MakeIcon(int size) {
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0, 0, 0, 0));
    float s = (float)size;
    SolidBrush a(cAccent), b(Color(230, 52, 211, 153));
    g.FillEllipse(&a, s * 0.04f, s * 0.16f, s * 0.6f, s * 0.6f);
    g.FillEllipse(&b, s * 0.36f, s * 0.24f, s * 0.6f, s * 0.6f);
    HICON h = nullptr; bmp.GetHICON(&h); return h;
}
static void InitFonts() {
    auto pick = [](std::initializer_list<const wchar_t*> names) -> FontFamily* {
        for (auto n : names) { auto* f = new FontFamily(n); if (f->GetLastStatus() == Ok && f->IsAvailable()) return f; delete f; }
        return FontFamily::GenericSansSerif()->Clone();
    };
    g_ff = pick({L"Segoe UI Variable Text", L"Segoe UI"});
    FontFamily* semi = new FontFamily(L"Segoe UI Semibold");
    if (semi->GetLastStatus() == Ok && semi->IsAvailable()) g_ffSemi = semi;
    else { delete semi; g_ffSemi = g_ff; g_semiIsBold = true; }
    g_ffIcon = pick({L"Segoe Fluent Icons", L"Segoe MDL2 Assets"});
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR cmd, int) {
    HANDLE mx = CreateMutexW(nullptr, FALSE, L"Local\\DualAudio.Singleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND w = FindWindowW(kClass, nullptr)) PostMessageW(w, WM_APP_SHOW, 0, 0);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    GdiplusStartupInput gsi; ULONG_PTR gtok = 0; GdiplusStartup(&gtok, &gsi, nullptr);
    InitFonts();
    g_icoBig = MakeIcon(32); g_icoSm = MakeIcon(16);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = g_icoBig; wc.hIconSm = g_icoSm; wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    g_hwnd = CreateWindowExW(0, kClass, L"DualAudio", style, CW_USEDEFAULT, CW_USEDEFAULT, 200, 200, nullptr, nullptr, hi, nullptr);
    g_dpi = GetDpiForWindow(g_hwnd);
    RECT rc{0, 0, (LONG)lround(kW * g_dpi / 96.f), (LONG)lround(kH * g_dpi / 96.f)};
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, g_dpi);
    MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    SetWindowPos(g_hwnd, nullptr, (mi.rcWork.left + mi.rcWork.right - ww) / 2, (mi.rcWork.top + mi.rcWork.bottom - wh) / 2, ww, wh,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    BOOL dark = TRUE; DwmSetWindowAttribute(g_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    COLORREF capCol = RGB(15, 17, 22); DwmSetWindowAttribute(g_hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &capCol, sizeof(capCol));

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    TrayAdd();

    ComPtr<IMMDeviceEnumerator> en;
    static Notifier notifier;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))))
        en->RegisterEndpointNotificationCallback(&notifier);

    RefreshDevices();
    LoadCfg(g_defId);
    g_startup = StartupEnabled();
    EngineRestart();

    if (cmd && wcsstr(cmd, L"--tray")) SetTimer(g_hwnd, TM_TICK, 500, nullptr);
    else ShowMain();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }

    if (en) en->UnregisterEndpointNotificationCallback(&notifier);
    GdiplusShutdown(gtok);
    CoUninitialize();
    if (mx) CloseHandle(mx);
    return 0;
}
