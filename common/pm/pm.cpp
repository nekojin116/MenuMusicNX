#include "pm.hpp"

namespace {

constexpr u64 QLAUNCH_TITLE_ID{pm::QlaunchTitleId};
constexpr s32 EVENT_COUNT = 32;

alignas(0x10) PdmAppletEvent g_applet_events[EVENT_COUNT]{};
alignas(0x10) PdmPlayEvent g_play_events[EVENT_COUNT]{};
s32 g_last_play_event_total{-1};
bool g_cached_home_foreground{};
bool g_cached_home_valid{};
bool g_pdmqry_initialized{};
bool g_pmdmnt_initialized{};
bool g_pminfo_initialized{};
u64 g_cached_app_tid{};

auto IsFocusEvent(u8 event_type) -> bool {
    return event_type == PdmAppletEventType_InFocus ||
           event_type == PdmAppletEventType_OutOfFocus ||
           event_type == PdmAppletEventType_OutOfFocus4;
}

auto PdmProgramIdToU64(const u32 program_id[2]) -> u64 {
    return (static_cast<u64>(program_id[1]) << 32) | program_id[0];
}

auto TitleIdsMatch(u64 lhs, u64 rhs) -> bool {
    if (lhs == rhs) {
        return true;
    }

    constexpr u64 base_mask = ~0xFFULL;
    if ((lhs & base_mask) == (rhs & base_mask)) {
        return true;
    }

    constexpr u64 update_mask = ~0xFFFULL;
    return (lhs & update_mask) == (rhs & update_mask);
}

auto IsQlaunchProgramId(u64 program_id) -> bool {
    return program_id == 0 || program_id == QLAUNCH_TITLE_ID;
}

auto ApplicationFocusEventIsHome(u8 event_type) -> bool {
    return event_type == PdmAppletEventType_OutOfFocus ||
           event_type == PdmAppletEventType_OutOfFocus4;
}

auto ResolveHomeFromFocusEvent(u64 program_id, u8 event_type, u64 app_tid, bool* home_foreground) -> bool {
    if (IsQlaunchProgramId(program_id)) {
        *home_foreground = event_type == PdmAppletEventType_InFocus;
        return true;
    }

    if (TitleIdsMatch(program_id, app_tid)) {
        *home_foreground = ApplicationFocusEventIsHome(event_type);
        return true;
    }

    if (event_type == PdmAppletEventType_InFocus) {
        *home_foreground = false;
        return true;
    }

    return false;
}

auto QueryHomeFromAppletEvents(u64 app_tid, s32 out, bool* home_foreground) -> bool {
    for (s32 i = out - 1; i >= 0; --i) {
        const PdmAppletEvent* event = &g_applet_events[i];
        if (!IsFocusEvent(event->event_type)) {
            continue;
        }

        if (ResolveHomeFromFocusEvent(event->program_id, event->event_type, app_tid, home_foreground)) {
            return true;
        }
    }

    return false;
}

auto QueryHomeFromPlayEvents(u64 app_tid, s32 out, bool* home_foreground) -> bool {
    for (s32 i = out - 1; i >= 0; --i) {
        const PdmPlayEvent* event = &g_play_events[i];
        if (event->play_event_type != PdmPlayEventType_Applet) {
            continue;
        }

        const u8 event_type = event->event_data.applet.event_type;
        if (!IsFocusEvent(event_type)) {
            continue;
        }

        const u64 program_id = PdmProgramIdToU64(event->event_data.applet.program_id);
        const u8 applet_id = event->event_data.applet.applet_id;

        if (applet_id == AppletId_SystemAppletMenu) {
            if (ResolveHomeFromFocusEvent(QLAUNCH_TITLE_ID, event_type, app_tid, home_foreground)) {
                return true;
            }
        } else if (applet_id == AppletId_application) {
            if (ResolveHomeFromFocusEvent(program_id, event_type, app_tid, home_foreground)) {
                return true;
            }
        } else if (event_type == PdmAppletEventType_InFocus) {
            *home_foreground = false;
            return true;
        }
    }

    return false;
}

auto QueryHomeForegroundFromPdm(u64 app_tid, bool* home_foreground) -> Result {
    if (!g_pdmqry_initialized) {
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }

    s32 total_entries{};
    s32 start_entry_index{};
    s32 end_entry_index{};
    Result rc = pdmqryGetAvailablePlayEventRange(&total_entries, &start_entry_index, &end_entry_index);
    if (R_FAILED(rc)) {
        return rc;
    }

    if (total_entries == g_last_play_event_total && g_cached_home_valid && app_tid == g_cached_app_tid) {
        *home_foreground = g_cached_home_foreground;
        return 0;
    }

    g_last_play_event_total = total_entries;

    s32 start_entry = end_entry_index - (EVENT_COUNT - 1);
    if (start_entry < 0) {
        start_entry = 0;
    }

    s32 out{};
    const bool allow_unknown_policy = hosversionAtLeast(10, 0, 0);
    rc = pdmqryQueryAppletEvent(start_entry, allow_unknown_policy, g_applet_events, EVENT_COUNT, &out);
    if (R_SUCCEEDED(rc) && out > 0) {
        if (QueryHomeFromAppletEvents(app_tid, out, home_foreground)) {
            g_cached_home_foreground = *home_foreground;
            g_cached_home_valid = true;
            g_cached_app_tid = app_tid;
            return 0;
        }
    }

    out = 0;
    rc = pdmqryQueryPlayEvent(start_entry, g_play_events, EVENT_COUNT, &out);
    if (R_FAILED(rc)) {
        g_cached_home_valid = false;
        return rc;
    }

    if (out <= 0) {
        g_cached_home_valid = false;
        return 1;
    }

    if (!QueryHomeFromPlayEvents(app_tid, out, home_foreground)) {
        g_cached_home_valid = false;
        return 1;
    }

    g_cached_home_foreground = *home_foreground;
    g_cached_home_valid = true;
    g_cached_app_tid = app_tid;
    return 0;
}

} // namespace

namespace pm {

auto Initialize() -> Result {
    g_pmdmnt_initialized = R_SUCCEEDED(pmdmntInitialize());
    g_pminfo_initialized = R_SUCCEEDED(pminfoInitialize());
    g_pdmqry_initialized = R_SUCCEEDED(pdmqryInitialize());

    g_last_play_event_total = -1;
    g_cached_home_valid = false;
    g_cached_app_tid = 0;

    return 0;
}

void Exit() {
    if (g_pdmqry_initialized) {
        pdmqryExit();
        g_pdmqry_initialized = false;
    }
    if (g_pminfo_initialized) {
        pminfoExit();
        g_pminfo_initialized = false;
    }
    if (g_pmdmnt_initialized) {
        pmdmntExit();
        g_pmdmnt_initialized = false;
    }
}

void getCurrentPidTid(u64* pid_out, u64* tid_out) {
    if (pid_out == nullptr || tid_out == nullptr) {
        return;
    }

    *pid_out = UINT64_MAX;
    *tid_out = 0;
    if (!g_pmdmnt_initialized) {
        return;
    }

    Result rc{};
    if (R_SUCCEEDED(rc = pmdmntGetApplicationProcessId(pid_out))) {
        if (g_pminfo_initialized && 0x20f == pminfoGetProgramId(tid_out, *pid_out)) {
            *tid_out = QLAUNCH_TITLE_ID;
        }
    } else if (rc == 0x20f) {
        *pid_out = 0;
        *tid_out = QLAUNCH_TITLE_ID;
    }
}

auto IsHomeMenuForeground() -> bool {
    u64 pid{};
    u64 app_tid{};
    getCurrentPidTid(&pid, &app_tid);

    if (pid == 0 || app_tid == QLAUNCH_TITLE_ID) {
        return true;
    }

    bool home = false;
    if (R_SUCCEEDED(QueryHomeForegroundFromPdm(app_tid, &home))) {
        return home;
    }

    return false;
}

} // namespace pm
