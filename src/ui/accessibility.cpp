#include "ui/accessibility.h"

#include <ole2.h>
#include <uiautomation.h>

#include <atomic>

#include "ui/browser_window.h"

namespace orbit {

struct AccessibilityHost::Shared {
  BrowserWindow* window = nullptr;  // null once the window is gone
  HWND hwnd = nullptr;
};

namespace {

class RootProvider;

// Finds the current item for an identity, or nullptr if it no longer exists.
bool FindItem(const AccessibilityHost::Shared& shared, int kind, intptr_t key,
              AccessibleItem* out, size_t* index = nullptr) {
  if (!shared.window) return false;
  const auto items = shared.window->AccessibleItems();
  for (size_t i = 0; i < items.size(); ++i) {
    if (items[i].kind == kind && items[i].key == key) {
      *out = items[i];
      if (index) *index = i;
      return true;
    }
  }
  return false;
}

UiaRect ToScreen(const AccessibilityHost::Shared& shared, const D2D1_RECT_F& r) {
  const float scale = static_cast<float>(::GetDpiForWindow(shared.hwnd)) / 96.f;
  POINT tl{static_cast<LONG>(r.left * scale), static_cast<LONG>(r.top * scale)};
  ::ClientToScreen(shared.hwnd, &tl);
  return UiaRect{static_cast<double>(tl.x), static_cast<double>(tl.y),
                 static_cast<double>((r.right - r.left) * scale),
                 static_cast<double>((r.bottom - r.top) * scale)};
}

// Minimal IUnknown plumbing for the provider objects below.
template <typename Self>
class RefCounted {
 public:
  ULONG AddRefImpl() { return ++refs_; }
  ULONG ReleaseImpl() {
    const ULONG n = --refs_;
    if (n == 0) delete static_cast<Self*>(this);
    return n;
  }

 private:
  std::atomic<ULONG> refs_{1};
};

class ItemProvider : public IRawElementProviderSimple,
                     public IRawElementProviderFragment,
                     public IInvokeProvider,
                     public ISelectionItemProvider,
                     public RefCounted<ItemProvider> {
 public:
  ItemProvider(std::shared_ptr<AccessibilityHost::Shared> shared, RootProvider* root, int kind,
               intptr_t key);

  // IUnknown
  ULONG STDMETHODCALLTYPE AddRef() override { return AddRefImpl(); }
  ULONG STDMETHODCALLTYPE Release() override { return ReleaseImpl(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple)) {
      *out = static_cast<IRawElementProviderSimple*>(this);
    } else if (riid == __uuidof(IRawElementProviderFragment)) {
      *out = static_cast<IRawElementProviderFragment*>(this);
    } else if (riid == __uuidof(IInvokeProvider)) {
      *out = static_cast<IInvokeProvider*>(this);
    } else if (riid == __uuidof(ISelectionItemProvider)) {
      *out = static_cast<ISelectionItemProvider*>(this);
    } else {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  // IRawElementProviderSimple
  HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* ret) override {
    *ret = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider |
                                        ProviderOptions_UseComThreading);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern, IUnknown** ret) override {
    *ret = nullptr;
    AccessibleItem item;
    if (!FindItem(*shared_, kind_, key_, &item)) return UIA_E_ELEMENTNOTAVAILABLE;
    if (pattern == UIA_InvokePatternId && item.enabled) {
      *ret = static_cast<IInvokeProvider*>(this);
    } else if (pattern == UIA_SelectionItemPatternId && item.role == AccessibleItem::Role::kTab) {
      *ret = static_cast<ISelectionItemProvider*>(this);
    }
    if (*ret) AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* ret) override {
    ret->vt = VT_EMPTY;
    AccessibleItem item;
    if (!FindItem(*shared_, kind_, key_, &item)) return UIA_E_ELEMENTNOTAVAILABLE;
    switch (id) {
      case UIA_NamePropertyId:
        ret->vt = VT_BSTR;
        ret->bstrVal = ::SysAllocString(item.name.c_str());
        break;
      case UIA_ControlTypePropertyId:
        ret->vt = VT_I4;
        ret->lVal = item.role == AccessibleItem::Role::kTab     ? UIA_TabItemControlTypeId
                    : item.role == AccessibleItem::Role::kGroup ? UIA_TreeItemControlTypeId
                                                                : UIA_ButtonControlTypeId;
        break;
      case UIA_IsKeyboardFocusablePropertyId:
        ret->vt = VT_BOOL;
        ret->boolVal = item.role == AccessibleItem::Role::kButton ? VARIANT_FALSE : VARIANT_TRUE;
        break;
      case UIA_HasKeyboardFocusPropertyId:
        ret->vt = VT_BOOL;
        ret->boolVal = item.focused ? VARIANT_TRUE : VARIANT_FALSE;
        break;
      case UIA_IsEnabledPropertyId:
        ret->vt = VT_BOOL;
        ret->boolVal = item.enabled ? VARIANT_TRUE : VARIANT_FALSE;
        break;
      case UIA_AutomationIdPropertyId: {
        ret->vt = VT_BSTR;
        const std::wstring aid = L"orbit." + std::to_wstring(kind_) + L"." + std::to_wstring(key_);
        ret->bstrVal = ::SysAllocString(aid.c_str());
        break;
      }
      case UIA_ExpandCollapseExpandCollapseStatePropertyId:
        if (item.role == AccessibleItem::Role::kGroup) {
          ret->vt = VT_I4;
          ret->lVal = item.expanded ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
        }
        break;
      default:
        break;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** ret) override {
    *ret = nullptr;  // fragments that are not HWND roots have no host
    return S_OK;
  }

  // IRawElementProviderFragment
  HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction,
                                     IRawElementProviderFragment** ret) override;
  HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** ret) override {
    const int ids[] = {UiaAppendRuntimeId, kind_, static_cast<int>(key_ & 0x7FFFFFFF),
                       static_cast<int>((static_cast<int64_t>(key_) >> 31) & 0x7FFFFFFF)};
    *ret = ::SafeArrayCreateVector(VT_I4, 0, 4);
    if (!*ret) return E_OUTOFMEMORY;
    for (LONG i = 0; i < 4; ++i) ::SafeArrayPutElement(*ret, &i, const_cast<int*>(&ids[i]));
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* ret) override {
    AccessibleItem item;
    if (!FindItem(*shared_, kind_, key_, &item)) return UIA_E_ELEMENTNOTAVAILABLE;
    *ret = ToScreen(*shared_, item.rect_dip);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** ret) override {
    *ret = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetFocus() override {
    AccessibleItem item;
    if (!FindItem(*shared_, kind_, key_, &item)) return UIA_E_ELEMENTNOTAVAILABLE;
    shared_->window->AccessibleFocus(kind_, key_);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** ret) override;

  // IInvokeProvider
  HRESULT STDMETHODCALLTYPE Invoke() override {
    AccessibleItem item;
    if (!FindItem(*shared_, kind_, key_, &item)) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!item.enabled) return UIA_E_ELEMENTNOTENABLED;
    // Posted, not run inline: actions may open menus or dialogs.
    shared_->window->AccessibleInvoke(kind_, key_);
    return S_OK;
  }

  // ISelectionItemProvider (tabs)
  HRESULT STDMETHODCALLTYPE Select() override { return Invoke(); }
  HRESULT STDMETHODCALLTYPE AddToSelection() override { return Invoke(); }
  HRESULT STDMETHODCALLTYPE RemoveFromSelection() override { return UIA_E_INVALIDOPERATION; }
  HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* ret) override {
    AccessibleItem item;
    if (!FindItem(*shared_, kind_, key_, &item)) return UIA_E_ELEMENTNOTAVAILABLE;
    *ret = item.selected ? TRUE : FALSE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** ret) override;

  int kind() const { return kind_; }
  intptr_t key() const { return key_; }

 private:
  ~ItemProvider();
  friend class RefCounted<ItemProvider>;
  std::shared_ptr<AccessibilityHost::Shared> shared_;
  RootProvider* root_;
  int kind_;
  intptr_t key_;
};

class RootProvider : public IRawElementProviderSimple,
                     public IRawElementProviderFragment,
                     public IRawElementProviderFragmentRoot,
                     public RefCounted<RootProvider> {
 public:
  explicit RootProvider(std::shared_ptr<AccessibilityHost::Shared> shared)
      : shared_(std::move(shared)) {}

  ItemProvider* CreateItem(int kind, intptr_t key) {
    return new ItemProvider(shared_, this, kind, key);  // refcount 1, owned by caller
  }
  // Child at |index| in the current item list (new reference), or nullptr.
  ItemProvider* ItemAt(size_t index) {
    if (!shared_->window) return nullptr;
    const auto items = shared_->window->AccessibleItems();
    if (index >= items.size()) return nullptr;
    return CreateItem(items[index].kind, items[index].key);
  }

  // IUnknown
  ULONG STDMETHODCALLTYPE AddRef() override { return AddRefImpl(); }
  ULONG STDMETHODCALLTYPE Release() override { return ReleaseImpl(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple)) {
      *out = static_cast<IRawElementProviderSimple*>(this);
    } else if (riid == __uuidof(IRawElementProviderFragment)) {
      *out = static_cast<IRawElementProviderFragment*>(this);
    } else if (riid == __uuidof(IRawElementProviderFragmentRoot)) {
      *out = static_cast<IRawElementProviderFragmentRoot*>(this);
    } else {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  // IRawElementProviderSimple
  HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* ret) override {
    *ret = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider |
                                        ProviderOptions_UseComThreading);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID, IUnknown** ret) override {
    *ret = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* ret) override {
    ret->vt = VT_EMPTY;
    if (!shared_->window) return UIA_E_ELEMENTNOTAVAILABLE;
    if (id == UIA_ControlTypePropertyId) {
      ret->vt = VT_I4;
      ret->lVal = UIA_WindowControlTypeId;
    }
    return S_OK;  // name etc. come from the HWND host provider
  }
  HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** ret) override {
    if (!shared_->hwnd) {
      *ret = nullptr;
      return UIA_E_ELEMENTNOTAVAILABLE;
    }
    return ::UiaHostProviderFromHwnd(shared_->hwnd, ret);
  }

  // IRawElementProviderFragment
  HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction,
                                     IRawElementProviderFragment** ret) override {
    *ret = nullptr;
    if (!shared_->window) return UIA_E_ELEMENTNOTAVAILABLE;
    if (direction == NavigateDirection_FirstChild) {
      *ret = ItemAt(0);
    } else if (direction == NavigateDirection_LastChild) {
      const size_t n = shared_->window->AccessibleItems().size();
      if (n) *ret = ItemAt(n - 1);
    }
    return S_OK;  // parent/siblings of an HWND root come from the host
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** ret) override {
    *ret = nullptr;  // HWND-hosted roots use the host's runtime id
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* ret) override {
    *ret = UiaRect{};  // the host provides it
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** ret) override {
    *ret = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetFocus() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** ret) override {
    *ret = this;
    AddRef();
    return S_OK;
  }

  // IRawElementProviderFragmentRoot
  HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y,
                                                     IRawElementProviderFragment** ret) override {
    *ret = nullptr;
    if (!shared_->window) return UIA_E_ELEMENTNOTAVAILABLE;
    POINT pt{static_cast<LONG>(x), static_cast<LONG>(y)};
    ::ScreenToClient(shared_->hwnd, &pt);
    const float scale = static_cast<float>(::GetDpiForWindow(shared_->hwnd)) / 96.f;
    const float dx = static_cast<float>(pt.x) / scale, dy = static_cast<float>(pt.y) / scale;
    for (const auto& item : shared_->window->AccessibleItems()) {
      const auto& r = item.rect_dip;
      if (dx >= r.left && dx < r.right && dy >= r.top && dy < r.bottom) {
        *ret = CreateItem(item.kind, item.key);
        return S_OK;
      }
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** ret) override {
    *ret = nullptr;
    if (!shared_->window) return UIA_E_ELEMENTNOTAVAILABLE;
    for (const auto& item : shared_->window->AccessibleItems()) {
      if (item.focused) {
        *ret = CreateItem(item.kind, item.key);
        break;
      }
    }
    return S_OK;
  }

 private:
  friend class RefCounted<RootProvider>;
  ~RootProvider() = default;
  std::shared_ptr<AccessibilityHost::Shared> shared_;
};

ItemProvider::ItemProvider(std::shared_ptr<AccessibilityHost::Shared> shared, RootProvider* root,
                           int kind, intptr_t key)
    : shared_(std::move(shared)), root_(root), kind_(kind), key_(key) {
  root_->AddRef();
}

ItemProvider::~ItemProvider() { root_->Release(); }

HRESULT ItemProvider::Navigate(NavigateDirection direction, IRawElementProviderFragment** ret) {
  *ret = nullptr;
  AccessibleItem item;
  size_t index = 0;
  if (!FindItem(*shared_, kind_, key_, &item, &index)) return UIA_E_ELEMENTNOTAVAILABLE;
  switch (direction) {
    case NavigateDirection_Parent:
      *ret = root_;
      root_->AddRef();
      break;
    case NavigateDirection_NextSibling:
      *ret = root_->ItemAt(index + 1);
      break;
    case NavigateDirection_PreviousSibling:
      if (index > 0) *ret = root_->ItemAt(index - 1);
      break;
    default:
      break;  // items have no children
  }
  return S_OK;
}

HRESULT ItemProvider::get_FragmentRoot(IRawElementProviderFragmentRoot** ret) {
  *ret = root_;
  root_->AddRef();
  return S_OK;
}

HRESULT ItemProvider::get_SelectionContainer(IRawElementProviderSimple** ret) {
  *ret = root_;
  root_->AddRef();
  return S_OK;
}

}  // namespace

AccessibilityHost::AccessibilityHost(BrowserWindow* window)
    : shared_(std::make_shared<Shared>()) {
  shared_->window = window;
  shared_->hwnd = window->hwnd();
}

AccessibilityHost::~AccessibilityHost() { Detach(); }

void AccessibilityHost::Detach() {
  if (!shared_->window) return;
  shared_->window = nullptr;
  if (root_) {
    // Tell UIA the tree is gone, then drop our reference.
    ::UiaDisconnectProvider(static_cast<RootProvider*>(root_));
    static_cast<RootProvider*>(root_)->Release();
    root_ = nullptr;
  }
  shared_->hwnd = nullptr;
}

LRESULT AccessibilityHost::HandleGetObject(HWND hwnd, WPARAM wp, LPARAM lp) {
  if (static_cast<long>(lp) != static_cast<long>(UiaRootObjectId) || !shared_->window) {
    return ::DefWindowProcW(hwnd, WM_GETOBJECT, wp, lp);
  }
  if (!shared_->hwnd) shared_->hwnd = hwnd;
  if (!root_) root_ = new RootProvider(shared_);
  return ::UiaReturnRawElementProvider(hwnd, wp, lp, static_cast<RootProvider*>(root_));
}

void AccessibilityHost::NotifyFocus(int kind, intptr_t key) {
  if (!root_ || !shared_->window || !::UiaClientsAreListening()) return;
  ItemProvider* item = static_cast<RootProvider*>(root_)->CreateItem(kind, key);
  ::UiaRaiseAutomationEvent(item, UIA_AutomationFocusChangedEventId);
  item->Release();
}

}  // namespace orbit
