// Live folders (ZenLiveFoldersManager + providers): folders whose tabs come
// from an RSS/Atom feed or the user's GitHub pull requests / issues, fetched
// in the background with the profile's cookies (as Zen's SEC_COOKIES_INCLUDE
// channel) and refreshed on an interval. Parsing lives in core/live_folders.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/live_folders.h"

namespace orbit {

class Profile;
class Tab;
class Workspace;

class LiveFolders {
 public:
  struct Entry {
    int64_t group_id = 0;
    LiveFolderState state;
    std::set<std::wstring> dismissed;  // composite ids
    bool fetching = false;
  };

  // Restores the stored live folders (after the profiles are loaded) and
  // starts the refresh clock.
  void Load();
  void Shutdown();

  // createFolder: a new collapsed live folder at the top of |ws|'s tabs. An
  // RSS folder is named after its feed, so it appears once the feed answered
  // (|done| gets the folder id, 0 on failure). GitHub folders appear at once;
  // for them a non-empty |feed_url| replaces github.com (self-tests).
  void Create(Workspace* ws, LiveFolderState::Kind kind, const std::wstring& feed_url,
              std::function<void(int64_t)> done);

  const Entry* Find(int64_t group_id) const;
  bool IsLive(int64_t group_id) const { return Find(group_id) != nullptr; }
  void Refresh(int64_t group_id);
  // Applies a changed setting and refetches (onOptionTrigger).
  void Update(int64_t group_id, const std::function<void(LiveFolderState&)>& change);

  // Tab hooks: a user close or moving a tab out of its folder dismisses the
  // item, so it does not come back (#onTabDismiss).
  void OnTabDismissed(Tab* tab);
  // Removed by Orbit itself (not a dismissal): just drops its item id.
  void ForgetTab(Tab* tab);
  // The folder went away (deleted or unpacked): forget it and its items.
  void OnFolderRemoved(int64_t group_id);

  // Self-tests: run the clock now (fetches whatever is due).
  void TickForTesting() { Tick(); }

 private:
  struct Fetched {
    std::wstring error;  // a zen-live-folder-* id; empty with |items|
    std::vector<LiveItem> items;
    std::set<std::wstring> repos;
    int json_api = -1;
  };
  void Tick();
  void ScheduleTick();
  void Fetch(Entry& entry);
  void FetchGithub(Profile* profile, LiveFolderState state, std::function<void(Fetched)> done);
  void Apply(int64_t group_id, const Fetched& result);
  void Save(const Entry& entry);
  Workspace* WorkspaceOf(int64_t group_id) const;

  std::map<int64_t, Entry> entries_;
  int generation_ = 0;
  bool loaded_ = false;
  bool removing_ = false;  // our own removals are not dismissals
};

}  // namespace orbit
