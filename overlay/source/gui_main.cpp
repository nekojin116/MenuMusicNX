#include "gui_main.hpp"

#include "elm_overlayframe.hpp"
#include "elm_volume.hpp"
#include "gui_browser.hpp"
#include "gui_playlist.hpp"
#include "config/config.hpp"

namespace {
    constexpr const size_t num_steps = 20;
}

MainGui::MainGui() {
    m_status_bar    = new StatusBar();
}

tsl::elm::Element *MainGui::createUI() {
    auto frame = new SysTuneOverlayFrame();
    auto list  = new tsl::elm::List();

    /* Current track and transport controls (skip/shuffle/repeat only). */
    list->addItem(this->m_status_bar, tsl::style::ListItemDefaultHeight * 2);

    list->addItem(new tsl::elm::CategoryHeader("MenuMusicNX"));

    auto info = new tsl::elm::ListItem("Plays on HOME Menu only");
    info->setValue("Auto");
    list->addItem(info);

    /* Applets (Settings, Album, hbmenu, ...) pause playback like games do. */
    auto applet_pause = new tsl::elm::ListItem("Pause in applets");
    applet_pause->setValue(config::get_pause_on_applet() ? "On" : "Off");
    applet_pause->setClickListener([applet_pause](u64 keys) {
        if (keys & HidNpadButton_A) {
            const bool value = !config::get_pause_on_applet();
            config::set_pause_on_applet(value);
            applet_pause->setValue(value ? "On" : "Off");
            applet_pause->invalidate();
            return true;
        }
        return false;
    });
    list->addItem(applet_pause);

    /* Playlist. */
    auto queue_button = new tsl::elm::ListItem("Playlist");
    queue_button->setClickListener([](u64 keys) {
        if (keys & HidNpadButton_A) {
            tsl::changeTo<PlaylistGui>();
            return true;
        }
        return false;
    });
    list->addItem(queue_button);

    /* Browser. */
    auto browser_button = new tsl::elm::ListItem("Music browser");
    browser_button->setClickListener([](u64 keys) {
        if (keys & HidNpadButton_A) {
            tsl::changeTo<BrowserGui>();
            return true;
        }
        return false;
    });
    list->addItem(browser_button);

    list->addItem(new tsl::elm::CategoryHeader("Volume"));

    float tune_volume = 1.f;
    tuneGetVolume(&tune_volume);

    auto tune_volume_slider = new ElmVolume("\uE13C", "Music Volume", num_steps);
    tune_volume_slider->setProgress(tune_volume * num_steps);
    tune_volume_slider->setValueChangedListener([](u8 value){
        const float volume = float(value) / float(num_steps);
        tuneSetVolume(volume);
    });
    list->addItem(tune_volume_slider);

    list->addItem(new tsl::elm::CategoryHeader("Misc"));

    auto startup_button = new tsl::elm::ListItem("Remove start up file");
    startup_button->setClickListener([frame](u64 keys) {
        if (keys & HidNpadButton_A) {
            char path[512];
            if (config::get_load_path(path, sizeof(path))) {
                config::set_load_path("");
                const auto* p = path;
                if (auto ext = std::strrchr(path, '/')) {
                    p = ext + 1;
                }

                frame->setToast("Removed start up file", p);
            } else {
                frame->setToast("Failed to remove start up file", "No start up file set in config");
            }
            return true;
        }
        return false;
    });
    list->addItem(startup_button);

    auto exit_button = new tsl::elm::ListItem("Close MenuMusicNX");
    exit_button->setClickListener([](u64 keys) {
        if (keys & HidNpadButton_A) {
            tuneQuit();
            tsl::goBack();
            return true;
        }
        return false;
    });
    list->addItem(exit_button);

    frame->setContent(list);

    return frame;
}

void MainGui::update() {
    static u8 tick = 0;
    /* Update status 4 times per second. */
    if ((tick % 15) == 0)
        this->m_status_bar->update();
    tick++;
}
