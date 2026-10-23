#pragma once

// Building blocks shared by the orbit:// page builders (UI thread only).

#include <string>

#include "browser/internal_pages.h"

namespace orbit {

class Profile;

// Wraps |body| in the common document (design tokens, light/dark theme, base
// styles) and appends |script| plus the orbitAction()/orbitGet() helpers.
std::string PageShell(const std::string& title, const std::string& body,
                      const std::string& script);

// Pages (HTML + JS).
InternalResponse HistoryPage(const InternalRequest& req);
InternalResponse BookmarksPage(const InternalRequest& req);
InternalResponse DownloadsPage(const InternalRequest& req);
InternalResponse SettingsPage(const InternalRequest& req);
InternalResponse DiagnosticsPage(const InternalRequest& req);
InternalResponse LibraryPage(const InternalRequest& req);
InternalResponse SiteDataPage(const InternalRequest& req);
InternalResponse WelcomePage(const InternalRequest& req);
InternalResponse ThemePage(const InternalRequest& req);
InternalResponse BoostPage(const InternalRequest& req);
InternalResponse ExtensionsPage(const InternalRequest& req);
InternalResponse FindPage(const InternalRequest& req);

// JSON APIs under orbit://<page>/api/<name>. The caller has already verified
// req.same_page; mutating endpoints check req.trusted_action themselves.
InternalResponse HistoryApi(const InternalRequest& req, Profile* profile);
InternalResponse BookmarksApi(const InternalRequest& req, Profile* profile);
InternalResponse DownloadsApi(const InternalRequest& req, Profile* profile);
InternalResponse SettingsApi(const InternalRequest& req, Profile* profile);
InternalResponse NewTabApi(const InternalRequest& req, Profile* profile);
InternalResponse DiagnosticsApi(const InternalRequest& req, Profile* profile);
InternalResponse LibraryApi(const InternalRequest& req, Profile* profile);
InternalResponse SiteDataApi(const InternalRequest& req, Profile* profile);
InternalResponse WelcomeApi(const InternalRequest& req, Profile* profile);
InternalResponse ThemeApi(const InternalRequest& req, Profile* profile);
InternalResponse BoostApi(const InternalRequest& req, Profile* profile);
InternalResponse ExtensionsApi(const InternalRequest& req, Profile* profile);
InternalResponse FindApi(const InternalRequest& req, Profile* profile);

}  // namespace orbit
