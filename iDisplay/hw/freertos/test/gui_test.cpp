/*
 * gui_test.cpp
 *
 * Host test for the GUI: iTextSurface and MonoCanvas, the screen stack
 * (xGuiCore), xMenu, the fields, xTimeHHMM/xTimeHHMMSS, xButton's debouncing, and xGui and
 * xGuiButton over a simulated CMSIS-RTOS2 queue (stub/cmsis_os2.h). No
 * hardware, HAL or RTOS needed. ID is the iDisplay folder:
 *
 *   g++ -std=c++17 -Wall -Wextra -Istub -I../inc -I$ID/inc -I$ID/gui/inc \
 *       gui_test.cpp ../src/xGui.cpp $ID/gui/src/xGuiCore.cpp $ID/gui/src/xMenu.cpp \
 *       $ID/gui/src/xFields.cpp $ID/gui/src/xButton.cpp $ID/gui/src/xTime.cpp $ID/src/iTextSurface.cpp \
 *       $ID/src/MonoCanvas.cpp $ID/src/Font5x7.cpp -o gui_test
 */

#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include "xGui.h"
#include "xMenu.h"
#include "xFields.h"
#include "xTime.h"
#include "MonoCanvas.h"

using namespace idisplay;

uint32_t g_tick = 0;
uint32_t g_slept = 0;
std::vector<uint32_t> g_delays;
int g_threadsCreated = 0;
uint32_t g_lastStackSize = 0;
bool g_failQueueNew = false;
bool g_failTimerNew = false;

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

// ---- a character display in RAM: what a screen drew, cell by cell ----
class FakeText : public iTextSurface {
public:
    FakeText(uint8_t cols, uint8_t rows) : iTextSurface(cols, rows), cells(cols * rows, ' '), inv(cols * rows, false) {}
    void clear() override {
        std::fill(cells.begin(), cells.end(), ' ');
        std::fill(inv.begin(), inv.end(), false);
        homeCursor();
    }
    std::string row(uint8_t r) const {
        std::string s(cells.begin() + r * cols(), cells.begin() + (r + 1) * cols());
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }
    bool inverseAt(uint8_t c, uint8_t r) const { return inv[r * cols() + c]; }
    void showEditCursor(uint8_t c, uint8_t r) override { editCol = c; editRow = r; }
    int editCol = -1, editRow = -1;
protected:
    void drawCell(uint8_t c, uint8_t r, char ch, bool inverse) override {
        cells[r * cols() + c] = ch;
        inv[r * cols() + c] = inverse;
    }
private:
    std::vector<char> cells;
    std::vector<bool> inv;
};

// ---- a display device around it; each flush takes a few main() calls ----
class FakeDisplay : public iDisplayDevice {
public:
    FakeDisplay(uint8_t cols, uint8_t rows) : surface(cols, rows) {}
    iTextSurface& text() override { return surface; }
    void requestFlush() override { wanted = true; }
    void main(uint32_t) override {
        ++mainCalls;
        if (broken) { isFailed = true; wanted = false; return; }
        isFailed = false;
        if (wanted && ++steps >= 3) { wanted = false; steps = 0; ++flushes; }
    }
    bool idle() const override { return !wanted; }
    bool failed() const override { return isFailed; }
    FakeText surface;
    int flushes = 0, mainCalls = 0, steps = 0;
    bool wanted = false, broken = false, isFailed = false;
};

// ---- screens ----
struct CountingScreen : public xScreen {
    const char* name;
    int enters = 0, resumes = 0, keys = 0, refreshes = 0;
    uint32_t lastArg = 0;
    xKey lastKey = xKey::Back;
    xScreen* onEnterOpens = nullptr;    // push this from onKey(Enter)
    explicit CountingScreen(const char* n) : name(n) {}
    void onEnter(xNavigator&) override { ++enters; }
    void onResume(xNavigator&) override { ++resumes; }
    void onKey(xKey k, xKeyAction a, xNavigator& nav) override {
        ++keys; lastKey = k;
        if (k == xKey::Enter && a == xKeyAction::Pressed && onEnterOpens) nav.push(*onEnterOpens);
    }
    void onRefresh(uint32_t arg) override { ++refreshes; lastArg = arg; }
    void render(iTextSurface& s) override { s.print(name); s.print(" "); s.print((int32_t)refreshes); }
};

static void textSurface() {
    std::printf("iTextSurface and MonoCanvas\n");
    FakeText t(10, 3);
    t.print((int32_t)-42); t.putChar(' '); t.print((int32_t)INT32_MIN);
    check(t.row(0) == "-42 -21474", "integers, INT32_MIN without overflow, clipped at the row's end");
    t.clear();
    t.print(21.46f, 1); t.putChar(' '); t.print(-0.04f, 1); t.putChar(' '); t.print(NAN, 2);
    check(t.row(0) == "21.5 0.0 -", "21.46 -> 21.5, -0.04 -> 0.0 (no -0.0), NAN -> -- (clipped)");
    t.clear();
    t.print(-3.14159f, 3); t.print("\nnext");
    check(t.row(0) == "-3.142" && t.row(1) == "next", "-3.142, and \\n starts the next row");
    t.clear();
    t.print(7.0f, 0); t.putChar(' '); t.print(0.5f, 2);
    check(t.row(0) == "7 0.50", "no decimals, and padded decimals");

    uint8_t buf[128 * 8];
    MonoCanvas c(buf, 128, 64);
    c.clear();
    c.drawPixel(5, 9, true);
    check(buf[128 + 5] == 0x02 && c.getPixel(5, 9), "pixel 5,9 is page 1, column 5, bit 1");
    c.drawPixel(-1, 0, true); c.drawPixel(128, 0, true); c.drawPixel(0, 64, true);
    check(!c.getPixel(-1, 0) && buf[0] == 0, "off-canvas pixels are clipped");
    c.fillRect(0, 0, 3, 8, true);
    check(buf[0] == 0xFF && buf[2] == 0xFF && buf[3] == 0, "fillRect");
    c.clear();
    c.drawChar(0, 0, 'A', true);
    check(buf[0] == 0x81 && buf[5] == 0xFF, "inverse char: glyph dark, its blank column and row lit");
    check(c.graphics() == &c && t.graphics() == nullptr, "graphics() is the canvas, nullptr on a character display");
    check(c.cols() == 21 && c.rows() == 8, "128x64 is 21 x 8 cells");
}

static void stack() {
    std::printf("xGuiCore: the screen stack\n");
    FakeText t(16, 4);
    CountingScreen home("home"), a("a"), b("b");
    xGuiCore gui(t, home);
    gui.start();
    check(&gui.top() == &home && home.enters == 1 && gui.depth() == 1, "start opens home");
    gui.handle(xGuiEvent::push(a));
    gui.handle(xGuiEvent::push(b));
    check(&gui.top() == &b && gui.depth() == 3 && a.enters == 1 && b.enters == 1, "push, push");
    gui.handle(xGuiEvent::pop());
    check(&gui.top() == &a && a.resumes == 1 && a.enters == 1, "pop resumes a, no new onEnter");
    gui.handle(xGuiEvent::show(b));
    check(&gui.top() == &b && gui.depth() == 2, "show replaces the top");
    gui.handle(xGuiEvent::home());
    check(&gui.top() == &home && gui.depth() == 1 && home.enters == 2, "home empties the stack");
    gui.handle(xGuiEvent::pop());
    check(&gui.top() == &home && gui.depth() == 1, "pop on home does nothing");
    gui.handle(xGuiEvent::show(a));
    check(&gui.top() == &a && gui.depth() == 2, "show from home keeps home underneath");
    for (int i = 0; i < 10; ++i) gui.push(b);
    check(gui.depth() == xGuiCore::kMaxDepth && &gui.top() == &b, "push onto a full stack replaces the top");
    gui.home();
    check(gui.handle(xGuiEvent::refresh(7)) && home.refreshes == 1 && home.lastArg == 7, "refresh reaches the top screen");
    check(gui.handle(xGuiEvent::keyEvent(xKey::Down, xKeyAction::Pressed)) && home.keys == 1 && home.lastKey == xKey::Down, "keys reach the top screen");
    gui.render();
    check(t.row(0) == "home 1", "render clears and draws the top screen");
}

static int g_actionCalls = 0;
static void* g_actionCtx = nullptr;
static void action(xNavigator&, void* ctx) { ++g_actionCalls; g_actionCtx = ctx; }

static void menu() {
    std::printf("xMenu\n");
    FakeText t(12, 4);
    CountingScreen home("home"), stations("stations");
    xMenuScreen main("Main Menu");
    int ctx = 3;
    main.menu().add("Network", action, &ctx);
    main.menu().add("Stations", stations);
    main.menu().add("Sensors", action);
    main.menu().add("Interfaces", action);
    main.menu().add("Settings", action);
    main.menu().addBack();
    xGuiCore gui(t, home);
    gui.start();
    gui.push(main);
    gui.render();
    check(t.row(0) == "Main Menu" && t.inverseAt(11, 0), "title, inverse to the end of the row");
    check(t.row(1) == ">Network" && t.row(2) == " Stations" && t.row(3) == " Sensors", "first three items, the first selected");
    check(t.inverseAt(0, 1) && t.inverseAt(11, 1) && !t.inverseAt(0, 2), "selected row drawn inverse across");
    for (int i = 0; i < 4; ++i) gui.handle(xGuiEvent::keyEvent(xKey::Down, xKeyAction::Pressed));
    gui.render();
    check(t.row(1) == " Sensors" && t.row(2) == " Interfaces" && t.row(3) == ">Settings", "scrolls to keep the selection on the last row");
    gui.handle(xGuiEvent::keyEvent(xKey::Down, xKeyAction::Pressed));
    gui.handle(xGuiEvent::keyEvent(xKey::Down, xKeyAction::Pressed));
    gui.render();
    check(main.menu().selected() == 5 && t.row(3) == ">Back", "stops at the last item");
    for (int i = 0; i < 4; ++i) gui.handle(xGuiEvent::keyEvent(xKey::Up, xKeyAction::Pressed));
    gui.render();
    check(t.row(1) == ">Stations" && t.row(2) == " Sensors", "scrolls back up");
    gui.handle(xGuiEvent::keyEvent(xKey::Up, xKeyAction::Released));
    check(main.menu().selected() == 1, "releases are ignored");
    gui.handle(xGuiEvent::keyEvent(xKey::Enter, xKeyAction::Pressed));
    check(&gui.top() == &stations && gui.depth() == 3, "Enter on a screen item pushes it");
    gui.handle(xGuiEvent::pop());
    check(&gui.top() == &main && main.menu().selected() == 1, "popped back: selection kept");
    gui.handle(xGuiEvent::keyEvent(xKey::Up, xKeyAction::Pressed));
    gui.handle(xGuiEvent::keyEvent(xKey::Enter, xKeyAction::Pressed));
    check(g_actionCalls == 1 && g_actionCtx == &ctx && &gui.top() == &main, "Enter on an action item runs it with its ctx");
    main.menu().select(5);
    gui.handle(xGuiEvent::keyEvent(xKey::Enter, xKeyAction::Pressed));
    check(&gui.top() == &home, "Enter on Back pops");
    gui.push(main);
    check(main.menu().selected() == 0, "opened afresh: selection back at the top");
    gui.handle(xGuiEvent::keyEvent(xKey::Back, xKeyAction::Pressed));
    check(&gui.top() == &home, "Back key pops");
    xMenu full;
    for (int i = 0; i < xMenu::kMaxItems; ++i) full.addBack();
    check(!full.addBack() && full.count() == xMenu::kMaxItems, "no more than kMaxItems");
}

static void fields() {
    std::printf("fields\n");
    const auto press = xKeyAction::Pressed;
    xYesNoField yn;
    yn.set(true);
    check(yn.onKey(xKey::Down, press) == xFieldResult::Changed && !yn.value(), "yes/no: Down flips");
    check(yn.onKey(xKey::Up, press) == xFieldResult::Changed && yn.value(), "yes/no: Up flips");
    check(yn.onKey(xKey::Enter, press) == xFieldResult::Done && yn.onKey(xKey::Back, press) == xFieldResult::Cancelled, "Enter done, Back cancelled");
    FakeText t(8, 2);
    yn.render(t);
    check(t.row(0) == "Yes", "draws Yes");

    xChoiceField ch;
    ch.add("Station 1"); ch.add("Station 2"); ch.add("Station 3");
    ch.onKey(xKey::Up, press);
    check(ch.selected() == 0, "choice: Up at the first stays");
    ch.onKey(xKey::Down, press); ch.onKey(xKey::Down, press); ch.onKey(xKey::Down, press);
    check(ch.selected() == 2, "choice: Down stops at the last");
    t.clear(); ch.render(t);
    check(t.row(0) == "Station", "draws the selected text (clipped)");

    // Keep an existing value by pressing Enter along it.
    char name[8] = "AB";
    xTextField f(name, sizeof name, xTextField::kCharsetUpper);
    f.begin();
    t.clear(); f.render(t);
    check(t.row(0) == "AB" && t.inverseAt(0, 0) && !t.inverseAt(1, 0), "text: shows the value, cursor on the first character");
    check(f.onKey(xKey::Enter, press) == xFieldResult::Changed && f.cursor() == 1, "Enter keeps A and moves on");
    f.onKey(xKey::Enter, press);
    t.clear(); f.render(t);
    check(t.row(0) == "AB>" && t.inverseAt(2, 0), "at the end the cursor shows end");
    check(f.onKey(xKey::Enter, press) == xFieldResult::Done && std::strcmp(name, "AB") == 0, "Enter on end: done, AB kept");

    // Change the first character, delete, truncate.
    std::strcpy(name, "CAT");
    f.begin();
    f.onKey(xKey::Down, press);     // C -> D
    f.onKey(xKey::Enter, press);    // DAT, cursor on A
    check(std::strcmp(name, "DAT") == 0, "Down steps C to D");
    // Up from A (the first of the set) wraps to end, then delete, then
    // the last character of the set; Down comes back to delete.
    f.onKey(xKey::Up, press); f.onKey(xKey::Up, press); f.onKey(xKey::Up, press);
    f.onKey(xKey::Down, press);
    t.clear(); f.render(t);
    check(t.row(0) == "D<T", "Up wraps past A to the commands; delete drawn <");
    check(f.onKey(xKey::Enter, press) == xFieldResult::Changed && std::strcmp(name, "AT") == 0 && f.cursor() == 0,
        "Enter on delete removes the character before the cursor");
    f.onKey(xKey::Up, press);       // A -> end
    check(f.onKey(xKey::Enter, press) == xFieldResult::Done && std::strcmp(name, "") == 0, "end at the start truncates to nothing");

    // Capacity: 7 characters in an 8 byte buffer.
    std::memset(name, 0, sizeof name);
    f.begin();
    for (int i = 0; i < 7; ++i) { f.onKey(xKey::Down, press); f.onKey(xKey::Down, press); f.onKey(xKey::Enter, press); }
    check(f.length() == 7 && name[7] == '\0', "seven characters fill the 8 byte buffer, NUL kept");
    f.onKey(xKey::Down, press);
    t.clear(); f.render(t);
    check(t.row(0) == "BBBBBBB<", "full: Down offers delete, not a character");
    f.onKey(xKey::Down, press);
    check(f.onKey(xKey::Enter, press) == xFieldResult::Done && std::strlen(name) == 7, "then end, which finishes");

    // A character outside the set is kept unless stepped away from.
    std::strcpy(name, "a!");
    f.begin();
    t.clear(); f.render(t);
    check(t.row(0) == "a!", "characters outside the set are shown as they are");
    f.onKey(xKey::Enter, press);
    f.onKey(xKey::Enter, press);
    f.onKey(xKey::Enter, press);
    check(std::strcmp(name, "a!") == 0, "and kept");

    // Scrolls sideways to keep the cursor in view.
    xTextFieldN<32> longer;
    std::strcpy(longer.buffer(), "WUNDERGROUNDSTATION");
    longer.begin();
    for (int i = 0; i < 12; ++i) longer.onKey(xKey::Enter, press);
    FakeText narrow(8, 1);
    narrow.setCursor(2, 0);
    longer.render(narrow);
    check(narrow.editCol == 7 && narrow.editRow == 0, "and marks that cell for a display without inverse");
    check(narrow.row(0) == "  ROUNDS" && narrow.inverseAt(7, 0) && !narrow.inverseAt(6, 0), "scrolled: the cursor on the last column");
}

static void buttons() {
    std::printf("xButton debouncing\n");
    xGuiEvent ev;
    xButton b(xKey::Enter, 50);
    check(b.onLevel(true, 1000, ev) && ev.type == xGuiEventType::Key && ev.key == xKey::Enter && ev.action == xKeyAction::Pressed,
        "first press reported");
    check(!b.onLevel(false, 1002, ev) && !b.onLevel(true, 1004, ev), "bounces of the press are not presses");
    check(!b.onLevel(true, 1005, ev), "same level again: nothing");
    check(!b.onLevel(false, 1200, ev), "release not reported by default");
    check(!b.onLevel(true, 1210, ev) && !b.onLevel(false, 1212, ev), "bounce of the release is not a press");
    check(b.onLevel(true, 1300, ev), "up 88 ms: the next press is taken");
    b.onLevel(false, 1310, ev);                     // a 10 ms tap: no release
    check(b.onLevel(true, 1400, ev), "after a tap shorter than the debounce, the next press still counts");

    xButton r(xKey::Up, 50, true);
    r.onLevel(true, 0, ev);
    check(!r.onLevel(false, 10, ev), "reportReleases: a release inside the debounce is bounce");
    r.onLevel(true, 12, ev);
    check(r.onLevel(false, 300, ev) && ev.action == xKeyAction::Released, "the real release is reported");

    xButton roll(xKey::Down, 50);
    roll.onLevel(true, 0xFFFFFFF0u, ev);
    roll.onLevel(false, 0xFFFFFFFAu, ev);
    check(!roll.onLevel(true, 0x00000010u, ev), "across the tick rollover: 22 ms up is still bounce");
    roll.onLevel(false, 0x00000011u, ev);
    check(roll.onLevel(true, 0x00000050u, ev), "and 63 ms is a press");
}

static bool tick(xButton& b, uint32_t now, xKeyAction expect) {
    xGuiEvent ev;
    return b.onTick(now, ev) && ev.action == expect && ev.key == b.key();
}

static void longPress() {
    std::printf("xButton long press and auto-repeat\n");
    xGuiEvent ev;

    xButton plain(xKey::Up);
    plain.onLevel(true, 0, ev);
    check(!plain.onTick(5000, ev), "plain button: the clock never reports anything");

    xButton enter(xKey::Enter, xButtonConfig::longPress(600));
    check(!enter.onLevel(true, 1000, ev), "long press button: nothing as it goes down");
    check(!enter.onTick(1300, ev), "nothing while it is down less than 600 ms");
    check(enter.onLevel(false, 1200, ev) && ev.action == xKeyAction::Pressed, "let go at 200 ms: a short press (Pressed)");
    check(!enter.onTick(1700, ev), "and no Held after it");

    enter.onLevel(true, 2000, ev);
    check(!enter.onTick(2599, ev), "down 599 ms: nothing");
    check(tick(enter, 2600, xKeyAction::Held), "down 600 ms: Held");
    check(!enter.onTick(2620, ev) && !enter.onTick(5000, ev), "Held only once per push");
    check(!enter.onLevel(false, 5100, ev), "let go after Held: nothing (a long press is not also a short one)");
    check(!enter.onLevel(true, 5110, ev) && !enter.onLevel(false, 5112, ev), "release bounce is no press");

    enter.onLevel(true, 6000, ev);
    enter.onLevel(false, 6010, ev);                 // bounce inside the debounce, pin stays up
    check(!enter.onTick(6700, ev), "pin up after a bounce: no Held from a press that ended");

    xButton both(xKey::Enter, xButtonConfig::longPress(600));
    both.onLevel(true, 0, ev);
    both.onLevel(false, 300, ev);
    check(!both.onTick(900, ev), "a short press already reported is never also Held");

    xButtonConfig rel = xButtonConfig::longPress(600);
    rel.reportReleases = true;
    xButton relB(xKey::Enter, rel);
    relB.onLevel(true, 0, ev);
    relB.onTick(600, ev);
    check(relB.onLevel(false, 800, ev) && ev.action == xKeyAction::Released, "reportReleases: Released after Held");
    relB.onLevel(true, 1000, ev);
    check(relB.onLevel(false, 1100, ev) && ev.action == xKeyAction::Pressed, "but a short press is just Pressed");

    xButton down(xKey::Down, xButtonConfig::autoRepeat(500, 150));
    check(down.onLevel(true, 0, ev) && ev.action == xKeyAction::Pressed, "auto-repeat: Pressed as it goes down");
    check(!down.onTick(499, ev), "nothing before 500 ms");
    check(tick(down, 500, xKeyAction::Repeat), "Repeat at 500 ms");
    check(!down.onTick(640, ev) && tick(down, 650, xKeyAction::Repeat) && tick(down, 800, xKeyAction::Repeat), "then every 150 ms");
    check(tick(down, 2000, xKeyAction::Repeat) && !down.onTick(2100, ev) && tick(down, 2150, xKeyAction::Repeat),
        "a late tick gives one Repeat, not a burst");
    check(!down.onLevel(false, 2200, ev) && !down.onTick(3000, ev), "let go: no more");
    check(down.onLevel(true, 3100, ev) && !down.onTick(3500, ev) && tick(down, 3600, xKeyAction::Repeat), "the next push starts over");

    xButton roll(xKey::Enter, xButtonConfig::longPress(600));
    roll.onLevel(true, 0xFFFFFF00u, ev);
    check(!roll.onTick(0x00000010u, ev) && tick(roll, 0x00000158u, xKeyAction::Held), "across the tick rollover: Held at 600 ms");
}

static void widgetsHeld() {
    std::printf("menus and fields with Held and Repeat\n");
    FakeText t(12, 4);
    CountingScreen home("home"), sub("sub");
    xMenuScreen m;
    m.menu().add("One", sub); m.menu().add("Two", sub); m.menu().add("Three", sub);
    xGuiCore gui(t, home);
    gui.start();
    gui.push(m);
    gui.handle(xGuiEvent::keyEvent(xKey::Down, xKeyAction::Repeat));
    gui.handle(xGuiEvent::keyEvent(xKey::Down, xKeyAction::Repeat));
    check(m.menu().selected() == 2, "menu: Repeat moves like a press");
    gui.handle(xGuiEvent::keyEvent(xKey::Enter, xKeyAction::Repeat));
    gui.handle(xGuiEvent::keyEvent(xKey::Enter, xKeyAction::Released));
    check(&gui.top() == &m, "Enter Repeat and Released do nothing");
    gui.handle(xGuiEvent::keyEvent(xKey::Enter, xKeyAction::Held));
    check(&gui.top() == &home, "holding Enter goes back");

    xYesNoField yn;
    check(yn.onKey(xKey::Up, xKeyAction::Repeat) == xFieldResult::Changed && yn.value(), "yes/no: Repeat flips");
    check(yn.onKey(xKey::Enter, xKeyAction::Held) == xFieldResult::Cancelled, "yes/no: holding Enter cancels");
    xChoiceField ch;
    ch.add("a"); ch.add("b"); ch.add("c");
    ch.onKey(xKey::Down, xKeyAction::Repeat); ch.onKey(xKey::Down, xKeyAction::Repeat);
    check(ch.selected() == 2 && ch.onKey(xKey::Enter, xKeyAction::Held) == xFieldResult::Cancelled, "choice: Repeat steps, holding Enter cancels");
    check(ch.onKey(xKey::Up, xKeyAction::Released) == xFieldResult::None && ch.selected() == 2, "choice: Released is ignored");
    char buf[8] = "";
    xTextField f(buf, sizeof buf, xTextField::kCharsetUpper);
    f.begin();
    for (int i = 0; i < 3; ++i) f.onKey(xKey::Down, xKeyAction::Repeat);   // end -> A -> B -> C
    f.onKey(xKey::Enter, xKeyAction::Pressed);
    check(std::strcmp(buf, "C") == 0, "text: Repeat runs through the set");
    check(f.onKey(xKey::Enter, xKeyAction::Repeat) == xFieldResult::None && std::strcmp(buf, "C") == 0, "text: Enter Repeat takes nothing");
    check(f.onKey(xKey::Enter, xKeyAction::Held) == xFieldResult::Cancelled, "text: holding Enter cancels");
}

static void task() {
    std::printf("xGui and xGuiButton (CMSIS-RTOS2)\n");
    FakeDisplay display(16, 4);
    CountingScreen home("home"), menuScreen("menu");
    home.onEnterOpens = &menuScreen;
    xGui gui(display, home);
    xGuiButton enter(gui, xKey::Enter), down(gui, xKey::Down);

    g_tick = 1000;
    check(!enter.onEdge(true), "a press before start() has nowhere to go");
    enter.onEdge(false);
    g_tick = 2000;

    check(gui.start("gui", 2048) && g_threadsCreated == 1 && g_lastStackSize == 2048, "start creates the task");
    check(gui.queue()->depth == 1 && gui.queue()->size == sizeof(xGuiEvent), "and a one-slot queue of xGuiEvent");
    check(gui.start() && g_threadsCreated == 1, "start twice: one task");

    gui.begin();
    check(home.enters == 1 && display.flushes == 1 && display.surface.row(0) == "home 0", "begin opens home and draws it");

    check(enter.onEdge(true), "button press posted");
    enter.onEdge(false);
    g_tick += 100;
    check(!down.onEdge(true), "second press while the first waits: queue full, dropped");
    down.onEdge(false);
    check(!gui.post(xGuiEvent::refresh(), 0), "a refresh is dropped too while a key waits");

    check(gui.runOnce(osWaitForever) && &gui.core().top() == &menuScreen && display.flushes == 2, "the task takes the key: home pushes the menu, redraw");
    check(display.surface.row(0) == "menu 0", "the menu is drawn");
    check(!gui.runOnce(0), "nothing queued: runOnce returns false");

    check(gui.post(xGuiEvent::refresh(9)), "refresh from another thread");
    gui.runOnce(osWaitForever);
    check(menuScreen.refreshes == 1 && menuScreen.lastArg == 9 && display.surface.row(0) == "menu 1" && display.flushes == 3,
        "refresh goes to the top screen and redraws");

    gui.post(xGuiEvent::home());
    gui.runOnce(osWaitForever);
    check(&gui.core().top() == &home && display.flushes == 4, "home from another thread");

    g_tick += 100;
    check(down.onEdge(true), "queue drained: presses get through again");
    down.onEdge(false);
    display.broken = true;
    const int calls = display.mainCalls;
    gui.runOnce(osWaitForever);
    check(home.lastKey == xKey::Down && display.mainCalls == calls + 1, "a failed display: one main() and the GUI carries on");
    display.broken = false;
    gui.post(xGuiEvent::refresh());
    gui.runOnce(osWaitForever);
    check(display.flushes == 5 && display.idle(), "and draws again once it is back");

    g_failQueueNew = true;
    xGui broken(display, home);
    check(!broken.start() && !broken.post(xGuiEvent::refresh()) && !broken.runOnce(0), "no queue: start fails, post and runOnce refuse");
    g_failQueueNew = false;

    // Long press and auto-repeat through the group's timer.
    {
        g_tick = 50000;
        xGui g(display, home);
        g.start();
        g.begin();
        xGuiButton enterB(g, xKey::Enter, xButtonConfig::longPress(600));
        xGuiButton downB(g, xKey::Down, xButtonConfig::autoRepeat(500, 150));
        xGuiButtonGroup group(20);
        check(group.add(enterB) && group.add(downB), "group: add buttons");
        check(group.start() && group.timer()->type == osTimerPeriodic && group.timer()->period == 20 && group.timer()->running,
            "group: a periodic 20 ms timer, running");
        enterB.onEdge(true);
        for (int i = 0; i < 29; ++i) { g_tick += 20; stubTimerFire(group.timer()); }
        check(osMessageQueueGetCount(g.queue()) == 0, "580 ms: nothing yet");
        g_tick += 20; stubTimerFire(group.timer());
        check(osMessageQueueGetCount(g.queue()) == 1, "600 ms: the timer posts Held");
        const int keys = home.keys;
        g.runOnce(osWaitForever);
        check(home.keys == keys + 1 && home.lastKey == xKey::Enter, "the GUI task takes it to the screen");
        enterB.onEdge(false);
        check(osMessageQueueGetCount(g.queue()) == 0, "let go after Held: nothing posted");

        g_tick += 100;
        downB.onEdge(true);
        g.runOnce(osWaitForever);                   // the Pressed
        for (int i = 0; i < 25; ++i) { g_tick += 20; stubTimerFire(group.timer()); }
        check(osMessageQueueGetCount(g.queue()) == 1, "auto-repeat: Repeat posted, the next ones dropped while it waits");
        g.runOnce(osWaitForever);
        for (int i = 0; i < 8; ++i) { g_tick += 20; stubTimerFire(group.timer()); }
        check(osMessageQueueGetCount(g.queue()) == 1, "taken: the next Repeat gets in");
        g.runOnce(osWaitForever);
        downB.onEdge(false);

        xGuiButtonGroup full;
        for (int i = 0; i < xGuiButtonGroup::kMaxButtons; ++i) full.add(enterB);
        check(!full.add(enterB), "group: no more than kMaxButtons");
        g_failTimerNew = true;
        xGuiButtonGroup noTimer;
        check(!noTimer.start(), "group: start fails without a timer");
        g_failTimerNew = false;
        noTimer.tick();                             // empty group: nothing to do
    }

    xGui deep(display, home, 4);
    deep.start();
    for (int i = 0; i < 4; ++i) deep.post(xGuiEvent::refresh());
    check(!deep.post(xGuiEvent::refresh()) && deep.queue()->depth == 4, "a deeper queue can be asked for");
}

static void timeOfDay() {
    std::printf("time of day\n");
    FakeText t(16, 2);
    xTimeHHMM hm(11, 0);
    check(!hm.valid() && hm.width() == 5 && xTimeHHMM::kWidth == 5, "HH:MM: 5 wide, not set yet");
    hm.update(t);
    check(t.row(0) == "           --:--", "dashes until a time is set");
    check(hm.set(9, 5), "set(9, 5)");
    hm.update(t);
    check(t.row(0) == "           09:05", "update() writes 09:05 at its place, zero padded");
    check(t.cursorCol() == 16 && t.cursorRow() == 0, "the cursor is left after it");
    check(!hm.set(24, 0) && !hm.set(12, 60) && hm.hours() == 9 && hm.minutes() == 5, "24:00 and 12:60 refused, the time kept");
    check(hm.set(23, 59), "23:59 taken");
    hm.update(t);
    check(t.row(0) == "           23:59", "23:59");
    hm.setFromSecondsOfDay(86400u * 3 + 13 * 3600 + 7 * 60 + 42);
    hm.update(t);
    check(t.row(0) == "           13:07" && hm.hours() == 13 && hm.minutes() == 7, "from seconds of the day, whole days dropped");
    hm.setFromSecondsOfDay(0xFFFFFFFFu);        // 4294967295 % 86400 = 23295 s = 06:28:15
    hm.update(t);
    check(t.row(0) == "           06:28", "the largest count (a uint32 of Unix seconds) wraps right");
    hm.invalidate();
    hm.update(t);
    check(t.row(0) == "           --:--", "invalidate(): dashes again");

    hm.set(7, 30);
    hm.setSeparator(0);
    check(hm.width() == 4, "no separator: 4 wide");
    t.clear();
    hm.update(t);
    check(t.row(0) == "           0730", "HHMM");
    hm.setSeparator(' ');
    hm.update(t);
    check(t.row(0) == "           07 30", "a blank separator (the blink's off half)");
    hm.setSeparator(':');

    // render() writes at the cursor, like the fields.
    t.clear();
    t.printAt(0, 1, "Now ");
    hm.render(t);
    check(t.row(1) == "Now 07:30", "render() writes at the cursor");
    hm.setPosition(0, 1);
    check(hm.col() == 0 && hm.row() == 1, "setPosition()");

    // Clipped at the edge, never wrapped onto the next row.
    FakeText narrow(4, 2);
    xTimeHHMM edge(2, 0);
    edge.set(12, 34);
    edge.update(narrow);
    check(narrow.row(0) == "  12" && narrow.row(1) == "", "clipped at the right edge");

    xTimeHHMMSS hms(4, 1);
    check(hms.width() == 8 && xTimeHHMMSS::kWidth == 8, "HH:MM:SS: 8 wide");
    t.clear();
    hms.update(t);
    check(t.row(1) == "    --:--:--", "dashes until set");
    check(hms.set(1, 2, 3), "set(1, 2, 3)");
    hms.update(t);
    check(t.row(1) == "    01:02:03" && hms.seconds() == 3, "01:02:03 at its place");
    check(!hms.set(1, 2, 60) && !hms.set(1, 60, 0) && !hms.set(24, 0, 0) && hms.seconds() == 3, "out of range refused");
    check(hms.set(23, 59, 59), "23:59:59 taken");
    hms.setFromSecondsOfDay(45296);             // 12:34:56
    hms.update(t);
    check(t.row(1) == "    12:34:56", "from seconds of the day");
    check(hms.set(8, 15) && hms.seconds() == 0, "set(h, m) on HH:MM:SS: seconds 0");
    hms.setSeparator('.');
    hms.update(t);
    check(t.row(1) == "    08.15.00", "another separator");
    hms.setSeparator(0);
    check(hms.width() == 6, "HHMMSS: 6 wide");

    // On a screen, through the GUI task: a data thread sets the time and
    // posts a refresh; the screen's render() calls update().
    struct ClockScreen : public xScreen {
        xTimeHHMM clock{11, 0};
        void render(iTextSurface& s) override { s.print("Outside 72"); clock.update(s); }
    } screen;
    FakeDisplay display(16, 2);
    xGui gui(display, screen);
    gui.start();
    gui.begin();
    check(display.surface.row(0) == "Outside 72 --:--", "home screen before the time is known");
    screen.clock.setFromSecondsOfDay(18 * 3600 + 45 * 60);
    check(gui.post(xGuiEvent::refresh()), "data thread: refresh posted");
    gui.runOnce(osWaitForever);
    check(display.surface.row(0) == "Outside 72 18:45", "and the GUI task drew 18:45");
}

int main() {
    textSurface();
    stack();
    menu();
    fields();
    buttons();
    longPress();
    widgetsHeld();
    task();
    timeOfDay();               // last: its xGui::start() counts a task
    std::printf("%s (%d failed)\n", g_failures ? "FAILED" : "all passed", g_failures);
    return g_failures ? 1 : 0;
}
