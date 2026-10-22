// Applying boosts to pages: the stored boost of a tab's site, when its main
// frame starts and finishes loading, and to every open tab of a site after
// an edit. UI thread.
#pragma once

#include <optional>
#include <string>

#include "core/boosts.h"

namespace orbit {

class Profile;
class Tab;

std::optional<Boost> LoadBoostForSite(Profile* profile, const std::wstring& domain);
void SaveBoostForSite(Profile* profile, const std::wstring& domain, const std::optional<Boost>& boost);
void ApplyBoost(Tab* tab);
void ApplyBoostToSite(Profile* profile, const std::wstring& domain);

}  // namespace orbit
