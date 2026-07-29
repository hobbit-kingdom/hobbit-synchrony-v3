// ============================================================================
//  custom_page.h  —  CUSTOM UI PAGE for THE HOBBIT (2003) PC  (own-template build)
//  Meridian.exe, Entropy engine, MSVC, 32-bit, image base 0x00400000, ASLR OFF.
//
//  Registers OUR OWN dialog template and lets the ENGINE build it. This gives a
//  real, self-contained page: clean (no foreign game content), centred at any
//  resolution (engine runs our design coords through scaleX/scaleY), with the
//  controls in the dialog's navigation grid and a base ui_dialog vtable
//  (Render draws frame + children, default OnPadSelect). One of each control
//  type: frame2 backdrop (opaque fill), text label, button, slider, checkbox, radio.
//
//  ---- FLAGS (control+0x2c), recovered from the binary's render/hit-test gates --
//   bit 0x1 = VISIBLE: a control is only drawn if set (container draw-children
//             FUN_006a30e0 and every per-control Render gate on (flags&1)).
//   bit 0x2 = NOT HITTABLE: hit-test FUN_006a31a0 requires (flags&2)==0 to click.
//   => interactive controls use 0x1; the backdrop + text labels use 0x3 so they
//      render but stay click-through, and we keep them out of the nav grid by
//      setting colSpan/rowSpan = 0 so they never steal keyboard/pad focus.
//
//  WHY THIS INSTEAD OF INJECTING INTO A LIVE PAGE: hosting on "pause summary"
//  dragged in its custom-drawn stats (not clearable) and a page/arrow input model
//  our controls couldn't join. A dialog the engine builds from a template owns its
//  controls, grid, navigation, and input — so it's clean and navigable.
//
//  ---- HOW IT WORKS (all verified from the binary) --------------------------
//   * UI_MGR = *(void**)0x0081303c (ui_manager, ECX/this for the ui calls)
//   * UI_CTX = *(void**)0x00813038 (screen/context, first arg / control ctx)
//   * RegisterDialogTemplate(ECX=UI_MGR, name, dialog_tem*, factory)   0x6a13b0 ret 0xc
//   * factory(__cdecl): (int, ui_manager*, dialog_tem* TEMPLATE, irect* rect,
//                        ui_win* parent, int, void*) -> ui_dialog*
//   * base ui_dialog ctor   0x6a22e0  (__thiscall; installs base vtable 0x6fcc14)
//   * base ui_dialog::Create 0x6a2400 (__thiscall ret 0x1c;
//        this, p1, ui_manager, dialog_tem*, irect* rect, ui_win* parent, p6, p7)
//        -> builds one control per template descriptor:
//           ctrl = CreateControlByName(ctx, descr.className, &rect, dlg, descr.flags)
//           SetID(descr.id); SetText(resolveString(descr.textId))
//        and places it into the nav grid cell (descr.col,row,cspan,rspan).
//   * operator_new 0x6c035e (__cdecl)   GetControlById 0x6a3320 (__thiscall)
//
//  dialog_tem (5 ints): { titleStrId, gridCols, gridRows, ctrlCount, descriptors* }
//  descriptor (12 ints, stride 0x30):
//     [0]id [1]textStrId [2]className* [3]X [4]Y [5]W [6]H
//     [7]col [8]row [9]colSpan [10]rowSpan [11]flags
//  X,Y are 640x480 DESIGN coords (engine centres them); W,H are pixel size.
//  We pass textStrId=0 (empty) and RELABEL each control via SetText after opening,
//  so labels are our own UTF-16 strings (not the game's string table).
//
//  BUILD: 32-bit MSVC injected DLL. Header-only. Call ShowCustomPage() on the
//  game main thread while a level/menu is up (a hotkey is ideal for testing).
// ============================================================================
#pragma once
#include <cstdint>
#include <cstring>

namespace hobbit_ui {

namespace addr {
    constexpr uintptr_t UI_MGR_PTR  = 0x0081303c;
    constexpr uintptr_t UI_CTX_PTR  = 0x00813038;
    constexpr uintptr_t OpenDialog  = 0x006a1430;
    constexpr uintptr_t RegisterTpl = 0x006a13b0;
    constexpr uintptr_t BaseCtor    = 0x006a22e0;
    constexpr uintptr_t BaseCreate  = 0x006a2400;
    constexpr uintptr_t GetCtrlById = 0x006a3320;
    constexpr uintptr_t OperatorNew = 0x006c035e;
    constexpr uintptr_t CreateCtrl  = 0x0069fe40; // (for the legacy inject helper)
    constexpr uintptr_t SetId       = 0x006a32c0;
    constexpr uintptr_t CloseDialog = 0x006a16c0; // __stdcall(ctx, popTopFlag)  ret 8
    constexpr uintptr_t SliderRange = 0x006a5e20; // __thiscall(slider, MIN, MAX)  ret 8

    // Pause: FUN_0052d740(active, screen) __thiscall ret 8 — active=0 fully resumes the
    // game AND closes the open pause dialog (+ resets state). The Pause_Mgr menu instance
    // is a fixed global; +0x14 != 0 means the pause menu is active.
    constexpr uintptr_t PauseToggle    = 0x0052d740;
    constexpr uintptr_t PauseMgr       = 0x00763208; // Pause_Mgr menu instance (this for PauseToggle)
    constexpr int       PAUSE_ACTIVE   = 0x14;       // != 0 while the pause menu is up
    constexpr uintptr_t RenderMainWin  = 0x0052cd20; // Pause_Mgr::RenderMainWindow(this,0) — summary bg
    constexpr int       DLG_RENDER_SLOT = 2;         // ui_dialog vtable slot for Render (+0x08)
    constexpr int       CTRL_RECT      = 0x44;       // control screen rect [L,T,R,B] (frame draws from here)

    // control-instance field offsets (verified from the render/hit-test code)
    constexpr int CTRL_FLAGS  = 0x2c;   // bit 0x1=visible 0x2=nohit 0x200=CHECKED 0x400=focused
    constexpr int CTRL_ID     = 0x30;   // SetID writes here; OnPadSelect reads the clicked id here
    constexpr int SLIDER_MIN  = 0x10c;  // slider min (SetRange arg1)
    constexpr int SLIDER_MAX  = 0x110;  // slider max (SetRange arg2)
    constexpr int SLIDER_VAL  = 0x120;  // CURRENT slider value (what OnPadNavigate updates)
    constexpr uint32_t CHECK_BIT = 0x200; // checkbox/radio "checked/selected" bit (0x400 = focus!)
    constexpr int DLG_ONPAD_SLOT = 28;  // ui_dialog vtable slot for OnPadSelect (+0x70)

    // UI_CTX active-dialog stack (used to check our page is still open before reading it)
    constexpr int CTX_DLG_COUNT = 0x5c8; // number of dialogs on the stack
    constexpr int CTX_DLG_ARRAY = 0x5cc; // pointer to the array of dialog*  (stack[count-1] = top)
}

inline void* UiMgr() { return *reinterpret_cast<void**>(addr::UI_MGR_PTR); }
inline void* UiCtx() { return *reinterpret_cast<void**>(addr::UI_CTX_PTR); }

// ---- engine call types ------------------------------------------------------
using OpenDialog_t  = void* (__thiscall*)(void* uimgr, void* ctx, const char* name,
                                          int rL, int rT, int rR, int rB, int a, int b, int c);
using RegisterTpl_t = int   (__thiscall*)(void* uimgr, const char* name, void* tmpl, void* factory);
using BaseCtor_t    = void  (__thiscall*)(void* dlg);
using BaseCreate_t  = int   (__thiscall*)(void* dlg, int p1, void* uimgr, void* tmpl,
                                          void* rect, void* parent, int p6, void* p7);
using GetById_t     = void* (__thiscall*)(void* dlg, int id);
using New_t         = void* (__cdecl*)(uint32_t size);
using CloseDialog_t = void  (__stdcall*)(void* ctx, int popTopFlag);
using SliderRange_t = void  (__thiscall*)(void* slider, int minValue, int maxValue);
using OnPad_t       = void  (__thiscall*)(void* dlg, void* activatedControl);
using PauseToggle_t = void  (__thiscall*)(void* pauseMgr, int active, int screen);
using RenderMain_t  = void  (__thiscall*)(void* pauseMgr, int p1);
using DlgRender_t   = void  (__thiscall*)(void* dlg, int xOff, int yOff);

// Backdrop options (pick with a parameter):
//   BD_PANEL   : our own dark filled frame2 panel (resize with the scale param).
//   BD_SUMMARY : draw the real pause-summary background behind our controls
//                (calls Pause_Mgr::RenderMainWindow each frame; pause context only).
enum BackdropStyle { BD_PANEL = 0, BD_SUMMARY = 1 };

inline void SetText(void* ctrl, const wchar_t* text) {       // control vtbl+0x1c, wide string
    if (!ctrl || !text) return;
    uint8_t* vtbl = *reinterpret_cast<uint8_t**>(ctrl);
    void* slot = *reinterpret_cast<void**>(vtbl + 0x1c);
    if (slot) reinterpret_cast<void(__thiscall*)(void*, const wchar_t*)>(slot)(ctrl, text);
}

// A frame control draws a FILLED rect ONLY if its fill-colour alpha byte is non-zero.
// frame Render (FUN_006a37d0): `if (*(char*)(frame+0x13b)) DrawFilledRect(rect, frame+0x138)`.
// The frame ctor zeroes that colour (alpha 0 => no fill => only the thin border shows),
// so we set an opaque colour here to get a real backdrop. Colour is 4 bytes at +0x138;
// byte +0x13b is the alpha/enable. (RGB channel order is engine-internal; greys are safe.)
inline void SetFrameFill(void* frame, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!frame) return;
    uint8_t* col = reinterpret_cast<uint8_t*>(frame) + 0x138;
    col[0] = r; col[1] = g; col[2] = b; col[3] = a;   // a (+0x13b) != 0 enables the fill
}

// ---- our template -----------------------------------------------------------
enum CtrlId { ID_FRAME=1, ID_TITLE, ID_INFO, ID_BUTTON, ID_SLIDER, ID_CHECK,
              ID_CLOSE,
              // Extra single-line labels: one text control per line is reliable,
              // whereas a '\n' inside one control is not rendered as a line break.
              ID_INFO2, ID_INFO3, ID_INFO4 };

// flags (control+0x2c), recovered from the render/hit-test gates in the binary:
//   bit 0x1 = VISIBLE  (FUN_006a30e0 draws children & each ctrl Render require it)
//   bit 0x2 = NOT HITTABLE (FUN_006a31a0 hit-test needs (flags&2)==0)
// So 0x1 = visible + clickable; 0x3 = visible + click-through (labels/backdrop).
enum CtrlFlag { F_VISIBLE = 0x1, F_NOHIT = 0x2, F_LABEL = 0x3 /*visible, click-through*/ };

#pragma pack(push, 1)
struct Descriptor {  // 12 dwords = 0x30 bytes, field order EXACTLY as base Create reads
    int id; const char* textKey; const char* className;   // textKey is a STRING KEY (resolveString does strlen) — never 0; use ""
    int x; int y; int w; int h;
    int col; int row; int colSpan; int rowSpan; int flags;
};
struct DialogTem {   // 5 dwords
    const char* titleKey; int gridCols; int gridRows; int ctrlCount; const Descriptor* descriptors;  // titleKey: "" not 0
};
#pragma pack(pop)

// class-name strings (our own; CreateControlByName matches by string compare)
inline const char* C_FRAME2 = "frame2"; inline const char* C_TEXT = "text";
inline const char* C_BTN    = "button"; inline const char* C_SLIDER = "slider";
inline const char* C_CHECK  = "check";

// Layout: backdrop frame2 (the panel style the stock "options video pc" page uses),
// then a title + info label, then the interactive controls stacked inside it.
//   * frame + labels => F_LABEL (0x3): drawn, but click-through so the full-size
//     backdrop (built first) doesn't swallow clicks meant for the controls on top,
//     and they're kept OUT of the nav grid (colSpan/rowSpan = 0 => claims no cells).
//   * interactive controls => F_VISIBLE (0x1) and DO occupy nav-grid cells.
// X,Y,W,H are 640x480 design coords; the engine centres them via scaleX/scaleY, so
// the render position is independent of the (col,row) grid which only drives nav.
inline const Descriptor* Descriptors() {
    // NOTE: the text field is a string KEY (resolveString runs strlen on it) — use ""
    // not 0, or the engine strlen(NULL)s and crashes. We relabel via SetText afterwards.
    // Big panel: 576x400 centred in the 640x480 design space, with the controls
    // laid out INSIDE it so frame and contents stay aligned at any resolution.
    static const Descriptor d[] = {
        // id        txt cls        x    y    w    h   col row csp rsp flags
        { ID_FRAME,   "", C_FRAME2,  32,  40, 576, 400,  0,  0,  0,  0, F_LABEL   },
        { ID_TITLE,   "", C_TEXT,     58,  70, 524,  30,  0,  0,  0,  0, F_LABEL  },
        { ID_INFO,    "", C_TEXT,     68, 126, 504,  24,  0,  0,  0,  0, F_LABEL  },
        { ID_INFO2,   "", C_TEXT,     68, 154, 504,  24,  0,  0,  0,  0, F_LABEL  },
        { ID_INFO3,   "", C_TEXT,     68, 182, 504,  24,  0,  0,  0,  0, F_LABEL  },
        { ID_INFO4,   "", C_TEXT,     68, 210, 504,  24,  0,  0,  0,  0, F_LABEL  },
        { ID_BUTTON,  "", C_BTN,      98, 274, 444,  32,  0,  0,  2,  1, F_VISIBLE },
        { ID_SLIDER,  "", C_SLIDER,   98, 318, 444,  22,  0,  1,  2,  1, F_VISIBLE },
        { ID_CHECK,   "", C_CHECK,    98, 318, 444,  22,  0,  2,  2,  1, F_VISIBLE },
        { ID_CLOSE,   "", C_BTN,     190, 356, 260,  32,  0,  3,  2,  1, F_VISIBLE },
    };
    return d;
}
inline DialogTem* Template() {
    static DialogTem t = { "", /*cols*/2, /*rows*/4, /*count*/10, Descriptors() };
    return &t;
}

// Our factory: build a plain ui_dialog (base ctor + base Create) from the template.
// __cdecl, 7 args — matches the engine's factory signature; OpenDialog calls it.
inline void* __cdecl KitchenFactory(int p1, void* uimgr, void* tmpl, void* rect,
                                    void* parent, int p6, void* p7) {
    void* dlg = reinterpret_cast<New_t>(addr::OperatorNew)(0x220);   // generous base size
    if (!dlg) return nullptr;
    reinterpret_cast<BaseCtor_t>(addr::BaseCtor)(dlg);
    reinterpret_cast<BaseCreate_t>(addr::BaseCreate)(dlg, p1, uimgr, tmpl, rect, parent, p6, nullptr);
    return dlg;
}

inline bool& RegisteredFlag() { static bool b = false; return b; }
inline void RegisterTemplate() {
    if (RegisteredFlag()) return;
    void* uimgr = UiMgr();
    if (!uimgr) return;
    reinterpret_cast<RegisterTpl_t>(addr::RegisterTpl)(
        uimgr, "kitchen_sink", Template(), reinterpret_cast<void*>(&KitchenFactory));
    RegisteredFlag() = true;
}

// ============================================================================
//  READING STATE + "WHAT WAS CLICKED"
// ----------------------------------------------------------------------------
//  Two kinds of input:
//   * Self-handling controls (slider/checkbox) update their OWN state in place —
//     just READ it any time with the getters below (no hook needed):
//        GetCheck(dlg)        -> bool   (checkbox ticked?)   [flag bit 0x400]
//        GetSliderValue(dlg)  -> int    (0..max, default max=100)  [slider+0x10c]
//   * Buttons fire the dialog's OnPadSelect(activatedControl) when pressed. The
//     base default just bubbles to the parent (does nothing for us), so we swap
//     THIS dialog's vtable slot 28 (+0x70) for our own handler, read the clicked
//     control's id at control+0x30, invoke your callback, and auto-close on Close.
//  Set your callback once:  hobbit_ui::OnClick() = &MyHandler;   // void(int id,void* dlg)
// ============================================================================

inline void* GetById(void* dlg, int id) {
    return dlg ? reinterpret_cast<GetById_t>(addr::GetCtrlById)(dlg, id) : nullptr;
}
inline int  GetControlId(void* ctrl) {
    return ctrl ? *reinterpret_cast<int*>(reinterpret_cast<char*>(ctrl) + addr::CTRL_ID) : 0;
}

// ---- tracked "active page" --------------------------------------------------
// The hook/ShowCustomPage stores the open dialog here so you can read it from
// anywhere (e.g. an ImGui loop) WITHOUT having the dlg pointer in scope.
inline void*& CurrentPage() { static void* p = nullptr; return p; }

// Is this dialog still on the engine's active-dialog stack? (Guards against
// reading a freed dialog after the page was dismissed by any means.)
inline bool IsPageOpen(void* dlg) {
    if (!dlg) return false;
    void* ctx = UiCtx();
    if (!ctx) return false;
    int    count = *reinterpret_cast<int*>(reinterpret_cast<char*>(ctx) + addr::CTX_DLG_COUNT);
    void** stack = *reinterpret_cast<void***>(reinterpret_cast<char*>(ctx) + addr::CTX_DLG_ARRAY);
    if (!stack || count <= 0 || count > 64) return false;
    for (int i = 0; i < count; ++i) if (stack[i] == dlg) return true;
    return false;
}
// The currently-open page, or nullptr if it's been closed. Use this for reads.
inline void* ActivePage() { void* d = CurrentPage(); return IsPageOpen(d) ? d : nullptr; }

// ---- read control state (pass a dlg, OR use the no-arg form on the active page)
inline bool GetCheck(void* dlg) {
    void* c = GetById(dlg, ID_CHECK);
    return c && (*reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(c) + addr::CTRL_FLAGS)
                 & addr::CHECK_BIT) != 0;
}
inline int  GetSliderValue(void* dlg) {
    void* s = GetById(dlg, ID_SLIDER);
    return s ? *reinterpret_cast<int*>(reinterpret_cast<char*>(s) + addr::SLIDER_VAL) : 0;
}
inline int  GetSliderMax(void* dlg) {
    void* s = GetById(dlg, ID_SLIDER);
    return s ? *reinterpret_cast<int*>(reinterpret_cast<char*>(s) + addr::SLIDER_MAX) : 0;
}
inline void SetSliderRange(void* dlg, int minValue, int maxValue) {   // SetRange(min, max)
    void* s = GetById(dlg, ID_SLIDER);
    if (s) reinterpret_cast<SliderRange_t>(addr::SliderRange)(s, minValue, maxValue);
}
inline void SetSliderValue(void* dlg, int value) {                   // write current value (+0x120)
    void* s = GetById(dlg, ID_SLIDER);
    if (!s) return;
    int lo = *reinterpret_cast<int*>(reinterpret_cast<char*>(s) + addr::SLIDER_MIN);
    int hi = *reinterpret_cast<int*>(reinterpret_cast<char*>(s) + addr::SLIDER_MAX);
    if (value < lo) value = lo;
    if (value > hi) value = hi;
    *reinterpret_cast<int*>(reinterpret_cast<char*>(s) + addr::SLIDER_VAL) = value;
}
// No-arg convenience overloads — read the tracked active page (safe if closed).
inline bool GetCheck()       { return GetCheck(ActivePage()); }
inline int  GetSliderValue() { return GetSliderValue(ActivePage()); }
inline int  GetSliderMax()   { return GetSliderMax(ActivePage()); }
inline bool IsPageOpen()     { return ActivePage() != nullptr; }

// User-settable click callback: receives the activated control's id + the dialog.
using ClickFn = void (*)(int id, void* dlg);
inline ClickFn& OnClick()   { static ClickFn fn = nullptr; return fn; }
inline OnPad_t& OrigOnPad() { static OnPad_t  fn = nullptr; return fn; }

// True while the pause menu is active (i.e. our page was opened by rerouting pause).
inline bool PauseActive() {
    return *reinterpret_cast<int*>(reinterpret_cast<char*>(
               reinterpret_cast<void*>(addr::PauseMgr)) + addr::PAUSE_ACTIVE) != 0;
}

// ---- backdrop configuration (settable before opening / by the hook path) -----
inline BackdropStyle& BackdropMode()  { static BackdropStyle s = BD_PANEL; return s; }
inline float&         BackdropScale() { static float f = 1.0f; return f; } // enlarge the BD_PANEL frame
inline DlgRender_t&   OrigRender()    { static DlgRender_t fn = nullptr; return fn; }

// Enlarge (or shrink) the backdrop frame in place: scale its screen rect about its
// centre. scale=1.0 leaves it unchanged; 1.5 = 50% bigger. Resolution-independent
// (operates on the already-centred, already-scaled rect — no scaleX/Y needed).
inline void ResizeFrame(void* dlg, float scale) {
    void* f = GetById(dlg, ID_FRAME);
    if (!f || scale == 1.0f) return;
    int* r = reinterpret_cast<int*>(reinterpret_cast<char*>(f) + addr::CTRL_RECT); // L,T,R,B
    int cx = (r[0] + r[2]) / 2, cy = (r[1] + r[3]) / 2;
    int hw = static_cast<int>((r[2] - r[0]) * 0.5f * scale);
    int hh = static_cast<int>((r[3] - r[1]) * 0.5f * scale);
    int L = cx - hw, T = cy - hh, R = cx + hw, B = cy + hh;
    int* r34 = reinterpret_cast<int*>(reinterpret_cast<char*>(f) + 0x34); // the other rect copy
    r[0] = L; r[1] = T; r[2] = R; r[3] = B;
    r34[0] = L; r34[1] = T; r34[2] = R; r34[3] = B;
}

// Render replacement (only installed for BD_SUMMARY): draw the real pause-summary
// background first, then our dialog as normal. Pause-only (guarded).
inline void __fastcall RenderHook(void* dlg, void* /*edx*/, int x, int y) {
    if (PauseActive())
        reinterpret_cast<RenderMain_t>(addr::RenderMainWin)(
            reinterpret_cast<void*>(addr::PauseMgr), 0);
    if (OrigRender()) OrigRender()(dlg, x, y);
}

// Close our page AND, if we're in the pause menu, resume the game properly.
inline void ClosePage() {
    if (PauseActive()) {
        // FUN_0052d740(active=0, screen=0): resumes game objects, closes the dialog,
        // deactivates the UI layer and resets pause state — the engine's own resume.
        reinterpret_cast<PauseToggle_t>(addr::PauseToggle)(
            reinterpret_cast<void*>(addr::PauseMgr), 0, 0);
    } else {
        // Opened standalone (e.g. a hotkey) — just pop our dialog off the UI stack.
        reinterpret_cast<CloseDialog_t>(addr::CloseDialog)(UiCtx(), 1);
    }
    CurrentPage() = nullptr;
}

// Our OnPadSelect replacement (engine calls it __thiscall(dlg, activatedControl)).
inline void __fastcall OnPadHook(void* dlg, void* /*edx*/, void* ctrl) {
    int id = GetControlId(ctrl);
    if (OnClick()) OnClick()(id, dlg);
    if (id == ID_CLOSE) {                                   // dismiss the page (+ resume if paused)
        ClosePage();
        return;                                             // dialog popped; don't bubble
    }
    if (OrigOnPad()) OrigOnPad()(dlg, ctrl);                // base bubbles to parent (harmless)
}

// Per-instance vtable swap: copy this dialog's vtable, redirect slot 28 to OnPadHook
// (clicks) and — for BD_SUMMARY — slot 2 to RenderHook (summary backdrop). Only our
// dialog is affected (other dialogs keep the original shared vtable).
inline void InstallClickHandler(void* dlg) {
    if (!dlg) return;
    static void* vcopy[64];                                 // one page open at a time
    void** orig = *reinterpret_cast<void***>(dlg);
    for (int i = 0; i < 64; ++i) vcopy[i] = orig[i];
    OrigOnPad() = reinterpret_cast<OnPad_t>(orig[addr::DLG_ONPAD_SLOT]);
    vcopy[addr::DLG_ONPAD_SLOT] = reinterpret_cast<void*>(&OnPadHook);
    if (BackdropMode() == BD_SUMMARY) {
        OrigRender() = reinterpret_cast<DlgRender_t>(orig[addr::DLG_RENDER_SLOT]);
        vcopy[addr::DLG_RENDER_SLOT] = reinterpret_cast<void*>(&RenderHook);
    }
    *reinterpret_cast<void***>(dlg) = vcopy;
}

// Apply the chosen backdrop:
//   BD_PANEL   -> resize our frame by BackdropScale() and give it the dark fill.
//   BD_SUMMARY -> hide our frame (clear visible bit) so the summary background (drawn
//                 by RenderHook) shows through behind the controls.
inline void ApplyBackdrop(void* dlg) {
    void* f = GetById(dlg, ID_FRAME);
    if (BackdropMode() == BD_SUMMARY) {
        if (f) *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(f) + addr::CTRL_FLAGS) &= ~1u;
    } else {
        ResizeFrame(dlg, BackdropScale());
        SetFrameFill(f, 0x14, 0x16, 0x20, 0xDC);
    }
}

// Apply our own UTF-16 labels (controls were built with empty text).
inline void Relabel(void* dlg) {
    if (!dlg) return;
    auto get = reinterpret_cast<GetById_t>(addr::GetCtrlById);
    struct L { int id; const wchar_t* t; };
    static const L labels[] = {
        { ID_TITLE,  L"CUSTOM PAGE" },
        { ID_INFO,   L"text / button / slider / checkbox" },
        { ID_BUTTON, L"A Button" },
        { ID_SLIDER, L"A Slider" },
        { ID_CHECK,  L"A Checkbox" },
        { ID_CLOSE,  L"Close" },
    };
    for (const auto& l : labels) SetText(get(dlg, l.id), l.t);
    InstallClickHandler(dlg);   // so button/Close presses reach our callback (+ summary render)
    ApplyBackdrop(dlg);         // BD_PANEL (resized dark fill) or BD_SUMMARY (real summary bg)
    // Slider: range 0..100, start at 50 (the LIVE value lives at slider+0x120).
    SetSliderRange(dlg, 0, 100);
    SetSliderValue(dlg, 50);
    CurrentPage() = dlg;        // remember it so GetCheck()/GetSliderValue() work anywhere
}

// ---- show the page ----------------------------------------------------------
// Registers the template once, then opens it. The engine builds + centres +
// pushes + focuses the dialog; we relabel afterwards.
//   style       : BD_PANEL (our dark panel) or BD_SUMMARY (real pause-summary bg).
//   panelScale  : only for BD_PANEL — enlarge the panel (1.0 = default, 1.6 = bigger).
// (These just set BackdropMode()/BackdropScale(); the pause-hook path can set those
//  globals directly instead of calling this.)
inline void* ShowCustomPage(BackdropStyle style = BD_PANEL, float panelScale = 1.0f) {
    BackdropMode()  = style;
    BackdropScale() = panelScale;
    RegisterTemplate();
    void* uimgr = UiMgr(); void* ctx = UiCtx();
    if (!uimgr || !ctx) return nullptr;
    void* dlg = reinterpret_cast<OpenDialog_t>(addr::OpenDialog)(
                    uimgr, ctx, "kitchen_sink", 0, 0, 640, 480, 0, 5, 0);
    if (dlg) Relabel(dlg);
    return dlg;
}

// Pause reroute: open OUR template instead of the summary.
//   void* __fastcall hk_OpenDialog(void* mgr, void*, void* ctx, const char* name,
//                                  int rL,int rT,int rR,int rB,int a,int b,int c){
//       if (name && std::strcmp(name,"pause summary")==0){
//           // choose the backdrop BEFORE Relabel:
//           hobbit_ui::BackdropMode()  = hobbit_ui::BD_SUMMARY;  // real summary background
//           // -- or --  BackdropMode()=BD_PANEL; BackdropScale()=1.6f;  // bigger dark panel
//           hobbit_ui::RegisterTemplate();
//           void* dlg = o_OpenDialog(mgr, ctx, "kitchen_sink", rL,rT,rR,rB, a,b,c);
//           if (dlg) hobbit_ui::Relabel(dlg);   // applies backdrop + click handler + labels
//           return dlg;
//       }
//       return o_OpenDialog(mgr, ctx, name, rL,rT,rR,rB, a,b,c);
//   }
//
// ---- READING WHAT WAS CLICKED (set this once, e.g. at DLL init) -------------
//   void MyHandler(int id, void* dlg) {
//       using namespace hobbit_ui;
//       switch (id) {
//           case ID_BUTTON:  /* A Button pressed */
//               printf("slider=%d  checkbox=%d\n", GetSliderValue(dlg), GetCheck(dlg));
//               break;
//           case ID_CLOSE:   /* Close pressed — page auto-closes after this */ break;
//           case ID_CHECK:   /* checkbox toggled; read GetCheck(dlg) */ break;
//       }
//   }
//   hobbit_ui::OnClick() = &MyHandler;   // install once; fires on every control press
//
// Slider value & checkbox state can ALSO be polled any time without a click:
//   int  v = hobbit_ui::GetSliderValue(dlg);   // 0..100
//   bool c = hobbit_ui::GetCheck(dlg);         // ticked?

// Diagnostic: open a STOCK control-rich page (no custom code) to confirm engine calls.
inline void* ShowStockOptionsPage() {
    void* uimgr = UiMgr(); void* ctx = UiCtx();
    if (!uimgr || !ctx) return nullptr;
    return reinterpret_cast<OpenDialog_t>(addr::OpenDialog)(uimgr, ctx, "options video pc", 0,0,640,480, 0,5,0);
}

} // namespace hobbit_ui
