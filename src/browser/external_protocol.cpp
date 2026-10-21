#include "browser/external_protocol.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include "app/application.h"
#include "browser/browser_client.h"
#include "browser/profile.h"
#include "browser/tab.h"
#include "core/external_scheme.h"
#include "core/log.h"
#include "ui/browser_window.h"

namespace orbit {

void ConfirmExternalProtocol(int browser_id, const std::wstring& url) {
  Tab* tab = Application::Get()->client()->FindTab(browser_id);
  BrowserWindow* window = (tab && tab->workspace()) ? tab->workspace()->window() : nullptr;
  if (!window) return;

  std::wstring scheme;
  if (!IsExternalUrlLaunchable(url, &scheme)) {
    ORBIT_LOG_WARNING("refused external protocol launch (blocked or malformed scheme)");
    return;
  }

  const std::wstring content =
      L"This page wants to open a \"" + scheme + L":\" link in an external application.";
  TASKDIALOGCONFIG config{sizeof(config)};
  config.hwndParent = window->hwnd();
  config.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
  config.pszWindowTitle = L"Orbit";
  config.pszMainInstruction = L"Open external application?";
  config.pszContent = content.c_str();
  const TASKDIALOG_BUTTON buttons[] = {{IDOK, L"Open"}, {IDCANCEL, L"Cancel"}};
  config.pButtons = buttons;
  config.cButtons = 2;
  config.nDefaultButton = IDCANCEL;
  int choice = IDCANCEL;
  if (FAILED(::TaskDialogIndirect(&config, &choice, nullptr, nullptr)) || choice != IDOK) {
    return;
  }
  // SEE_MASK_NOZONECHECKS is not used; the shell applies its own policies.
  SHELLEXECUTEINFOW info{sizeof(info)};
  info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  info.hwnd = window->hwnd();
  info.lpVerb = L"open";
  info.lpFile = url.c_str();
  info.nShow = SW_SHOWNORMAL;
  if (!::ShellExecuteExW(&info)) {
    ORBIT_LOG_WARNING("ShellExecuteEx failed for external protocol (%lu)", ::GetLastError());
  }
}

}  // namespace orbit
