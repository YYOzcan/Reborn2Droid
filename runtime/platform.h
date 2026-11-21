// What the reimplemented Java side needs from the host (SDL on PC and Android).
#pragma once
#include <string>
#include <functional>
#include <cstdint>

namespace plat {
extern int width, height;            // drawable size in pixels
extern float dpi;
extern std::string files_dir, cache_dir, data_dir, apk_path, package_name, language, device_id;
extern bool is_android;
extern int android_sdk;

void open_url(const std::string &url);
void vibrate(int ms);
void keep_screen_on(bool on);
void quit();
void set_orientation(int android_orientation);   // ActivityInfo.SCREEN_ORIENTATION_*
int requested_orientation();
bool is_portrait();

// audio: the host pulls interleaved stereo s16 frames from fill() on its audio thread
void audio_start(int rate, std::function<void(int16_t *frames, int count)> fill);
void audio_stop();
void audio_pause(bool paused);

// key/value store behind Unity's PlayerPrefs (persisted in files_dir)
struct Prefs {
    static int get_int(const std::string &k, int def);
    static float get_float(const std::string &k, float def);
    static std::string get_string(const std::string &k, const std::string &def);
    static bool has(const std::string &k);
    static void set_int(const std::string &k, int v);
    static void set_float(const std::string &k, float v);
    static void set_string(const std::string &k, const std::string &v);
    static void remove(const std::string &k);
    static void clear();
    static void sync();
};

// runs fn on the game (GL) thread before the next frame
void post(std::function<void()> fn);
}
