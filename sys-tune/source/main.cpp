#include "impl/music_player.hpp"
#include "sdmc/sdmc.hpp"
#include "pm/pm.hpp"
#include "impl/source.hpp"
#include "tune_service.hpp"
#include "tune_result.hpp"

extern "C" {
u32 __nx_applet_type     = AppletType_None;
u32 __nx_fs_num_sessions = 1;

// TODO(TJ): calculate minimum heap
// TODO(TJ): calculate reasonable amount of heap for playlist entries.
void __libnx_initheap(void) {
    static char inner_heap[1024 * 250];
    extern char *fake_heap_start;
    extern char *fake_heap_end;

    // Configure the newlib heap.
    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

void __appInit() {
    R_ABORT_UNLESS(smInitialize());
    R_ABORT_UNLESS(setsysInitialize());
    {
        SetSysFirmwareVersion version;
        R_ABORT_UNLESS(setsysGetFirmwareVersion(&version));
        hosversionSet(MAKEHOSVERSION(version.major, version.minor, version.micro));
        setsysExit();
    }

    R_ABORT_UNLESS(fsInitialize());
    R_ABORT_UNLESS(pm::Initialize());
    R_ABORT_UNLESS(sdmc::Open());
}

void __appExit(void) {
    sdmc::Close();
    pm::Exit();
    fsExit();
    smExit();
}

} // extern "C"

namespace {

    alignas(0x1000) u8 gpioThreadBuffer[0x1000];
    alignas(0x1000) u8 pmdmntThreadBuffer[0x4000];
    alignas(0x1000) u8 tuneThreadBuffer[0x6000];

}

int main(int, char *[]) {
    /* Claim the IPC name first. A duplicate launch can race boot; in that case
       exit quietly before opening audio or creating worker threads. */
    if (R_FAILED(tune::InitializeServer()))
        return 0;

    if (R_FAILED(tune::impl::Initialize())) {
        tune::ExitServer();
        return 0;
    }

    ::Thread gpioThread{};
    ::Thread pmdmtThread{};
    ::Thread tuneThread{};

    Result rc = threadCreate(&pmdmtThread, tune::impl::PmdmntThreadFunc, nullptr,
        pmdmntThreadBuffer, sizeof(pmdmntThreadBuffer), 0x20, -2);
    if (R_FAILED(rc)) {
        tune::ExitServer();
        return 0;
    }
    rc = threadCreate(&tuneThread, tune::impl::TuneThreadFunc, nullptr,
        tuneThreadBuffer, sizeof(tuneThreadBuffer), 0x20, -2);
    if (R_FAILED(rc)) {
        threadClose(&pmdmtThread);
        tune::ExitServer();
        return 0;
    }
    rc = threadStart(&pmdmtThread);
    if (R_FAILED(rc)) {
        threadClose(&pmdmtThread);
        threadClose(&tuneThread);
        tune::ExitServer();
        return 0;
    }
    rc = threadStart(&tuneThread);
    if (R_FAILED(rc)) {
        tune::impl::Exit();
        svcCancelSynchronization(pmdmtThread.handle);
        threadWaitForExit(&pmdmtThread);
        threadClose(&pmdmtThread);
        threadClose(&tuneThread);
        tune::ExitServer();
        return 0;
    }

    /* Headphone detection is optional. Some hardware/firmware combinations do
       not expose this GPIO pad; music playback must still remain available. */
    GpioPadSession headphone_detect_session{};
    const bool gpio_initialized = R_SUCCEEDED(gpioInitialize());
    const bool gpio_session_open = gpio_initialized &&
        R_SUCCEEDED(gpioOpenSession(&headphone_detect_session, GpioPadName(0x15)));

    bool gpio_thread_created = false;
    bool gpio_thread_started = false;
    if (gpio_session_open && R_SUCCEEDED(threadCreate(&gpioThread, tune::impl::GpioThreadFunc,
            &headphone_detect_session, gpioThreadBuffer, sizeof(gpioThreadBuffer), 0x20, -2))) {
        gpio_thread_created = true;
        gpio_thread_started = R_SUCCEEDED(threadStart(&gpioThread));
    }
    tune::LoopProcess();
    tune::ExitServer();

    tune::impl::Exit();
    if (gpio_thread_started)
        svcCancelSynchronization(gpioThread.handle);
    svcCancelSynchronization(pmdmtThread.handle);
    svcCancelSynchronization(tuneThread.handle);

    if (gpio_thread_started)
        threadWaitForExit(&gpioThread);
    threadWaitForExit(&pmdmtThread);
    threadWaitForExit(&tuneThread);

    if (gpio_thread_created)
        threadClose(&gpioThread);
    threadClose(&pmdmtThread);
    threadClose(&tuneThread);

    /* Close gpio session. */
    if (gpio_session_open)
        gpioPadClose(&headphone_detect_session);
    if (gpio_initialized)
        gpioExit();

    return 0;
}
