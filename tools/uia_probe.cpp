// Development tool: lists the UI Automation children of a Orbit window using
// the native UIA client (IUIAutomation), i.e. the same path screen readers
// such as Narrator use. Usage: uia_probe <pid>
// Output: one line per element: "<control type id>\t<name>\t<class>".

#include <windows.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <cstdio>
#include <cwchar>
#include <cstdlib>

using Microsoft::WRL::ComPtr;

int wmain(int argc, wchar_t** argv) {
  if (argc != 2) {
    fwprintf(stderr, L"usage: uia_probe <pid>\n");
    return 2;
  }
  const int pid = _wtoi(argv[1]);
  ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  int code = 1;
  {
    ComPtr<IUIAutomation> uia;
    if (FAILED(::CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&uia)))) {
      return 1;
    }
    ComPtr<IUIAutomationElement> root;
    uia->GetRootElement(&root);
    VARIANT v_pid, v_class;
    v_pid.vt = VT_I4;
    v_pid.lVal = pid;
    v_class.vt = VT_BSTR;
    v_class.bstrVal = ::SysAllocString(L"OrbitBrowserWindow");
    ComPtr<IUIAutomationCondition> c1, c2, both;
    uia->CreatePropertyCondition(UIA_ProcessIdPropertyId, v_pid, &c1);
    uia->CreatePropertyCondition(UIA_ClassNamePropertyId, v_class, &c2);
    uia->CreateAndCondition(c1.Get(), c2.Get(), &both);
    ComPtr<IUIAutomationElement> window;
    root->FindFirst(TreeScope_Children, both.Get(), &window);
    ::SysFreeString(v_class.bstrVal);
    if (window) {
      ComPtr<IUIAutomationCondition> all;
      uia->CreateTrueCondition(&all);
      ComPtr<IUIAutomationElementArray> children;
      window->FindAll(TreeScope_Children, all.Get(), &children);
      int n = 0;
      if (children) children->get_Length(&n);
      for (int i = 0; i < n; ++i) {
        ComPtr<IUIAutomationElement> e;
        children->GetElement(i, &e);
        CONTROLTYPEID type = 0;
        BSTR name = nullptr, cls = nullptr;
        e->get_CurrentControlType(&type);
        e->get_CurrentName(&name);
        e->get_CurrentClassName(&cls);
        wprintf(L"%d\t%s\t%s\n", type, name ? name : L"", cls ? cls : L"");
        ::SysFreeString(name);
        ::SysFreeString(cls);
      }
      code = 0;
    }
  }
  ::CoUninitialize();
  return code;
}
