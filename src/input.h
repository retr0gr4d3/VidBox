#pragma once
#include <windows.h>

// Maps a Windows key message to the W3C KeyboardEvent.code name the KVM expects ("KeyA", "Enter", ...).
// Returns nullptr for keys that can't be forwarded.
const char* KeyCodeFromMessage(WPARAM vk, LPARAM lParam);
