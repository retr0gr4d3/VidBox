#include "input.h"

// Scancode set 1 -> KeyboardEvent.code. Index = scancode (0..0x7F); [1] = with E0 (extended) prefix.
static const char* kScan[0x80][2] = {};

static void Init() {
  static bool done = false;
  if (done) return;
  done = true;
  auto S = [](int sc, const char* n, const char* e = nullptr) { kScan[sc][0] = n; kScan[sc][1] = e; };
  S(0x01, "Escape");
  const char* digits[] = {"Digit1", "Digit2", "Digit3", "Digit4", "Digit5", "Digit6", "Digit7", "Digit8", "Digit9", "Digit0"};
  for (int i = 0; i < 10; ++i) S(0x02 + i, digits[i]);
  S(0x0C, "Minus"); S(0x0D, "Equal"); S(0x0E, "Backspace"); S(0x0F, "Tab");
  const char* row1[] = {"KeyQ", "KeyW", "KeyE", "KeyR", "KeyT", "KeyY", "KeyU", "KeyI", "KeyO", "KeyP"};
  for (int i = 0; i < 10; ++i) S(0x10 + i, row1[i]);
  S(0x1A, "BracketLeft"); S(0x1B, "BracketRight");
  S(0x1C, "Enter", "NumpadEnter");
  S(0x1D, "ControlLeft", "ControlRight");
  const char* row2[] = {"KeyA", "KeyS", "KeyD", "KeyF", "KeyG", "KeyH", "KeyJ", "KeyK", "KeyL"};
  for (int i = 0; i < 9; ++i) S(0x1E + i, row2[i]);
  S(0x27, "Semicolon"); S(0x28, "Quote"); S(0x29, "Backquote"); S(0x2A, "ShiftLeft"); S(0x2B, "Backslash");
  const char* row3[] = {"KeyZ", "KeyX", "KeyC", "KeyV", "KeyB", "KeyN", "KeyM"};
  for (int i = 0; i < 7; ++i) S(0x2C + i, row3[i]);
  S(0x33, "Comma"); S(0x34, "Period"); S(0x35, "Slash", "NumpadDivide"); S(0x36, "ShiftRight");
  S(0x37, "NumpadMultiply", "PrintScreen");
  S(0x38, "AltLeft", "AltRight");
  S(0x39, "Space"); S(0x3A, "CapsLock");
  const char* fk[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10"};
  for (int i = 0; i < 10; ++i) S(0x3B + i, fk[i]);
  S(0x45, "Pause", "NumLock");
  S(0x46, "ScrollLock");
  S(0x47, "Numpad7", "Home"); S(0x48, "Numpad8", "ArrowUp"); S(0x49, "Numpad9", "PageUp");
  S(0x4A, "NumpadSubtract");
  S(0x4B, "Numpad4", "ArrowLeft"); S(0x4C, "Numpad5"); S(0x4D, "Numpad6", "ArrowRight");
  S(0x4E, "NumpadAdd");
  S(0x4F, "Numpad1", "End"); S(0x50, "Numpad2", "ArrowDown"); S(0x51, "Numpad3", "PageDown");
  S(0x52, "Numpad0", "Insert"); S(0x53, "NumpadDecimal", "Delete");
  S(0x56, "IntlBackslash");
  S(0x57, "F11"); S(0x58, "F12");
  S(0x5B, nullptr, "MetaLeft"); S(0x5C, nullptr, "MetaRight"); S(0x5D, nullptr, "ContextMenu");
}

const char* KeyCodeFromMessage(WPARAM vk, LPARAM lParam) {
  Init();
  if (vk == VK_PAUSE) return "Pause";
  if (vk == VK_NUMLOCK) return "NumLock";
  if (vk == VK_SNAPSHOT) return "PrintScreen";
  UINT sc = (lParam >> 16) & 0xFF;
  bool ext = (lParam >> 24) & 1;
  if (sc >= 0x80) return nullptr;
  const char* n = kScan[sc][ext ? 1 : 0];
  if (!n && ext) n = kScan[sc][0];
  return n;
}
