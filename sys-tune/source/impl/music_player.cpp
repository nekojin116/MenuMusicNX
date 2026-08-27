#include "music_player.hpp"

#include "../tune_result.hpp"
#include "../tune_service.hpp"
#include "sdmc/sdmc.hpp"
#include "pm/pm.hpp"
#include "config/config.hpp"
#include "source.hpp"
#include "resamplers/SDL_audioEX.h"

#include <atomic>
#include <cstring>
#include <nxExt.h>

namespace tune::impl {

    namespace {
        constexpr float VOLUME_MAX = 1.f;
        constexpr auto PLAYLIST_ENTRY_MAX = 300; // 75k
        constexpr auto PATH_SIZE_MAX = 256;

        struct PlaylistID {
            u32 id{UINT32_MAX};

            bool IsValid() const {
                return id != UINT32_MAX;
            }

            void Reset() {
                id = UINT32_MAX;
            }
        };

        class PlayList {
        public:
            void Init() {
                Clear();
                m_playlist.reserve(PLAYLIST_ENTRY_MAX);
                m_shuffle_playlist.reserve(PLAYLIST_ENTRY_MAX);
            }

            bool Add(const char* path, EnqueueType type) {
                u32 index;
                if (!FindNextFreeEntry(index)) {
                    return false;
                }

                if (!m_entries[index].Add(path)) {
                    return false;
                }

                if (type == EnqueueType::Front) {
                    m_playlist.emplace(m_playlist.cbegin(), index);
                } else {
                    m_playlist.emplace_back(index);
                }

                // add new entry id to shuffle_playlist_list
                const auto shuffle_playlist_size = m_shuffle_playlist.size() + 1;
                const auto shuffle_index = randomGet64() % shuffle_playlist_size;
                m_shuffle_playlist.emplace(m_shuffle_playlist.cbegin() + shuffle_index, index);

                return true;
            }

            bool Remove(u32 index, ShuffleMode shuffle) {
                const auto entry = Get(index, shuffle);
                R_UNLESS(entry.IsValid(), false);

                // remove entry.
                m_entries[entry.id].Remove();

                // remove from both playlists.
                if (shuffle == ShuffleMode::On) {
                    m_playlist.erase(m_playlist.begin() + GetIndexFromID(entry, ShuffleMode::Off));
                    m_shuffle_playlist.erase(m_shuffle_playlist.begin() + index);
                } else {
                    m_playlist.erase(m_playlist.begin() + index);
                    m_shuffle_playlist.erase(m_shuffle_playlist.begin() + GetIndexFromID(entry, ShuffleMode::On));
                }

                return true;
            }

            bool Swap(u32 src, u32 dst, ShuffleMode shuffle) {
                if (src >= Size() || dst >= Size()) {
                    return false;
                }

                if (shuffle == ShuffleMode::On) {
                    std::swap(m_shuffle_playlist[src], m_shuffle_playlist[dst]);
                } else {
                    std::swap(m_playlist[src], m_playlist[dst]);
                }

                return true;
            }

            void Shuffle() {
                const auto size = m_shuffle_playlist.size();
                if (!size) {
                    return;
                }

                for (auto& e : m_shuffle_playlist) {
                    const auto index = randomGet64() % size;
                    std::swap(e, m_shuffle_playlist[index]);
                }
            }

            const char* GetPath(u32 index, ShuffleMode shuffle) const {
                return GetPath(Get(index, shuffle));
            }

            const char* GetPath(const PlaylistID& entry) const {
                R_UNLESS(entry.IsValid(), nullptr);

                return m_entries[entry.id].GetPath();
            }

            void Clear() {
                for (u32 i = 0; i < m_entries.size(); i++) {
                    m_entries[i].Remove();
                }

                m_playlist.clear();
                m_shuffle_playlist.clear();
            }

            u32 Size() const {
                return m_playlist.size();
            }

            PlaylistID Get(u32 index, ShuffleMode shuffle) const {
                if (index >= Size()) {
                    return {};
                }

                if (shuffle == ShuffleMode::On) {
                    return m_shuffle_playlist[index];
                } else {
                    return m_playlist[index];
                }
            }

            u32 GetIndexFromID(const PlaylistID& entry, ShuffleMode shuffle) const {
                if (!entry.IsValid()) {
                    return 0;
                }

                std::span list{m_playlist};
                if (shuffle == ShuffleMode::On) {
                    list = m_shuffle_playlist;
                }

                for (u32 i = 0; i < list.size(); i++) {
                    if (list[i].id == entry.id) {
                        return i;
                    }
                }

                return 0;
            }

        private:
            bool FindNextFreeEntry(u32& index) const {
                for (u32 i = 0; i < m_entries.size(); i++) {
                    if (m_entries[i].IsEmpty()) {
                        index = i;
                        return true;
                    }
                }

                return false;
            }

        private:
            struct PlayListNameEntry {
            public:
                // in most cases, the path will not exceed 256 bytes,
                // so this is a reasonable max rather than 0x301.
                bool Add(const char* path) {
                    if (!IsEmpty()) {
                        return false;
                    }

                    if (std::strlen(path) >= sizeof(m_path)) {
                        return false;
                    }

                    std::strcpy(m_path, path);
                    return true;
                }

                bool Remove() {
                    m_path[0] = '\0';
                    return true;
                }

                bool IsEmpty() const {
                    return m_path[0] == '\0';
                }

                const char* GetPath() const {
                    return m_path;
                }

            private:
                char m_path[PATH_SIZE_MAX]{};
            };

        private:
            std::vector<PlaylistID> m_playlist{};
            std::vector<PlaylistID> m_shuffle_playlist{};
            std::array<PlayListNameEntry, PLAYLIST_ENTRY_MAX> m_entries{};
        };

        PlayList g_playlist;

        PlaylistID g_current;
        u32 g_queue_position;

        LockableMutex g_mutex;

        std::atomic<RepeatMode> g_repeat{RepeatMode::All};
        ShuffleMode g_shuffle = ShuffleMode::Off;
        std::atomic<PlayerStatus> g_status{PlayerStatus::FetchNext};
        Source *g_source = nullptr;

        float g_title_volume = 1.f;
        float g_default_title_volume = 1.f;
        bool g_use_title_volume = true;

        constexpr auto AUDIO_FREQ          = 48000;
        constexpr auto AUDIO_CHANNEL_COUNT = 2;
        constexpr auto AUDIO_BUFFER_COUNT  = 2;
        constexpr auto AUDIO_LATENCY_MS    = 42;
        constexpr auto AUDIO_BUFFER_SIZE   = AUDIO_FREQ / 1000 * AUDIO_LATENCY_MS * AUDIO_CHANNEL_COUNT;

        AudioOutBuffer g_audout_buffer[AUDIO_BUFFER_COUNT];
        alignas(0x1000) s16 AudioMemoryPool[AUDIO_BUFFER_COUNT][(AUDIO_BUFFER_SIZE + 0xFFF) & ~0xFFF];
        static_assert((sizeof(AudioMemoryPool[0]) % 0x2000) == 0, "Audio Memory pool needs to be page aligned!");

        std::atomic_bool g_home_foreground{false};
        std::atomic_bool g_headphone_pause{false};
        std::atomic_bool g_should_run{true};
        float g_fade_gain        = 0.f;

        constexpr float FADE_STEP = 0.06f; // ~500 ms at ~42 ms/buffer

        auto ApplyFadeGain(s16 *data, size_t byte_size, float gain) -> void {
            if (gain >= 0.999f) {
                return;
            }

            if (gain <= 0.f) {
                std::memset(data, 0, byte_size);
                return;
            }

            const size_t count = byte_size / sizeof(s16);
            for (size_t i = 0; i < count; i++) {
                data[i] = static_cast<s16>(data[i] * gain);
            }
        }

        auto UpdateFadeGain() -> void {
            const bool should_pause = !g_home_foreground.load() || g_headphone_pause.load();
            const float target = should_pause ? 0.f : 1.f;

            if (g_fade_gain < target) {
                g_fade_gain = std::min(target, g_fade_gain + FADE_STEP);
            } else if (g_fade_gain > target) {
                g_fade_gain = std::max(target, g_fade_gain - FADE_STEP);
            }
        }

        void FinishTrack() {
            const auto repeat = g_repeat.load();
            if (repeat == RepeatMode::One) {
                return;
            }
            if (repeat == RepeatMode::All) {
                Next();
                return;
            }

            std::scoped_lock lk(g_mutex);
            const auto size = g_playlist.Size();
            if (g_queue_position + 1 < size) {
                ++g_queue_position;
                g_status.store(PlayerStatus::FetchNext);
            } else {
                g_status.store(PlayerStatus::Stopped);
            }
        }

        Result PlayTrack(const char* path) {
            /* Open file and allocate */
            auto source = OpenFile(path);
            R_UNLESS(source != nullptr, tune::FileOpenFailure);
            R_UNLESS(source->IsOpen(), tune::FileOpenFailure);
            R_UNLESS(source->SetupResampler(audoutGetChannelCount(), audoutGetSampleRate()), tune::VoiceInitFailure);

            AudioOutState state;
            R_TRY(audoutGetAudioOutState(&state));
            if (state == AudioOutState_Stopped) {
                R_TRY(audoutStartAudioOut());
            }

            {
                std::scoped_lock lk(g_mutex);
                g_source = source.get();
            }
            struct RegisteredSourceGuard {
                Source *registered;
                ~RegisteredSourceGuard() {
                    std::scoped_lock lk(g_mutex);
                    if (g_source == registered) {
                        g_source = nullptr;
                    }
                }
            } source_guard{source.get()};
            g_fade_gain = 0.f;

            // for the first buffer, use very small buffer sizes to reduce latency between songs.
            int first = 1;

            while (g_should_run.load() && g_status.load() == PlayerStatus::Playing) {
                UpdateFadeGain();

                if (g_fade_gain <= 0.f && (!g_home_foreground.load() || g_headphone_pause.load())) {
                    svcSleepThread(17'000'000);
                    continue;
                }

                AudioOutBuffer* buffer = NULL;
                for (int i = 0; i < AUDIO_BUFFER_COUNT; i++) {
                    bool has_buffer = false;
                    R_TRY(audoutContainsAudioOutBuffer(&g_audout_buffer[i], &has_buffer));
                    if (!has_buffer) {
                        buffer = &g_audout_buffer[i];
                        break;
                    }
                }

                if (!buffer) {
                    u32 released_count;
                    const Result rc = audoutWaitPlayFinish(&buffer, &released_count, 100'000'000);
                    if (rc == KERNELRESULT(TimedOut)) {
                        continue;
                    }
                    R_TRY(rc);
                }

                bool error = false;
                if (buffer) {
                    auto buffer_size = AUDIO_BUFFER_SIZE * sizeof(s16);
                    if (first) {
                        first--;
                        buffer_size = std::min(512 * sizeof(s16), buffer_size);
                    }

                    const auto nSamples = source->Resample((u8*)buffer->buffer, buffer_size);
                    if (nSamples <= 0) {
                        error = true;
                    } else {
                        ApplyFadeGain(static_cast<s16 *>(buffer->buffer), nSamples, g_fade_gain);
                        buffer->data_size = nSamples;
                        R_TRY(audoutAppendAudioOutBuffer(buffer));
                    }
                }

                if (source->Done()) {
                    FinishTrack();
                    break;
                }
                if (error) {
                    return tune::DecodeFailure;
                }
            }

            return 0;
        }

    }

    Result Initialize() {
        for (int i = 0; i < AUDIO_BUFFER_COUNT; i++) {
            g_audout_buffer[i].buffer = AudioMemoryPool[i];
            g_audout_buffer[i].buffer_size = sizeof(AudioMemoryPool[i]);
        }

        R_TRY(audoutInitialize());
        R_TRY(audoutSetAudioOutVolume(std::clamp(config::get_volume(), 0.f, VOLUME_MAX)));

        /* Fetch values from config, sanitize the return value */
        if (auto c = config::get_repeat(); c <= 2 && c >= 0) {
            SetRepeatMode(static_cast<RepeatMode>(c));
        }

        SetShuffleMode(static_cast<ShuffleMode>(config::get_shuffle()));

        // reserves memory so that we don't allocate later on.
        g_playlist.Init();

        return 0;

    }

    void Exit() {
        g_should_run.store(false);
        g_status.store(PlayerStatus::FetchNext);
    }

    void TuneThreadFunc(void *) {
        {
            char load_path[PATH_SIZE_MAX];
            if (config::get_load_path(load_path, sizeof(load_path))) {
                // check if the path is a file or folder.
                FsDirEntryType type;
                if (R_SUCCEEDED(sdmc::GetType(load_path, &type))) {
                    if (type == FsDirEntryType_File) {
                        // path is a file, load single entry.
                        if (GetSourceType(load_path) != SourceType::NONE) {
                            Enqueue(load_path, std::strlen(load_path), EnqueueType::Back);
                        }
                    } else {
                        // path is a folder, load all entries.
                        FsDir dir;
                        if (R_SUCCEEDED(sdmc::OpenDir(&dir, load_path, FsDirOpenMode_ReadFiles|FsDirOpenMode_NoFileSize))) {
                            // during init, we have a lot of memory to work with.
                            std::vector<FsDirectoryEntry> entries(std::min(64, PLAYLIST_ENTRY_MAX));

                            s64 total;
                            char full_path[PATH_SIZE_MAX];
                            Result rc = 0;

                            while (R_SUCCEEDED(fsDirRead(&dir, &total, entries.size(), entries.data())) && total) {
                                for (s64 i = 0; i < total; i++) {
                                    if (GetSourceType(entries[i].name) != SourceType::NONE) {
                                        const int written = std::snprintf(full_path, sizeof(full_path), "%s/%s", load_path, entries[i].name);
                                        if (written < 0 || static_cast<size_t>(written) >= sizeof(full_path)) {
                                            continue;
                                        }
                                        rc = Enqueue(full_path, std::strlen(full_path), EnqueueType::Back);
                                        if (rc == tune::OutOfMemory) {
                                            break;
                                        }
                                    }
                                }

                                if (rc == tune::OutOfMemory) {
                                    break;
                                }
                            }

                            fsDirClose(&dir);
                        }
                    }
                }
            }
        }

        /* Run as long as we aren't stopped and no error has been encountered. */
        while (g_should_run.load()) {
            if (g_status.load() == PlayerStatus::Stopped) {
                svcSleepThread(100'000'000);
                continue;
            }

            char current_path[PATH_SIZE_MAX]{};
            {
                std::scoped_lock lk(g_mutex);

                g_current.Reset();
                const auto queue_size = g_playlist.Size();
                if (queue_size != 0 && g_queue_position >= queue_size) {
                    g_queue_position = queue_size - 1;
                }
                if (queue_size != 0) {
                    g_current = g_playlist.Get(g_queue_position, g_shuffle);
                    const char *path = g_playlist.GetPath(g_current);
                    if (path != nullptr) {
                        std::snprintf(current_path, sizeof(current_path), "%s", path);
                        g_status.store(PlayerStatus::Playing);
                    }
                }
            }
            if (current_path[0] == '\0') {
                svcSleepThread(100'000'000);
                continue;
            }

            /* Only play if playing and we have a track queued. */
            Result rc = PlayTrack(current_path);

            /* Log error. */
            if (R_FAILED(rc)) {
                /* Invalid media should not be retried forever. Transient audio
                   service errors retain the track and retry after a delay. */
                if (rc == tune::FileOpenFailure || rc == tune::VoiceInitFailure ||
                    rc == tune::DecodeFailure) {
                    std::scoped_lock lk(g_mutex);
                    for (u32 i = 0; i < g_playlist.Size(); ++i) {
                        const char *queued_path = g_playlist.GetPath(i, g_shuffle);
                        if (queued_path != nullptr && std::strcmp(queued_path, current_path) == 0) {
                            g_playlist.Remove(i, g_shuffle);
                            if (g_queue_position > i)
                                --g_queue_position;
                            if (g_playlist.Size() == 0)
                                g_queue_position = 0;
                            else if (g_queue_position >= g_playlist.Size())
                                g_queue_position = g_playlist.Size() - 1;
                            break;
                        }
                    }
                    g_status.store(PlayerStatus::FetchNext);
                } else {
                    g_status.store(PlayerStatus::FetchNext);
                    svcSleepThread(500'000'000);
                }
            }
        }

        audoutStopAudioOut();
        audoutExit();
    }

    void GpioThreadFunc(void *ptr) {
        GpioPadSession *session = static_cast<GpioPadSession *>(ptr);

        /* [0] Low == plugged in; [1] High == not plugged in. */
        GpioValue old_value = GpioValue_High;

        // TODO(TJ): pausing on headphone change should be a config option.
        while (g_should_run.load()) {
            /* Fetch current gpio value. */
            GpioValue value;
            if (R_SUCCEEDED(gpioPadGetValue(session, &value))) {
                if (old_value == GpioValue_Low && value == GpioValue_High) {
                    g_headphone_pause.store(true);
                } else if (old_value == GpioValue_High && value == GpioValue_Low) {
                    g_headphone_pause.store(false);
                }
                old_value = value;
            }
            svcSleepThread(10'000'000);
        }
    }

    void PmdmntThreadFunc(void *) {
        while (g_should_run.load()) {
            g_home_foreground.store(pm::IsHomeMenuForeground());
            svcSleepThread(50'000'000);
        }
    }

    bool GetStatus() {
        return g_home_foreground.load() && !g_headphone_pause.load();
    }

    void Play() {
        // Playback is controlled automatically by HOME Menu foreground detection.
    }

    void Pause() {
        // Playback is controlled automatically by HOME Menu foreground detection.
    }

    void Next() {
        {
            std::scoped_lock lk(g_mutex);

            const auto size = g_playlist.Size();
            if (size == 0) {
                g_queue_position = 0;
                return;
            }
            if (g_queue_position < size - 1) {
                g_queue_position++;
            } else {
                g_queue_position = 0;
            }
        }
        g_status.store(PlayerStatus::FetchNext);
    }

    void Prev() {
        {
            std::scoped_lock lk(g_mutex);

            const auto size = g_playlist.Size();
            if (size == 0) {
                g_queue_position = 0;
                return;
            }
            if (g_queue_position > 0) {
                g_queue_position--;
            } else {
                g_queue_position = size - 1;
            }
        }
        g_status.store(PlayerStatus::FetchNext);
    }

    float GetVolume() {
        float volume = 1.F;
        audoutGetAudioOutVolume(&volume);
        return volume;
    }

    void SetVolume(float volume) {
        volume = std::clamp(volume, 0.f, VOLUME_MAX);
        audoutSetAudioOutVolume(volume);
        config::set_volume(volume);
    }

    float GetTitleVolume() {
        return g_title_volume;
    }

    void SetTitleVolume(float volume) {
        volume = std::clamp(volume, 0.f, VOLUME_MAX);
        g_title_volume = volume;
        g_use_title_volume = true;
    }

    float GetDefaultTitleVolume() {
        return g_default_title_volume;
    }

    void SetDefaultTitleVolume(float volume) {
        volume = std::clamp(volume, 0.f, VOLUME_MAX);
        g_default_title_volume = volume;
        config::set_default_title_volume(volume);
    }

    RepeatMode GetRepeatMode() {
        return g_repeat.load();
    }

    void SetRepeatMode(RepeatMode mode) {
        if (mode >= RepeatMode::Off && mode <= RepeatMode::All) {
            g_repeat.store(mode);
        }
    }

    ShuffleMode GetShuffleMode() {
        std::scoped_lock lk(g_mutex);
        return g_shuffle;
    }

    void SetShuffleMode(ShuffleMode mode) {
        if (mode != ShuffleMode::Off && mode != ShuffleMode::On) {
            return;
        }
        std::scoped_lock lk(g_mutex);

        // if we just enabled shuffle mode, re-shuffle the playlist.
        if (g_shuffle == ShuffleMode::Off && mode == ShuffleMode::On) {
            g_playlist.Shuffle();
        }

        g_shuffle = mode;
    }

    u32 GetPlaylistSize() {
        std::scoped_lock lk(g_mutex);

        return g_playlist.Size();
    }

    Result GetPlaylistItem(u32 index, char *buffer, size_t buffer_size) {
        R_UNLESS(buffer != nullptr && buffer_size > 0, tune::InvalidArgument);
        std::scoped_lock lk(g_mutex);

        const auto path = g_playlist.GetPath(index, g_shuffle);
        R_UNLESS(path, tune::OutOfRange);

        std::snprintf(buffer, buffer_size, "%s", path);

        return 0;
    }

    Result GetCurrentQueueItem(CurrentStats *out, char *buffer, size_t buffer_size) {
        R_UNLESS(out != nullptr && buffer != nullptr && buffer_size > 0, tune::InvalidArgument);

        std::scoped_lock lk(g_mutex);
        R_UNLESS(g_source != nullptr, tune::NotPlaying);
        R_UNLESS(g_source->IsOpen(), tune::NotPlaying);

        const auto path = g_playlist.GetPath(g_current);
        R_UNLESS(path, tune::NotPlaying);

        std::snprintf(buffer, buffer_size, "%s", path);

        auto [current, total] = g_source->Tell();
        int sample_rate       = g_source->GetSampleRate();

        out->sample_rate   = sample_rate;
        out->current_frame = current;
        out->total_frames  = total;

        return 0;
    }

    void ClearQueue() {
        {
            std::scoped_lock lk(g_mutex);

            g_playlist.Clear();
            g_queue_position = 0;
        }
        g_status.store(PlayerStatus::FetchNext);
    }

    // currently unused (and untested).
    void MoveQueueItem(u32 src, u32 dst) {
        std::scoped_lock lk(g_mutex);

        if (!g_playlist.Swap(src, dst, g_shuffle)) {
            return;
        }

        if (g_queue_position == src) {
            g_queue_position = dst;
        }
    }

    void Select(u32 index) {
        {
            std::scoped_lock lk(g_mutex);

            const auto size = g_playlist.Size();
            if (!size) {
                return;
            }

            g_queue_position = std::min(index, size - 1);
        }
        g_status.store(PlayerStatus::FetchNext);
    }

    void Seek(u32 position) {
        std::scoped_lock lk(g_mutex);
        if (g_source != nullptr && g_source->IsOpen())
            g_source->Seek(position);
    }

    Result Enqueue(const char *buffer, size_t buffer_length, EnqueueType type) {
        R_UNLESS(buffer != nullptr && buffer_length > 0 && buffer_length < PATH_SIZE_MAX, tune::InvalidArgument);
        R_UNLESS(type == EnqueueType::Front || type == EnqueueType::Back, tune::InvalidArgument);

        char path[PATH_SIZE_MAX]{};
        std::memcpy(path, buffer, buffer_length);
        path[buffer_length] = '\0';

        if (GetSourceType(path) == SourceType::NONE)
            return tune::InvalidPath;

        /* Ensure file exists. */
        if (!sdmc::FileExists(path))
            return tune::InvalidPath;

        std::scoped_lock lk(g_mutex);

        if (!g_playlist.Add(path, type)) {
            return tune::OutOfMemory;
        }

        if (g_status.load() == PlayerStatus::Stopped) {
            g_status.store(PlayerStatus::FetchNext);
        }

        // check if the current position still points to the same entry, update if not.
        if (g_current.IsValid() && g_current.id != g_playlist.Get(g_queue_position, g_shuffle).id) {
            g_queue_position = g_playlist.GetIndexFromID(g_current, g_shuffle);
            g_current = g_playlist.Get(g_queue_position, g_shuffle);
        }

        return 0;
    }

    Result Remove(u32 index) {
        std::scoped_lock lk(g_mutex);

        /* Ensure we don't operate out of bounds. */
        R_UNLESS(g_playlist.Size(), tune::QueueEmpty);

        if (!g_playlist.Remove(index, g_shuffle)) {
            return tune::OutOfRange;
        }

        /* Fetch a new track if we deleted the current song. */
        const bool fetch_new = g_queue_position == index;

        /* Lower current position if needed. */
        if (g_queue_position > index) {
            g_queue_position--;
        }

        if (fetch_new)
            g_status.store(PlayerStatus::FetchNext);

        return 0;
    }

}
