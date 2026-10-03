// Android specific startup: storage paths and working directory.

#include <cstdlib>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <android/log.h>

#include "SDL2/SDL.h"
#include "SDL2/SDL_system.h"

#include <filesystem>

#include "librecomp/game.hpp"
#include "android_support.h"

namespace {
    // Sends stdout and stderr to logcat so the game's printf output can be read with `adb logcat -s MegaMan64`.
    int sLogPipe[2];

    void* log_thread(void*) {
        char buffer[1024];
        ssize_t size;
        while ((size = read(sLogPipe[0], buffer, sizeof(buffer) - 1)) > 0) {
            if (buffer[size - 1] == '\n') {
                size--;
            }
            buffer[size] = '\0';
            __android_log_write(ANDROID_LOG_INFO, "MegaMan64", buffer);
        }
        return nullptr;
    }

    void redirect_output_to_logcat() {
        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        pipe(sLogPipe);
        dup2(sLogPipe[1], STDOUT_FILENO);
        dup2(sLogPipe[1], STDERR_FILENO);
        pthread_t thread;
        pthread_create(&thread, nullptr, log_thread, nullptr);
        pthread_detach(thread);
    }
}

void android_prepare_environment() {
    redirect_output_to_logcat();

    // The Java activity extracts the assets folder into internal storage before starting the game.
    const char* internal_path = SDL_AndroidGetInternalStoragePath();
    if (internal_path != nullptr) {
        chdir(internal_path);
        // Libraries that keep files under the home folder (RT64's shader cache) need one they can write to.
        setenv("HOME", internal_path, 1);
    }

    // Config, saves, mods and the stored ROM live in external storage so they can be pushed with adb:
    // /sdcard/Android/data/<package>/files
    const char* external_path = SDL_AndroidGetExternalStoragePath();
    if (external_path != nullptr) {
        setenv("APP_FOLDER_PATH", external_path, 1);
    }
    printf("Android paths: internal %s external %s\n", internal_path ? internal_path : "(null)", external_path ? external_path : "(null)");

    // Development aid: an env.txt in the app folder sets environment variables (one NAME=VALUE per line), since an app
    // can't be started with them on Android (for example MM64_AUTOLOAD=1 or RT64_PRINT_FRAME_TIME=2).
    if (external_path != nullptr) {
        FILE* file = fopen((std::string(external_path) + "/env.txt").c_str(), "r");
        if (file != nullptr) {
            char line[512];
            while (fgets(line, sizeof(line), file) != nullptr) {
                std::string text(line);
                while (!text.empty() && ((text.back() == '\n') || (text.back() == '\r') || (text.back() == ' '))) {
                    text.pop_back();
                }

                const size_t equals = text.find('=');
                if ((equals == std::string::npos) || (equals == 0) || (text[0] == '#')) {
                    continue;
                }

                setenv(text.substr(0, equals).c_str(), text.substr(equals + 1).c_str(), 1);
                printf("env.txt: %s\n", text.c_str());
            }

            fclose(file);
        }
    }
}

void android_import_rom(const std::u8string& game_id_in) {
    std::u8string game_id = game_id_in;
    if (recomp::is_rom_valid(game_id)) {
        return;
    }

    const char* external_path = SDL_AndroidGetExternalStoragePath();
    if (external_path == nullptr) {
        return;
    }

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(external_path, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::string extension = entry.path().extension().string();
        for (char& c : extension) {
            c = (char)tolower(c);
        }
        if (extension != ".z64" && extension != ".n64" && extension != ".v64") {
            continue;
        }
        // The game stores its validated copy of the ROM with this same folder, skip it.
        if (entry.path().filename().u8string() == game_id + u8".z64") {
            continue;
        }

        recomp::RomValidationError result = recomp::select_rom(entry.path(), game_id);
        printf("ROM import of %s: %d\n", entry.path().string().c_str(), (int)result);
        if (result == recomp::RomValidationError::Good) {
            std::filesystem::remove(entry.path(), ec);
            return;
        }
    }
}
