#include "browser/boost_apply.h"

#include "app/application.h"
#include "browser/profile.h"
#include "browser/tab.h"

namespace orbit {

std::optional<Boost> LoadBoostForSite(Profile* profile, const std::wstring& domain) {
  if (!profile || profile->is_private() || domain.empty()) return std::nullopt;
  return ParseBoost(Application::Get()->store().LoadBoost(profile->id(), domain));
}

void SaveBoostForSite(Profile* profile, const std::wstring& domain, const std::optional<Boost>& boost) {
  if (!profile || profile->is_private() || domain.empty()) return;
  Store& store = Application::Get()->store();
  if (boost) {
    store.SaveBoost(profile->id(), domain, SerializeBoost(*boost));
  } else {
    store.DeleteBoost(profile->id(), domain);
  }
  ApplyBoostToSite(profile, domain);
}

void ApplyBoost(Tab* tab) {
  if (!tab || !tab->browser()) return;
  const std::wstring domain = BoostDomain(tab->url());
  if (domain.empty()) return;
  const std::optional<Boost> boost = LoadBoostForSite(tab->profile(), domain);
  // A page without a boost is left alone, unless one was just removed.
  static const std::string kClear = BoostApplyScript(nullptr);
  const std::string script = boost ? BoostApplyScript(&*boost) : kClear;
  tab->browser()->GetMainFrame()->ExecuteJavaScript(script, "", 0);
}

void ApplyBoostToSite(Profile* profile, const std::wstring& domain) {
  if (!profile) return;
  for (Workspace* ws : profile->containers()) {
    for (const auto& t : ws->tabs()) {
      if (t->browser() && BoostDomain(t->url()) == domain) ApplyBoost(t.get());
    }
  }
}

}  // namespace orbit
