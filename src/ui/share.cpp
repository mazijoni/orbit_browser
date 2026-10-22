#include "ui/share.h"

#include <roapi.h>
#include <shobjidl_core.h>
#include <windows.applicationmodel.datatransfer.h>
#include <windows.foundation.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>

#include "core/log.h"

namespace orbit::ui {
namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
namespace dt = ABI::Windows::ApplicationModel::DataTransfer;
namespace wf = ABI::Windows::Foundation;

// One registration per window: a new share replaces the previous handler.
struct Registration {
  HWND window = nullptr;
  ComPtr<dt::IDataTransferManager> manager;
  EventRegistrationToken token{};
};
Registration g_last;

}  // namespace

bool ShareWebLink(HWND window, const std::wstring& url, const std::wstring& title) {
  ComPtr<IDataTransferManagerInterop> interop;
  HRESULT hr = ::RoGetActivationFactory(
      HStringReference(RuntimeClass_Windows_ApplicationModel_DataTransfer_DataTransferManager).Get(),
      IID_PPV_ARGS(&interop));
  if (FAILED(hr)) {
    ORBIT_LOG_WARNING("share: DataTransferManager unavailable (0x%08lx)", hr);
    return false;
  }
  ComPtr<dt::IDataTransferManager> manager;
  if (FAILED(interop->GetForWindow(window, IID_PPV_ARGS(&manager)))) return false;
  if (g_last.manager) g_last.manager->remove_DataRequested(g_last.token);
  g_last = {};
  using Handler = wf::ITypedEventHandler<dt::DataTransferManager*, dt::DataRequestedEventArgs*>;
  auto handler = Callback<Handler>([url, title](dt::IDataTransferManager*, dt::IDataRequestedEventArgs* args) {
    ComPtr<dt::IDataRequest> request;
    ComPtr<dt::IDataPackage> data;
    ComPtr<dt::IDataPackagePropertySet> properties;
    if (FAILED(args->get_Request(&request)) || FAILED(request->get_Data(&data)) ||
        FAILED(data->get_Properties(&properties))) {
      return S_OK;
    }
    properties->put_Title(HStringReference(title.empty() ? url.c_str() : title.c_str()).Get());
    ComPtr<wf::IUriRuntimeClassFactory> uri_factory;
    ComPtr<wf::IUriRuntimeClass> uri;
    ComPtr<dt::IDataPackage2> data2;
    if (SUCCEEDED(::RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Foundation_Uri).Get(),
                                           IID_PPV_ARGS(&uri_factory))) &&
        SUCCEEDED(uri_factory->CreateUri(HStringReference(url.c_str()).Get(), &uri)) &&
        SUCCEEDED(data.As(&data2))) {
      data2->SetWebLink(uri.Get());
    } else {
      data->SetText(HStringReference(url.c_str()).Get());
    }
    return S_OK;
  });
  EventRegistrationToken token{};
  if (!handler || FAILED(manager->add_DataRequested(handler.Get(), &token))) return false;
  g_last = {window, manager, token};
  hr = interop->ShowShareUIForWindow(window);
  if (FAILED(hr)) ORBIT_LOG_WARNING("share: ShowShareUIForWindow failed (0x%08lx)", hr);
  return SUCCEEDED(hr);
}

}  // namespace orbit::ui
