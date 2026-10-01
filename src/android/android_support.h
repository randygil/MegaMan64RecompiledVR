#ifndef __ANDROID_SUPPORT_H__
#define __ANDROID_SUPPORT_H__

// Sets up the working directory, the app folder path and logcat output. Called at the start of main.
void android_prepare_environment();

#include <string>

// Imports the first ROM (.z64, .n64 or .v64) found in the app folder if the game doesn't have one yet, then deletes
// the copy so the folder doesn't keep two.
void android_import_rom(const std::u8string& game_id);

#endif
