#include "browser/permissions.h"

#include <windows.h>
#include <commctrl.h>

#include <functional>

#include "app/application.h"
#include "browser/profile.h"
#include "core/log.h"
#include "include/base/cef_callback.h"
#include "include/wrapper/cef_closure_task.h"

namespace orbit {
namespace {

constexpr wchar_t kPrefPrefix[] = L"perm|";

struct Label {
  uint32_t bit;
  const wchar_t* text;
};
constexpr Label kLabels[] = {
    {CEF_PERMISSION_TYPE_CAMERA_STREAM, L"Use your camera"},
    {CEF_PERMISSION_TYPE_MIC_STREAM, L"Use your microphone"},
    {CEF_PERMISSION_TYPE_CAMERA_PAN_TILT_ZOOM, L"Move your camera"},
    {CEF_PERMISSION_TYPE_GEOLOCATION, L"Know your location"},
    {CEF_PERMISSION_TYPE_CLIPBOARD, L"See text and images copied to the clipboard"},
    {CEF_PERMISSION_TYPE_MIDI_SYSEX, L"Control and reprogram your MIDI devices"},
    {CEF_PERMISSION_TYPE_IDLE_DETECTION, L"Know when you are actively using this device"},
    {CEF_PERMISSION_TYPE_LOCAL_FONTS, L"Use the fonts installed on your computer"},
    {CEF_PERMISSION_TYPE_WINDOW_MANAGEMENT, L"Manage windows on all your displays"},
    {CEF_PERMISSION_TYPE_MULTIPLE_DOWNLOADS, L"Download multiple files"},
    {CEF_PERMISSION_TYPE_STORAGE_ACCESS, L"Use its cookies and data while embedded in other sites"},
    {CEF_PERMISSION_TYPE_TOP_LEVEL_STORAGE_ACCESS, L"Use cookies and data across related sites"},
    {CEF_PERMISSION_TYPE_POINTER_LOCK, L"Lock and use your mouse pointer"},
    {CEF_PERMISSION_TYPE_KEYBOARD_LOCK, L"Capture your keyboard"},
    {CEF_PERMISSION_TYPE_FILE_SYSTEM_ACCESS, L"Edit files and folders"},
    {CEF_PERMISSION_TYPE_SENSORS, L"Use motion or light sensors"},
    {CEF_PERMISSION_TYPE_LOCAL_NETWORK, L"Access devices on your local network"},
    {CEF_PERMISSION_TYPE_LOOPBACK_NETWORK, L"Access services running on this device"},
    {CEF_PERMISSION_TYPE_PROTECTED_MEDIA_IDENTIFIER, L"Identify your device to play protected content"},
    {CEF_PERMISSION_TYPE_CAPTURED_SURFACE_CONTROL, L"Scroll and zoom a captured tab or window"},
    {CEF_PERMISSION_TYPE_DISK_QUOTA, L"Store large amounts of data"},
};

std::wstring PrefKey(const std::wstring& origin, uint32_t bit) {
  return std::wstring(kPrefPrefix) + origin + L"|" + std::to_wstring(bit);
}

std::wstring SessionKey(const Profile* profile, const std::wstring& origin, uint32_t bit) {
  return std::to_wstring(profile->id()) + L"|" + origin + L"|" + std::to_wstring(bit);
}

}  // namespace

uint32_t MediaToPermissionBits(uint32_t media) {
  uint32_t bits = 0;
  if (media & (CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE | CEF_MEDIA_PERMISSION_DESKTOP_AUDIO_CAPTURE)) {
    bits |= CEF_PERMISSION_TYPE_MIC_STREAM;
  }
  if (media & (CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE | CEF_MEDIA_PERMISSION_DESKTOP_VIDEO_CAPTURE)) {
    bits |= CEF_PERMISSION_TYPE_CAMERA_STREAM;
  }
  return bits;
}

std::wstring PermissionManager::Describe(uint32_t permissions) {
  std::wstring out;
  for (const auto& l : kLabels) {
    if (permissions & l.bit) out += L"• " + std::wstring(l.text) + L"\n";
  }
  return out.empty() ? L"• Use a browser feature that needs your permission\n" : out;
}

PermissionManager::Decision PermissionManager::Get(Profile* profile, const std::wstring& origin,
                                                   uint32_t bit) {
  if (profile->is_private()) {
    auto it = session_.find(SessionKey(profile, origin, bit));
    if (it == session_.end()) return Decision::kAsk;
    return it->second ? Decision::kAllow : Decision::kBlock;
  }
  auto value = Application::Get()->store().GetPref(profile->id(), PrefKey(origin, bit));
  if (!value) return Decision::kAsk;
  return *value == L"allow" ? Decision::kAllow : Decision::kBlock;
}

void PermissionManager::Set(Profile* profile, const std::wstring& origin, uint32_t bit,
                            bool allow) {
  if (profile->is_private()) {
    session_[SessionKey(profile, origin, bit)] = allow;
  } else {
    Application::Get()->store().SetPref(profile->id(), PrefKey(origin, bit),
                                        allow ? L"allow" : L"block");
  }
}

void PermissionManager::Forget(Profile* profile, const std::wstring& origin, uint32_t bit) {
  if (profile->is_private()) {
    session_.erase(SessionKey(profile, origin, bit));
    return;
  }
  auto& db = Application::Get()->store().db();
  auto s = db.Prepare("DELETE FROM prefs WHERE scope = ? AND key = ?");
  s.Bind(1, profile->id()).Bind(2, PrefKey(origin, bit));
  s.Run();
}

std::vector<PermissionManager::Entry> PermissionManager::List(Profile* profile) {
  std::vector<Entry> out;
  if (profile->is_private()) return out;  // session decisions are not listed
  auto& db = Application::Get()->store().db();
  auto s = db.Prepare("SELECT key, value FROM prefs WHERE scope = ? AND key LIKE 'perm|%' ORDER BY key");
  s.Bind(1, profile->id());
  while (s.Step()) {
    const std::wstring key = s.ColumnText(0).substr(5);  // strip "perm|"
    const size_t bar = key.rfind(L'|');
    if (bar == std::wstring::npos) continue;
    out.push_back({key.substr(0, bar), static_cast<uint32_t>(_wtoi64(key.c_str() + bar + 1)),
                   s.ColumnText(1) == L"allow"});
  }
  return out;
}

void PermissionManager::ForgetProfile(const Profile* profile) {
  const std::wstring prefix = std::to_wstring(profile->id()) + L"|";
  std::erase_if(session_, [&](const auto& kv) { return kv.first.starts_with(prefix); });
}

PermissionManager::Decision PermissionManager::Combined(Profile* profile,
                                                        const std::wstring& origin,
                                                        uint32_t permissions) {
  bool all_allowed = true;
  for (uint32_t bit = 1; bit != 0 && bit <= permissions; bit <<= 1) {
    if (!(permissions & bit)) continue;
    const Decision d = Get(profile, origin, bit);
    if (d == Decision::kBlock) return Decision::kBlock;
    if (d != Decision::kAllow) all_allowed = false;
  }
  return all_allowed ? Decision::kAllow : Decision::kAsk;
}

void PermissionManager::Prompt(int64_t profile_id, HWND owner, const std::wstring& origin,
                               uint32_t permissions, std::function<void(bool)> done) {
  if (test_auto_answer_ >= 0) {
    ++test_prompt_count_;
    const bool allow = test_auto_answer_ == 1;
    if (Profile* profile = Application::Get()->FindAnyProfile(profile_id)) {
      for (uint32_t bit = 1; bit != 0 && bit <= permissions; bit <<= 1) {
        if (permissions & bit) Set(profile, origin, bit, allow);
      }
    }
    CefPostTask(TID_UI, base::BindOnce([](std::function<void(bool)> cb, bool a) { cb(a); },
                                       std::move(done), allow));
    return;
  }
  // Shown from a posted task, never from inside the CEF callback.
  CefPostTask(TID_UI, base::BindOnce(
                          [](int64_t pid, HWND owner_hwnd, std::wstring site, uint32_t bits,
                             std::function<void(bool)> callback) {
                            Application* app = Application::Get();
                            Profile* profile = app ? app->FindAnyProfile(pid) : nullptr;
                            if (!profile) {
                              callback(false);
                              return;
                            }
                            const std::wstring content =
                                site + L" wants to:\n" + PermissionManager::Describe(bits);
                            TASKDIALOGCONFIG config{sizeof(config)};
                            config.hwndParent = ::IsWindow(owner_hwnd) ? owner_hwnd : nullptr;
                            config.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW |
                                             TDF_ALLOW_DIALOG_CANCELLATION |
                                             TDF_VERIFICATION_FLAG_CHECKED;
                            config.pszWindowTitle = L"Orbit";
                            config.pszMainInstruction = L"Allow this site?";
                            config.pszContent = content.c_str();
                            config.pszVerificationText = L"Remember my choice for this site";
                            const TASKDIALOG_BUTTON buttons[] = {{IDOK, L"Allow"},
                                                                 {IDCANCEL, L"Block"}};
                            config.pButtons = buttons;
                            config.cButtons = 2;
                            config.nDefaultButton = IDCANCEL;
                            int choice = IDCANCEL;
                            BOOL remember = TRUE;
                            if (FAILED(::TaskDialogIndirect(&config, &choice, nullptr, &remember))) {
                              choice = IDCANCEL;
                              remember = FALSE;
                            }
                            const bool allow = choice == IDOK;
                            if (remember) {
                              for (uint32_t bit = 1; bit != 0 && bit <= bits; bit <<= 1) {
                                if (bits & bit) app->permissions().Set(profile, site, bit, allow);
                              }
                            }
                            callback(allow);
                          },
                          profile_id, owner, origin, permissions, std::move(done)));
}

bool PermissionManager::OnRequestMediaAccess(Profile* profile, HWND owner,
                                             const std::wstring& origin, uint32_t media,
                                             CefRefPtr<CefMediaAccessCallback> callback) {
  if (!profile || origin.empty()) return false;  // default: deny
  const uint32_t bits = MediaToPermissionBits(media);
  auto finish = [callback, media](bool allow) {
    if (allow) {
      callback->Continue(media);
    } else {
      callback->Cancel();
    }
  };
  switch (Combined(profile, origin, bits)) {
    case Decision::kAllow:
      finish(true);
      return true;
    case Decision::kBlock:
      finish(false);
      return true;
    case Decision::kAsk:
      Prompt(profile->id(), owner, origin, bits, finish);
      return true;
  }
  return false;
}

bool PermissionManager::OnShowPrompt(Profile* profile, HWND owner, const std::wstring& origin,
                                     uint32_t permissions,
                                     CefRefPtr<CefPermissionPromptCallback> callback) {
  if (!profile || origin.empty()) return false;  // default: ignore (deny)
  if (permissions & kUnsupported) {
    callback->Continue(CEF_PERMISSION_RESULT_DENY);
    return true;
  }
  auto finish = [callback](bool allow) {
    callback->Continue(allow ? CEF_PERMISSION_RESULT_ACCEPT : CEF_PERMISSION_RESULT_DENY);
  };
  switch (Combined(profile, origin, permissions)) {
    case Decision::kAllow:
      finish(true);
      return true;
    case Decision::kBlock:
      finish(false);
      return true;
    case Decision::kAsk:
      Prompt(profile->id(), owner, origin, permissions, finish);
      return true;
  }
  return false;
}

}  // namespace orbit
