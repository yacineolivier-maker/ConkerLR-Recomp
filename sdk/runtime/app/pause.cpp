// Escape: the game stops and asks before closing. Yes ends the process at once, no or Escape again gives the game
// back where it was. Keys, mouse and controllers (A yes, B no) answer.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <imgui.h>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "xdk/d3d8.h"

namespace app {

namespace {

constexpr UINT_PTR kTimer = 0x7E5C;  // while paused: the frame shown again, the controllers read
constexpr UINT kTick = 16;           // ms
enum Answer : int { kNone, kYes, kNo };

std::atomic<bool> g_paused{false};
std::mutex g_park;
std::condition_variable g_resume;
// the guest's clock: the host's less g_offset, held at g_frozen while paused; g_seq is odd while they change
std::atomic<uint32_t> g_seq{0};
std::atomic<int64_t> g_offset{0}, g_frozen{0};
std::atomic<int> g_choice{kNo};    // the highlighted answer
std::atomic<int> g_answer{kNone};  // given in the ImGui frame, taken on the window thread
std::atomic<bool> g_hold[4] = {};
input::Pad g_before[4];  // window thread: the controllers at the last tick, answers on a press

int64_t HostNs() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

bool Down(const input::Pad& p, int analog) { return p.analog[analog] > 0x80; }

void Pause(HWND hwnd) {
    g_seq++;
    g_frozen = HostNs() - g_offset;
    g_paused = true;
    g_seq++;
    g_choice = kNo;
    g_answer = kNone;
    for (int i = 0; i < 4; i++) g_before[i] = input::Read(i);
    ClipCursor(nullptr);  // the mouse answers too
    SetTimer(hwnd, kTimer, kTick, nullptr);
    LOGI("pause: Escape, asking before closing");
}

void Resume(HWND hwnd) {
    KillTimer(hwnd, kTimer);
    for (auto& h : g_hold) h = true;
    {
        std::lock_guard lock(g_park);
        g_seq++;
        g_offset = HostNs() - g_frozen;
        g_paused = false;
        g_seq++;
    }
    g_resume.notify_all();
    LOGI("pause: back to the game");
}

void Take(HWND hwnd, int answer) {
    if (answer == kYes) {
        KillTimer(hwnd, kTimer);
        LOGI("pause: closing the game");
        Quit();
    }
    if (answer == kNo) Resume(hwnd);
}

// a press on a controller: A yes, B no, the pad's left and right move the highlight
void ReadPads(HWND hwnd) {
    for (int i = 0; i < 4; i++) {
        input::Pad p = input::Read(i), was = g_before[i];
        g_before[i] = p;
        if (!p.connected) continue;
        if (Down(p, 0) && !Down(was, 0)) return Take(hwnd, kYes);
        if (Down(p, 1) && !Down(was, 1)) return Take(hwnd, kNo);
        if ((p.buttons & ~was.buttons) & 0x0C) g_choice = g_choice == kYes ? kNo : kYes;
    }
}

}  // namespace

bool Paused() { return g_paused; }

void PausePoint() {
    if (!g_paused) return;
    std::unique_lock lock(g_park);
    g_resume.wait(lock, [] { return !g_paused.load(); });
}

int64_t GameNs() {
    for (;;) {
        uint32_t s = g_seq.load();
        if (s & 1) {
            std::this_thread::yield();
            continue;
        }
        int64_t t = g_paused ? g_frozen.load() : HostNs() - g_offset;
        if (g_seq.load() == s) return t;
    }
}

bool PauseMessage(void* window, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    HWND hwnd = static_cast<HWND>(window);
    switch (msg) {
        case WM_KEYDOWN: {
            bool repeat = lp & (1 << 30);
            if (!g_paused) {
                if (wp != VK_ESCAPE || repeat) return false;
                Pause(hwnd);
                *result = 0;
                return true;
            }
            switch (wp) {
                case VK_ESCAPE:
                    if (!repeat) Take(hwnd, kNo);
                    break;
                case VK_LEFT:
                case VK_RIGHT:
                case VK_UP:
                case VK_DOWN:
                case VK_TAB: g_choice = g_choice == kYes ? kNo : kYes; break;
                case VK_RETURN:
                case VK_SPACE:
                    if (!repeat) Take(hwnd, g_choice);
                    break;
                case 'O': Take(hwnd, kYes); break;
                case 'N': Take(hwnd, kNo); break;
            }
            *result = 0;  // nothing else takes keys while the question is up
            return true;
        }
        case WM_SETCURSOR:
            if (!g_paused || LOWORD(lp) != HTCLIENT) return false;
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            *result = TRUE;
            return true;
        case WM_TIMER:
            if (wp != kTimer) return false;
            if (g_paused) Take(hwnd, g_answer.exchange(kNone));
            if (g_paused) ReadPads(hwnd);
            if (g_paused) d3d::Redisplay();
            *result = 0;
            return true;
    }
    return false;
}

void PauseUi() {
    if (!g_paused) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                                                  IM_COL32(0, 0, 0, 120));
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    // no ImGui navigation: the highlight is this file's, a controller's A must not press another button
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoNav;
    ImGui::SetNextWindowFocus();
    if (!ImGui::Begin("##quitter", nullptr, kFlags)) {
        ImGui::End();
        return;
    }
    ImGui::SetWindowFontScale(1.8f);
    ImGui::TextUnformatted("Vous allez fermer le jeu. Êtes-vous sûr ?");
    ImGui::Spacing();
    float w = ImGui::GetFontSize() * 5;
    float gap = ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetWindowSize().x - 2 * w - gap) / 2));
    for (int a : {int(kYes), int(kNo)}) {
        if (a == kNo) ImGui::SameLine();
        bool lit = g_choice == a;
        if (lit) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(a == kYes ? "OUI" : "NON", ImVec2(w, 0))) g_answer = a;
        if (lit) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) g_choice = a;
    }
    ImGui::End();
}

bool PauseHoldsPad(int port, const input::Pad& p) {
    if (port < 0 || port >= 4 || !g_hold[port]) return false;
    bool pressed = p.buttons != 0;
    for (uint8_t a : p.analog) pressed |= a > 0x20;
    if (!pressed) g_hold[port] = false;
    return pressed;
}

}  // namespace app
