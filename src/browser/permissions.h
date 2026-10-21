#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "include/cef_permission_handler.h"

namespace orbit {

class Profile;

// Site permission decisions and prompts. Default is deny: nothing is granted
// without an explicit user choice. Decisions are per profile and per origin;
// private profiles keep them in memory only. UI thread only.
class PermissionManager {
 public:
  enum class Decision { kAsk, kAllow, kBlock };

  struct Entry {
    std::wstring origin;
    uint32_t permission = 0;  // single cef_permission_request_types_t bit
    bool allowed = false;
  };

  // CefPermissionHandler entry points (called from BrowserClient).
  bool OnRequestMediaAccess(Profile* profile, HWND owner, const std::wstring& origin,
                            uint32_t media_permissions,
                            CefRefPtr<CefMediaAccessCallback> callback);
  bool OnShowPrompt(Profile* profile, HWND owner, const std::wstring& origin,
                    uint32_t permissions, CefRefPtr<CefPermissionPromptCallback> callback);

  Decision Get(Profile* profile, const std::wstring& origin, uint32_t permission);
  void Set(Profile* profile, const std::wstring& origin, uint32_t permission, bool allow);
  std::vector<Entry> List(Profile* profile);
  void Forget(Profile* profile, const std::wstring& origin, uint32_t permission);
  void ForgetProfile(const Profile* profile);

  // Human-readable description of the requested permission bits.
  static std::wstring Describe(uint32_t permissions);

  // Self-tests only: answer prompts automatically ("remember" checked) instead
  // of showing a dialog, and count them.
  void SetTestAutoAnswer(bool allow) { test_auto_answer_ = allow ? 1 : 0; }
  int test_prompt_count() const { return test_prompt_count_; }
  // Permissions Orbit never grants (no supporting UI in Alloy style).
  static constexpr uint32_t kUnsupported =
      CEF_PERMISSION_TYPE_NOTIFICATIONS | CEF_PERMISSION_TYPE_AR_SESSION |
      CEF_PERMISSION_TYPE_VR_SESSION | CEF_PERMISSION_TYPE_HAND_TRACKING |
      CEF_PERMISSION_TYPE_WEB_APP_INSTALLATION | CEF_PERMISSION_TYPE_REGISTER_PROTOCOL_HANDLER |
      CEF_PERMISSION_TYPE_IDENTITY_PROVIDER;

 private:
  // Returns kAllow only if every bit is allowed, kBlock if any bit is blocked.
  Decision Combined(Profile* profile, const std::wstring& origin, uint32_t permissions);
  // Shows the prompt (after the CEF callback has returned); |done| receives the result.
  void Prompt(int64_t profile_id, HWND owner, const std::wstring& origin, uint32_t permissions,
              std::function<void(bool allowed)> done);

  std::map<std::wstring, bool> session_;
  int test_auto_answer_ = -1;  // -1: show real prompts
  int test_prompt_count_ = 0;  // private profiles: "<profile>|<origin>|<bit>"
};

// Maps media-capture bits (cef_media_access_permission_types_t) onto the
// permission-prompt bits used for storage.
uint32_t MediaToPermissionBits(uint32_t media_permissions);

}  // namespace orbit
