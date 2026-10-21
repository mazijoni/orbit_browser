#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <d2d1.h>

namespace orbit {

class BrowserWindow;

// One element of the custom-drawn chrome, as exposed to UI Automation.
struct AccessibleItem {
  enum class Role { kButton, kTab, kGroup };
  // Stable identity across repaints: (kind, key). |key| is a tab pointer value,
  // a group id, or an index for fixed buttons.
  int kind = 0;
  intptr_t key = 0;
  Role role = Role::kButton;
  std::wstring name;
  D2D1_RECT_F rect_dip{};  // client coordinates in DIPs
  bool selected = false;   // active tab / current workspace
  bool expanded = true;    // groups
  bool focused = false;    // keyboard focus within the sidebar
  bool enabled = true;
};

// UI Automation provider for a BrowserWindow (WM_GETOBJECT). Elements are
// looked up by identity on every call, so they never point at freed state;
// a stale element reports UIA_E_ELEMENTNOTAVAILABLE.
class AccessibilityHost {
 public:
  explicit AccessibilityHost(BrowserWindow* window);
  ~AccessibilityHost();
  AccessibilityHost(const AccessibilityHost&) = delete;
  AccessibilityHost& operator=(const AccessibilityHost&) = delete;

  LRESULT HandleGetObject(HWND hwnd, WPARAM wp, LPARAM lp);
  // Raises a UIA focus-changed event for the item with this identity.
  void NotifyFocus(int kind, intptr_t key);
  // Called when the window is destroyed: outstanding providers go stale.
  void Detach();

  struct Shared;  // state shared with provider COM objects

 private:
  std::shared_ptr<Shared> shared_;
  void* root_ = nullptr;  // RootProvider*, ref-counted COM object
};

}  // namespace orbit
