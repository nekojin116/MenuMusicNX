#define TESLA_INIT_IMPL
#include "tune.h"
#include "gui_error.hpp"
#include "gui_main.hpp"
#include "sdmc/sdmc.hpp"
#include "config/config.hpp"

#include <tesla.hpp>

class SysTuneOverlay final : public tsl::Overlay {
  private:
    const char *msg = nullptr;
    Result fail     = 0;
    bool tune_ready = false;
    bool sdmc_ready = false;

  public:
    void initServices() override {
        Result rc = tuneInitialize();

        // not found can happen if the service isn't started
        // connection refused can happen is the service was terminated by pmshell
        if (R_VALUE(rc) == KERNELRESULT(NotFound) || R_VALUE(rc) == KERNELRESULT(ConnectionRefused)) {
            u64 pid = 0;
            const NcmProgramLocation programLocation{
                .program_id = 0x4200000000000000,
                .storageID  = NcmStorageId_None,
            };
            rc = pmshellInitialize();
            if (R_SUCCEEDED(rc)) {
                rc = pmshellLaunchProgram(0, &programLocation, &pid);
                pmshellExit();
            }
            if (R_FAILED(rc) || pid == 0) {
                this->fail = rc;
                this->msg  = "  Failed to\n"
                            "launch MenuMusicNX";
                return;
            }
            svcSleepThread(500'000'000ULL);
            rc = tuneInitialize();
        }

        if (R_FAILED(rc)) {
            this->msg  = "Something went wrong:";
            this->fail = rc;
            return;
        }
        this->tune_ready = true;

        if (R_FAILED(sdmc::Open())) {
            this->msg  = "Failed sdmc::Open()";
            return;
        }
        this->sdmc_ready = true;

        u32 api;
        if (R_FAILED(tuneGetApiVersion(&api)) || api != TUNE_API_VERSION) {
            this->msg = "   Unsupported\n"
                        "MenuMusicNX version!";
        }
    }

    void exitServices() override {
        if (this->sdmc_ready)
            sdmc::Close();
        if (this->tune_ready)
            tuneExit();
    }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        if (this->msg) {
            return std::make_unique<ErrorGui>(this->msg, this->fail);
        } else {
            return std::make_unique<MainGui>();
        }
    }
};

int main(int argc, char **argv) {
    return tsl::loop<SysTuneOverlay>(argc, argv);
}
