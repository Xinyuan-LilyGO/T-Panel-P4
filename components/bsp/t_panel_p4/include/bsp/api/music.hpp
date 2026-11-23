// Board class-body fragments for src/music.cpp.
// The public and private sections are selected by board.hpp.

#if defined(BSP_BOARD_API_PUBLIC)

        // 3.9 音乐播放与元数据 API：src/music.cpp；依赖音频和存储
        // ========================================================================
        [[nodiscard]] esp_err_t playMusicFile(const char *path);
        [[nodiscard]] esp_err_t playMusicFile(const char *path, const MusicPlaybackConfig &config);
        [[nodiscard]] esp_err_t probeMusicFile(const char *path, MusicProbe &probe) const;
        [[nodiscard]] esp_err_t pauseMusic(bool paused);
        [[nodiscard]] esp_err_t stopMusic();
        [[nodiscard]] esp_err_t stopMusic(std::uint32_t timeout_ms);
        [[nodiscard]] esp_err_t deinitializeMusic();
        [[nodiscard]] esp_err_t setMusicEventCallback(MusicEventCallback callback,
                                                      void *context = nullptr);
        [[nodiscard]] esp_err_t setMusicNextTrackCallback(MusicNextTrackCallback callback,
                                                          void *context = nullptr);
        [[nodiscard]] MusicState musicState() const noexcept;
        [[nodiscard]] MusicFormat musicFormat() const noexcept;
        [[nodiscard]] esp_err_t musicLastError() const noexcept;
        [[nodiscard]] const char *musicLastErrorName() const noexcept;
        [[nodiscard]] MusicStatistics musicStatistics() const noexcept;
        void resetMusicStatistics() noexcept;
        [[nodiscard]] bool isMusicPlaying() const noexcept;
        [[nodiscard]] bool isMusicPaused() const noexcept;
        [[nodiscard]] std::uint32_t musicPositionMs() const noexcept;
        [[nodiscard]] std::uint32_t musicDurationMs() const noexcept;
        [[nodiscard]] std::uint8_t musicLevelPercent() const noexcept;
        void getCurrentMusicPath(char *path, std::size_t size) const noexcept;
        void getMusicWaveform(std::int16_t *samples, std::size_t count) const noexcept;
        [[nodiscard]] esp_err_t seekMusicMs(std::uint32_t position_ms);
        [[nodiscard]] esp_err_t getMusicCover(std::vector<std::uint8_t> &cover) const;
        [[nodiscard]] esp_err_t getMusicLyrics(std::string &lyrics) const;
        [[nodiscard]] esp_err_t getMusicCover(const char *path,
                                              std::vector<std::uint8_t> &cover) const;
        [[nodiscard]] esp_err_t getMusicLyrics(const char *path, std::string &lyrics) const;
        [[nodiscard]] esp_err_t getMusicMetadata(const char *path, std::vector<std::uint8_t> &cover,
                                                 std::string &lyrics) const;

#elif defined(BSP_BOARD_API_PRIVATE)

        // 4.8 音乐运行时：src/music.cpp；MusicRuntime 隐藏播放器实现
        // ========================================================================
        [[nodiscard]] MusicRuntime *ensureMusicRuntime();
        MusicRuntime *music_runtime_{};

#endif
