#pragma once

#include <tesla.hpp>

#include "elm_overlayframe.hpp"

class BrowserGui final : public tsl::Gui {
  private:
    SysTuneOverlayFrame* m_frame{nullptr};
    tsl::elm::List *m_list{nullptr};
    FsFileSystem m_fs{};
    bool m_fs_open{false};
    bool has_music{false};
    char cwd[FS_MAX_PATH]{};

  public:
    BrowserGui();
    ~BrowserGui();

    tsl::elm::Element *createUI() override;
    bool handleInput(u64 keysDown, u64, const HidTouchState&, HidAnalogStickState, HidAnalogStickState) override;

  private:
    void scanCwd();
    void upCwd();
    void addAllToPlaylist();
    void infoAlert(const std::string &title, const std::string &text);
};
