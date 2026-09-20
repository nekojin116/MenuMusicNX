#include "pm.hpp"

#include "config/config.hpp"
#include "sdmc/sdmc.hpp"

#include <algorithm>
#include <cstdio>

namespace {

constexpr u64 QLAUNCH_TITLE_ID{pm::QlaunchTitleId};
constexpr s32 EVENT_COUNT = 32;

/* qlaunch and every applet (library applets, the overlay applet, "starter",
   ...) live in the reserved 0x0100000000001xxx title-id range. */
constexpr u64 APPLET_TITLE_ID_BASE{0x0100000000001000ULL};
constexpr u64 APPLET_TITLE_ID_MASK{0xFFFFFFFFFFFFF000ULL};

/* The overlay applet is used by Tesla/nx-ovlloader. Opening the MenuMusicNX
   overlay must not be treated as leaving the HOME Menu. */
constexpr u64 OVERLAY_APPLET_TITLE_ID{0x010000000000100CULL};

/* Terminated applets whose older events must be skipped while scanning. */
constexpr u32 CLOSED_PROGRAM_MAX = 4;

constexpr u32 FOCUS_LOG_MAX_LINES = 12;
constexpr const char FOCUS_LOG_PATH[] = "/config/sys-tune/focus.log";

alignas(0x10) PdmAppletEvent g_applet_events[EVENT_COUNT]{};
alignas(0x10) PdmPlayEvent g_play_events[EVENT_COUNT]{};
s32 g_last_play_event_total{-1};
bool g_cached_home_foreground{};
bool g_cached_home_valid{};
bool g_pdmqry_initialized{};
bool g_pmdmnt_initialized{};
bool g_pminfo_initialized{};
u64 g_cached_app_tid{};
pm::FocusOwner g_focus_owner{pm::FocusOwner::Unknown};
pm::FocusOwner g_cached_focus_owner{pm::FocusOwner::Unknown};
bool g_pause_on_applet{true};
bool g_focus_log{false};
bool g_focus_log_dir_ready{false};
s32 g_last_logged_entry_index{-1};

auto IsSystemAppletTitleId(u64 program_id) -> bool {
    return (program_id & APPLET_TITLE_ID_MASK) == APPLET_TITLE_ID_BASE;
}

enum class FocusTarget : u8 {
    Ignore,
    Home,
    Application,
    Applet,
};

enum class FocusEventKind : u8 {
    Ignore,
    InFocus,
    OutOfFocus,
    Terminated,
};

auto ClassifyFocusEventKind(u8 event_type) -> FocusEventKind {
    switch (event_type) {
        case PdmAppletEventType_InFocus:
            return FocusEventKind::InFocus;
        case PdmAppletEventType_OutOfFocus:
            return FocusEventKind::OutOfFocus;
        /* The process is gone, so its older events no longer describe the
           foreground owner. */
        case PdmAppletEventType_Exit:
        case PdmAppletEventType_OutOfFocus4:
        case PdmAppletEventType_Exit5:
        case PdmAppletEventType_Exit6:
            return FocusEventKind::Terminated;
        default: /* PdmAppletEventType_Launch */
            return FocusEventKind::Ignore;
    }
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

auto ClassifyFocusTarget(u64 program_id, u8 applet_id, u64 app_tid) -> FocusTarget {
    if (applet_id != AppletId_None) {
        /* The play event log carries the applet id, which is exact. */
        if (applet_id == AppletId_SystemAppletMenu) {
            return FocusTarget::Home;
        }
        if (applet_id == AppletId_OverlayApplet) {
            return FocusTarget::Ignore;
        }
        if (applet_id == AppletId_application) {
            return TitleIdsMatch(program_id, app_tid) ? FocusTarget::Application : FocusTarget::Applet;
        }
        return FocusTarget::Applet;
    }

    /* The applet event log only reports the program id. */
    if (program_id == 0) {
        return FocusTarget::Ignore;
    }
    if (program_id == QLAUNCH_TITLE_ID) {
        return FocusTarget::Home;
    }
    if (program_id == OVERLAY_APPLET_TITLE_ID) {
        return FocusTarget::Ignore;
    }
    if (!IsSystemAppletTitleId(program_id) && TitleIdsMatch(program_id, app_tid)) {
        return FocusTarget::Application;
    }

    return FocusTarget::Applet;
}

struct FocusEventView {
    u32 entry_index;
    u64 program_id;
    u8 applet_id;
    u8 event_type;
};

FocusEventView g_focus_view[EVENT_COUNT]{};

struct FocusQuery {
    bool decided{false};
    bool home{false};

    /* Diagnostics for the decisive event. */
    u64 program_id{};
    u8 applet_id{};
    u8 event_type{};
    u32 entry_index{};
    FocusTarget target{FocusTarget::Ignore};
};

struct FocusScan {
    u64 closed[CLOSED_PROGRAM_MAX]{};
    u32 closed_count{};
};

auto IsClosedProgram(const FocusScan& scan, u64 program_id) -> bool {
    for (u32 i = 0; i < scan.closed_count; ++i) {
        if (scan.closed[i] == program_id) {
            return true;
        }
    }

    return false;
}

void MarkProgramClosed(FocusScan* scan, u64 program_id) {
    if (scan->closed_count >= CLOSED_PROGRAM_MAX || IsClosedProgram(*scan, program_id)) {
        return;
    }

    scan->closed[scan->closed_count++] = program_id;
}

/* Applies a single focus event, newest first. Returns true when the event
   decides whether the HOME Menu owns the foreground. */
auto ApplyFocusEvent(FocusQuery* query, FocusScan* scan, const FocusEventView& event, u64 app_tid) -> bool {
    const auto kind = ClassifyFocusEventKind(event.event_type);
    if (kind == FocusEventKind::Ignore || IsClosedProgram(*scan, event.program_id)) {
        return false;
    }

    const auto target = ClassifyFocusTarget(event.program_id, event.applet_id, app_tid);
    if (target == FocusTarget::Ignore) {
        return false;
    }

    query->program_id = event.program_id;
    query->applet_id = event.applet_id;
    query->event_type = event.event_type;
    query->entry_index = event.entry_index;
    query->target = target;

    switch (target) {
        case FocusTarget::Home:
            /* qlaunch only owns the foreground while it has focus. */
            query->home = kind == FocusEventKind::InFocus;
            query->decided = true;
            return true;

        case FocusTarget::Application:
            /* A game/homebrew that loses focus or terminates hands the
               foreground back to the HOME Menu. */
            query->home = kind != FocusEventKind::InFocus;
            query->decided = true;
            return true;

        case FocusTarget::Applet:
            if (kind == FocusEventKind::InFocus) {
                if (!g_pause_on_applet) {
                    return false;
                }
                query->home = false;
                query->decided = true;
                return true;
            }
            /* An applet that terminated is gone: keep scanning for the applet
               underneath it (or for the HOME Menu). */
            if (kind == FocusEventKind::Terminated) {
                MarkProgramClosed(scan, event.program_id);
            }
            return false;

        case FocusTarget::Ignore:
            break;
    }

    return false;
}

auto ResolveFocusQuery(u64 app_tid, s32 count, FocusQuery* query) -> bool {
    FocusScan scan{};

    for (s32 i = count - 1; i >= 0; --i) {
        if (ApplyFocusEvent(query, &scan, g_focus_view[i], app_tid)) {
            return true;
        }
    }

    return false;
}

void FillFocusViewFromAppletEvents(s32 start_entry, s32 count) {
    for (s32 i = 0; i < count; ++i) {
        g_focus_view[i] = FocusEventView{
            static_cast<u32>(start_entry + i),
            g_applet_events[i].program_id,
            AppletId_None,
            g_applet_events[i].event_type,
        };
    }
}

void FillFocusViewFromPlayEvents(s32 start_entry, s32 count, s32* out_count) {
    s32 entries = 0;

    for (s32 i = 0; i < count; ++i) {
        if (g_play_events[i].play_event_type != PdmPlayEventType_Applet) {
            continue;
        }

        g_focus_view[entries] = FocusEventView{
            static_cast<u32>(start_entry + i),
            PdmProgramIdToU64(g_play_events[i].event_data.applet.program_id),
            g_play_events[i].event_data.applet.applet_id,
            g_play_events[i].event_data.applet.event_type,
        };
        ++entries;
    }

    *out_count = entries;
}

auto FocusOwnerFromTarget(FocusTarget target) -> pm::FocusOwner {
    switch (target) {
        case FocusTarget::Home:
            return pm::FocusOwner::Home;
        case FocusTarget::Application:
            return pm::FocusOwner::Application;
        case FocusTarget::Applet:
            return pm::FocusOwner::Applet;
        case FocusTarget::Ignore:
            break;
    }

    return pm::FocusOwner::Unknown;
}

auto FocusTargetName(FocusTarget target) -> const char* {
    switch (target) {
        case FocusTarget::Home:
            return "home";
        case FocusTarget::Application:
            return "app";
        case FocusTarget::Applet:
            return "applet";
        case FocusTarget::Ignore:
            break;
    }

    return "ignore";
}

/* Optional breadcrumb trail for diagnosing focus behaviour on-device.
   Enabled with "[config] focus_log=1"; nothing is written otherwise. */
void LogFocusEvents(s32 count, u64 app_tid, const FocusQuery& query, bool decided) {
    if (!g_focus_log || count <= 0) {
        return;
    }

    if (!g_focus_log_dir_ready) {
        sdmc::CreateFolder("/config");
        sdmc::CreateFolder("/config/sys-tune");
        g_focus_log_dir_ready = true;
    }

    char buffer[FOCUS_LOG_MAX_LINES * 104 + 168];
    int used = 0;

    const auto append = [&](const char* format, auto... args) {
        const auto room = static_cast<int>(sizeof(buffer)) - used;
        if (room <= 1) {
            return;
        }

        const auto written = std::snprintf(buffer + used, static_cast<size_t>(room), format, args...);
        if (written <= 0) {
            return;
        }

        used += std::min(written, room - 1);
    };

    /* Oldest first, so the log reads chronologically. */
    for (s32 i = 0; i < count && i < static_cast<s32>(FOCUS_LOG_MAX_LINES); ++i) {
        const FocusEventView& event = g_focus_view[count - 1 - i];
        if (static_cast<s32>(event.entry_index) <= g_last_logged_entry_index) {
            continue;
        }

        append("ev idx=%u pid=%016llX applet=%u type=%u kind=%u target=%s\n",
            event.entry_index,
            static_cast<unsigned long long>(event.program_id),
            static_cast<unsigned>(event.applet_id),
            static_cast<unsigned>(event.event_type),
            static_cast<unsigned>(ClassifyFocusEventKind(event.event_type)),
            FocusTargetName(ClassifyFocusTarget(event.program_id, event.applet_id, app_tid)));

        g_last_logged_entry_index = static_cast<s32>(event.entry_index);
    }

    append("decision decided=%d home=%d target=%s pid=%016llX applet=%u type=%u\n",
        decided ? 1 : 0,
        query.home ? 1 : 0,
        FocusTargetName(query.target),
        static_cast<unsigned long long>(query.program_id),
        static_cast<unsigned>(query.applet_id),
        static_cast<unsigned>(query.event_type));

    if (used > 0) {
        sdmc::AppendFile(FOCUS_LOG_PATH, buffer, static_cast<size_t>(used));
    }
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
        g_focus_owner = g_cached_focus_owner;
        return 0;
    }

    g_last_play_event_total = total_entries;

    /* The ini is re-read whenever the focus event log changed, so a setting
       toggled in the overlay applies the moment it matters. */
    g_pause_on_applet = config::get_pause_on_applet();
    g_focus_log = config::get_focus_log();

    s32 start_entry = end_entry_index - (EVENT_COUNT - 1);
    if (start_entry < 0) {
        start_entry = 0;
    }

    s32 out{};
    s32 count{};
    rc = pdmqryQueryAppletEvent(start_entry, hosversionAtLeast(10, 0, 0), g_applet_events, EVENT_COUNT, &out);
    if (R_SUCCEEDED(rc) && out > 0) {
        count = std::min(out, EVENT_COUNT);
        FillFocusViewFromAppletEvents(start_entry, count);
    }

    FocusQuery query{};
    bool decided = ResolveFocusQuery(app_tid, count, &query);

    if (!decided) {
        /* Event types vary with firmware, so fall back to the play event log
           before reporting "unknown". */
        out = 0;
        rc = pdmqryQueryPlayEvent(start_entry, g_play_events, EVENT_COUNT, &out);
        if (R_FAILED(rc)) {
            g_cached_home_valid = false;
            return rc;
        }

        count = std::min(out, EVENT_COUNT);
        FillFocusViewFromPlayEvents(start_entry, count, &count);
        decided = ResolveFocusQuery(app_tid, count, &query);
    }

    LogFocusEvents(count, app_tid, query, decided);

    if (!decided) {
        g_cached_home_valid = false;
        return 1;
    }

    *home_foreground = query.home;
    g_focus_owner = FocusOwnerFromTarget(query.target);
    g_cached_home_foreground = query.home;
    g_cached_focus_owner = g_focus_owner;
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
    g_cached_focus_owner = pm::FocusOwner::Unknown;
    g_focus_owner = pm::FocusOwner::Unknown;
    g_last_logged_entry_index = -1;

    g_pause_on_applet = config::get_pause_on_applet();
    g_focus_log = config::get_focus_log();

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

    const bool no_application = pid == 0 || app_tid == QLAUNCH_TITLE_ID;

    /* QueryHomeForegroundFromPdm returns 0 only when the focus events actually
       decided the answer; "no application runs" is the fallback for every
       other case (no pdm:qry, undecidable or stale log). */
    bool home = no_application;
    if (QueryHomeForegroundFromPdm(app_tid, &home) == 0) {
        return home;
    }

    return no_application;
}

auto GetFocusOwner() -> FocusOwner {
    return g_focus_owner;
}

} // namespace pm
