#pragma once
#include "imgui.h"
#include "imgui_internal.h"

namespace overlay {
// Controller / keyboard focus between the two panes of the settings window: the sidebar (the list of
// pages) and the page.
//
// They were child windows flattened into one navigation scope (ImGuiChildFlags_NavFlattened). Dear
// ImGui only moves sideways to an item that overlaps the focused one vertically, and counts an item
// that is off to the side as above or below. So Right on a sidebar row found the page only when a
// widget happened to sit at the row's height, A chose the page and left the focus on the row, and Up
// or Down in the page went to the nearest sidebar row instead of the next setting (reported
// 2026-11-17: "when i press a or right dpad to get into sub menu it doesn't work").
//
// Now each pane is a navigation scope of its own (the window does not flatten them; the page's cards
// stay flattened into the page), so a move stays in its pane, and the panes hand the focus over here:
//   sidebar row: A (Enter, Space) or Right opens the page with the focus on its first setting
//   page:        B, or Left with nothing left of the focused setting, goes back to the page's row
//   neither (the window itself has the focus, as after ImGui's own cancel): the page's row
//
// Use, each frame of the window:
//   begin(show);
//   sidebar child: for each row { before_row(current); item; if (after_row(clicked, right)) open it; }
//                  (always call after_row, also for a row that was clicked)
//   page child:    begin_page(left); the page's items
//   after the window: if (B pressed && !back()) close the window
class PaneFocus {
public:
    void focus_sidebar() { to_sidebar_ = true, to_page_ = false; }  // the row of the page that is shown
    void focus_page() { to_page_ = true, to_sidebar_ = false; }     // the first setting of the page
    bool in_page() const { return in_page_; }
    bool in_sidebar() const { return row_focused_; }

    // after the window's Begin. show: the focus ring is shown when the focus is handed over (a controller
    // or the keyboard moves through the menu; not while the mouse has it)
    void begin(bool show) {
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        show_ = show;
        // Left was pressed in the page a frame ago and the focus stayed where it was: nothing is left of it
        if (left_frame_ >= 0 && left_frame_ == g.FrameCount - 1 && in_page_ && g.NavId == left_from_) focus_sidebar();
        left_frame_ = -1;
        // the window itself has the focus, neither pane: a controller would move between the panes' frames
        if (show && !to_page_ && g.NavWindow == g.CurrentWindow) to_sidebar_ = true;
        was_in_page_ = in_page_;
        row_focused_ = false;
    }
    // before a sidebar row's item; current: the row of the page that is shown
    void before_row(bool current) {
        if (!to_sidebar_ || !current) return;
        ImGui::SetKeyboardFocusHere();
        if (show_) ImGui::SetNavCursorVisible(true);
        to_sidebar_ = false;
    }
    // after a sidebar row's item; right: Right was pressed this frame. True: open this row's page.
    bool after_row(bool clicked, bool right) {
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        const bool focused = ImGui::IsItemFocused();
        row_focused_ |= focused;
        const bool activated = clicked && g.NavActivateId == ImGui::GetItemID();  // A, Enter, Space: not a click
        if (!activated && !(focused && right && g.ActiveId == 0)) return false;
        focus_page();
        return true;
    }
    // in the page's child window, before its items; left: Left was pressed this frame
    void begin_page(bool left) {
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        in_page_ = !row_focused_ && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
        // the page was asked to take the focus a frame ago and a row still has it: nothing in this page
        // takes the focus, so the sidebar gets the navigation back
        if (asked_frame_ >= 0 && asked_frame_ == g.FrameCount - 1 && row_focused_) focus_sidebar();
        asked_frame_ = -1;
        if (to_page_) {
            ImGui::SetKeyboardFocusHere();  // the next item that takes the focus, also inside a card
            if (show_) ImGui::SetNavCursorVisible(true);
            to_page_ = false;
            asked_frame_ = g.FrameCount;
        } else if (in_page_ && left && g.ActiveId == 0 && g.NavId != 0) {
            left_from_ = g.NavId;
            left_frame_ = g.FrameCount;
        }
    }
    // B: true when it was used here (the page gave the focus back to the sidebar, or B ended the edit of
    // a slider or text field), false when the window should close
    bool back() {
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        if (g.ActiveIdPreviousFrame != 0 && g.ActiveId == 0) return true;  // ImGui cancelled the edit with it
        // as of the last frame: ImGui's own cancel has already moved the focus from the page to the window
        if (!was_in_page_) return false;
        focus_sidebar();
        return true;
    }

private:
    bool show_ = false;
    bool to_sidebar_ = false, to_page_ = false;
    bool in_page_ = false, was_in_page_ = false, row_focused_ = false;
    ImGuiID left_from_ = 0;
    int left_frame_ = -1, asked_frame_ = -1;
};
}  // namespace overlay
