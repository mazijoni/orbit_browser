#include "browser/profile.h"

#include <algorithm>
#include <functional>
#include <cstddef>

#include "app/application.h"
#include "browser/internal_pages.h"
#include "browser/tab.h"
#include "core/log.h"

namespace orbit {

Workspace::Workspace(Profile* profile, WorkspaceRecord record)
    : profile_(profile), record_(std::move(record)) {}

Workspace::~Workspace() = default;

bool IsEssentialTab(const Tab* tab) {
  return tab && tab->workspace() && tab->workspace()->is_essentials();
}

void Workspace::set_profile(Profile* profile) {
  profile_ = profile;
  record_.profile_id = profile->id();
  if (essentials_) essentials_->set_profile(profile);
}

Workspace* Workspace::EnsureEssentials() {
  if (essentials_) return essentials_.get();
  WorkspaceRecord rec;
  rec.profile_id = profile_->id();
  rec.name = L"Essentials";
  rec.kind = WorkspaceRecord::kKindEssentials;
  rec.owner_id = record_.id;
  if (!profile_->is_private()) {
    auto stored = Application::Get()->store().CreateWorkspace(rec);
    if (!stored) return nullptr;
    rec = *stored;
  }
  AdoptEssentials(std::make_unique<Workspace>(profile_, rec));
  return essentials_.get();
}

void Workspace::AdoptEssentials(std::unique_ptr<Workspace> grid) {
  essentials_ = std::move(grid);
  if (essentials_) essentials_->owner_ = this;
}

int Workspace::IndexOf(const Tab* tab) const {
  for (size_t i = 0; i < tabs_.size(); ++i) {
    if (tabs_[i].get() == tab) return static_cast<int>(i);
  }
  return -1;
}

int Workspace::PinnedCount() const {
  return static_cast<int>(
      std::count_if(tabs_.begin(), tabs_.end(), [](const auto& t) { return t->pinned(); }));
}

Tab* Workspace::InsertTab(std::unique_ptr<Tab> tab, int index) {
  const int pinned = PinnedCount();
  const int lo = tab->pinned() ? 0 : pinned;
  const int hi = tab->pinned() ? pinned : static_cast<int>(tabs_.size());
  index = (index < 0) ? hi : std::clamp(index, lo, hi);
  tab->set_workspace(this);
  Tab* raw = tab.get();
  tabs_.insert(tabs_.begin() + index, std::move(tab));
  if (!profile_->is_private()) {
    TabRecord& rec = raw->mutable_record();
    rec.workspace_id = record_.id;
    if (rec.id == 0) rec.id = Application::Get()->store().InsertTab(rec);
  }
  PersistOrder();
  return raw;
}

std::unique_ptr<Tab> Workspace::RemoveTab(Tab* tab) {
  const int index = IndexOf(tab);
  if (index < 0) return nullptr;
  RemoveFromSplit(tab);
  std::unique_ptr<Tab> out = std::move(tabs_[static_cast<size_t>(index)]);
  tabs_.erase(tabs_.begin() + index);
  if (active_tab_ == tab) active_tab_ = nullptr;
  out->set_workspace(nullptr);
  if (!out->record().live_item.empty()) Application::Get()->live_folders().OnTabDismissed(out.get());
  if (out->record().group_id) {
    out->set_group(0);
    PruneEmptyGroups();
  }
  PersistOrder();
  return out;
}

const TabGroupRecord* Workspace::FindGroup(int64_t id) const {
  for (const auto& g : groups_) {
    if (g.id == id) return &g;
  }
  return nullptr;
}

int Workspace::GroupSize(int64_t group_id) const {
  return static_cast<int>(std::count_if(tabs_.begin(), tabs_.end(), [&](const auto& t) {
    return !t->empty_placeholder() && InGroup(t.get(), group_id);
  }));
}

int Workspace::GroupDepth(int64_t group_id) const {
  int depth = 0;
  for (const TabGroupRecord* g = FindGroup(group_id); g && g->parent_id && depth < 16; ++depth) {
    g = FindGroup(g->parent_id);
  }
  return depth;
}

std::vector<int64_t> Workspace::GroupChain(const Tab* tab) const {
  std::vector<int64_t> chain;
  for (int64_t id = tab ? tab->record().group_id : 0; id && chain.size() < 16;) {
    const TabGroupRecord* g = FindGroup(id);
    if (!g) break;
    chain.insert(chain.begin(), id);
    id = g->parent_id;
  }
  return chain;
}

bool Workspace::InGroup(const Tab* tab, int64_t group_id) const {
  const auto chain = GroupChain(tab);
  return std::find(chain.begin(), chain.end(), group_id) != chain.end();
}

std::vector<Tab*> Workspace::GroupTabs(int64_t group_id) const {
  std::vector<Tab*> out;
  for (const auto& t : tabs_) {
    if (InGroup(t.get(), group_id)) out.push_back(t.get());
  }
  return out;
}

int64_t Workspace::CreateSubfolder(int64_t parent_id, const std::wstring& name, Tab** placeholder,
                                   uint32_t color) {
  const TabGroupRecord* parent = parent_id ? FindGroup(parent_id) : nullptr;
  if (parent_id && (!parent || GroupDepth(parent_id) >= kMaxFolderDepth - 1)) return 0;
  // The empty tab goes after everything in the parent folder (a top-level
  // folder starts the unpinned tabs).
  const auto members = parent ? GroupTabs(parent_id) : std::vector<Tab*>{};
  TabRecord rec;
  rec.url = Application::kNewTabUrl;
  rec.pinned = false;
  const int index = !parent ? PinnedCount() : members.empty() ? -1 : IndexOf(members.back()) + 1;
  Tab* empty = InsertTab(std::make_unique<Tab>(this, rec), index);
  empty->set_empty_placeholder(true);
  const int64_t id = CreateGroup(name, parent ? parent->color : color, {empty}, parent_id);
  if (placeholder) *placeholder = empty;
  return id;
}

int64_t Workspace::CreateGroup(const std::wstring& name, uint32_t color,
                               const std::vector<Tab*>& tabs, int64_t parent_id) {
  TabGroupRecord g;
  g.workspace_id = record_.id;
  g.name = name;
  g.color = color;
  g.parent_id = FindGroup(parent_id) ? parent_id : 0;
  if (profile_->is_private()) {
    g.id = next_session_group_id_--;
  } else {
    g.id = Application::Get()->store().InsertTabGroup(g);
    if (!g.id) return 0;
  }
  groups_.push_back(g);
  for (Tab* t : tabs) SetTabGroup(t, g.id);
  return g.id;
}

void Workspace::SetTabGroup(Tab* tab, int64_t group_id) {
  if (IndexOf(tab) < 0) return;
  if (tab->pinned() || (group_id && !FindGroup(group_id))) group_id = 0;
  if (tab->record().group_id != group_id) {
    tab->set_group(group_id);
    const std::wstring& live = tab->record().live_item;
    if (!live.empty() && !InGroup(tab, _wtoi64(live.substr(0, live.find(L':')).c_str()))) {
      Application::Get()->live_folders().OnTabDismissed(tab);
    }
    if (!profile_->is_private() && tab->id()) {
      Application::Get()->store().UpdateTab(tab->record());
    }
  }
  NormalizeGroups();
  PruneEmptyGroups();
  PersistOrder();
}

void Workspace::UpdateGroup(const TabGroupRecord& group) {
  for (auto& g : groups_) {
    if (g.id != group.id) continue;
    g.name = group.name;
    g.color = group.color;
    g.collapsed = group.collapsed;
    g.icon = group.icon;
    if (!profile_->is_private()) Application::Get()->store().UpdateTabGroup(g);
  }
}

void Workspace::Ungroup(int64_t group_id) {
  // unpackTabs: what was in the folder moves into its parent folder.
  Application::Get()->live_folders().OnFolderRemoved(group_id);
  const TabGroupRecord* g = FindGroup(group_id);
  const int64_t parent = g ? g->parent_id : 0;
  for (const auto& t : tabs_) {
    if (t->record().group_id == group_id) {
      t->set_group(parent);
      if (!profile_->is_private() && t->id()) Application::Get()->store().UpdateTab(t->record());
    }
  }
  for (auto& child : groups_) {
    if (child.parent_id == group_id) {
      child.parent_id = parent;
      if (!profile_->is_private()) Application::Get()->store().UpdateTabGroup(child);
    }
  }
  std::erase_if(groups_, [&](const TabGroupRecord& x) { return x.id == group_id; });
  if (!profile_->is_private()) Application::Get()->store().DeleteTabGroup(group_id);
}

void Workspace::PruneEmptyGroups() {
  // A folder lives while anything (a placeholder included) is in its subtree.
  std::vector<int64_t> empty;
  for (const auto& g : groups_) {
    const bool used = std::any_of(tabs_.begin(), tabs_.end(),
                                  [&](const auto& t) { return InGroup(t.get(), g.id); });
    if (!used) empty.push_back(g.id);
  }
  for (int64_t id : empty) Ungroup(id);
}

void Workspace::NormalizeGroups() {
  // Pinned tabs never belong to a group.
  for (const auto& t : tabs_) {
    if (t->pinned() && t->record().group_id) t->set_group(0);
  }
  // A split lives in the folder of its first member.
  for (const SplitView& split : splits_) {
    Tab* first = nullptr;
    for (const auto& t : tabs_) {
      if (SplitOf(t.get()) == &split) {
        if (!first) {
          first = t.get();
        } else if (t->record().group_id != first->record().group_id) {
          t->set_group(first->record().group_id);
          if (!profile_->is_private() && t->id()) {
            Application::Get()->store().UpdateTab(t->record());
          }
        }
      }
    }
  }
  // Every folder's subtree (its tabs and its subfolders') follows its first
  // item, recursively by depth, keeping the order otherwise.
  std::vector<std::vector<int64_t>> chains;
  chains.reserve(tabs_.size());
  for (const auto& t : tabs_) chains.push_back(GroupChain(t.get()));
  std::vector<size_t> order(tabs_.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::function<void(std::vector<size_t>&, size_t)> arrange = [&](std::vector<size_t>& items,
                                                                   size_t depth) {
    std::vector<size_t> out;
    std::vector<bool> taken(items.size(), false);
    for (size_t a = 0; a < items.size(); ++a) {
      if (taken[a]) continue;
      const auto& chain = chains[items[a]];
      if (chain.size() <= depth) {
        taken[a] = true;
        out.push_back(items[a]);
        continue;
      }
      const int64_t folder = chain[depth];
      std::vector<size_t> sub;
      for (size_t b = a; b < items.size(); ++b) {
        if (!taken[b] && chains[items[b]].size() > depth && chains[items[b]][depth] == folder) {
          taken[b] = true;
          sub.push_back(items[b]);
        }
      }
      arrange(sub, depth + 1);
      out.insert(out.end(), sub.begin(), sub.end());
    }
    items = std::move(out);
  };
  arrange(order, 0);
  std::vector<std::unique_ptr<Tab>> ordered;
  ordered.reserve(tabs_.size());
  for (size_t i : order) ordered.push_back(std::move(tabs_[i]));
  tabs_ = std::move(ordered);
  // Then every split's members behind its first one (they share a folder,
  // so this keeps folders contiguous).
  if (splits_.empty()) return;
  ordered.clear();
  std::vector<bool> taken(tabs_.size(), false);
  for (size_t i = 0; i < tabs_.size(); ++i) {
    if (taken[i]) continue;
    const SplitView* split = SplitOf(tabs_[i].get());
    if (!split) {
      taken[i] = true;
      ordered.push_back(std::move(tabs_[i]));
      continue;
    }
    for (size_t j = i; j < tabs_.size(); ++j) {
      if (!taken[j] && tabs_[j] && SplitOf(tabs_[j].get()) == split) {
        taken[j] = true;
        ordered.push_back(std::move(tabs_[j]));
      }
    }
  }
  tabs_ = std::move(ordered);
}

const Workspace::SplitView* Workspace::SplitOf(const Tab* tab) const {
  if (!tab) return nullptr;
  const int64_t key = SplitKey(tab);
  for (const SplitView& split : splits_) {
    for (int64_t k : SplitKeys(split.root)) {
      if (k == key) return &split;
    }
  }
  return nullptr;
}

Workspace::SplitView* Workspace::FindSplit(int64_t id) {
  for (SplitView& split : splits_) {
    if (split.id == id) return &split;
  }
  return nullptr;
}

Tab* Workspace::SplitTab(int64_t key) const {
  for (const auto& t : tabs_) {
    if (SplitKey(t.get()) == key) return t.get();
  }
  return nullptr;
}

std::vector<Tab*> Workspace::SplitMembers(const SplitView& split) const {
  std::vector<Tab*> out;
  for (int64_t key : SplitKeys(split.root)) {
    if (Tab* t = SplitTab(key)) out.push_back(t);
  }
  return out;
}

void Workspace::StoreSplit(SplitView& split) {
  if (profile_->is_private()) return;
  // On disk the leaves are tab ids.
  SplitNode stored = split.root;
  std::vector<int64_t> keys = SplitKeys(split.root);
  std::vector<int64_t> ids;
  for (int64_t key : keys) {
    const Tab* t = SplitTab(key);
    ids.push_back(t ? t->id() : 0);
  }
  ReplaceSplitKeys(stored, keys, ids);
  SplitViewRecord rec{split.id, record_.id, SerializeSplit(stored)};
  Store& store = Application::Get()->store();
  if (split.id > 0) {
    store.UpdateSplitView(rec);
  } else if (split.id == 0) {
    split.id = store.InsertSplitView(rec);
  }
}

int64_t Workspace::CreateSplit(const std::vector<Tab*>& tabs, SplitGrid grid) {
  std::vector<Tab*> members;
  for (Tab* t : tabs) {
    if (t && IndexOf(t) >= 0 && !t->pinned() &&
        std::find(members.begin(), members.end(), t) == members.end()) {
      members.push_back(t);
    }
  }
  if (members.size() < 2 || members.size() > static_cast<size_t>(kMaxSplitTabs)) return 0;
  for (Tab* t : members) RemoveFromSplit(t);
  // The members join the first one: same folder, right after it.
  const int64_t group = members[0]->record().group_id;
  for (size_t i = 1; i < members.size(); ++i) {
    Tab* t = members[i];
    if (t->record().group_id != group) {
      t->set_group(group);
      if (!profile_->is_private() && t->id()) Application::Get()->store().UpdateTab(t->record());
    }
    const int from = IndexOf(t);
    std::unique_ptr<Tab> owned = std::move(tabs_[static_cast<size_t>(from)]);
    tabs_.erase(tabs_.begin() + from);
    tabs_.insert(tabs_.begin() + IndexOf(members[i - 1]) + 1, std::move(owned));
  }
  std::vector<int64_t> keys;
  for (Tab* t : members) keys.push_back(SplitKey(t));
  SplitView split;
  split.root = BuildSplitTree(keys, grid);
  if (profile_->is_private()) split.id = next_session_split_id_--;
  StoreSplit(split);
  if (split.id == 0) return 0;
  const int64_t id = split.id;
  splits_.push_back(std::move(split));
  NormalizeGroups();
  PruneEmptyGroups();
  PersistOrder();
  return id;
}

bool Workspace::AddToSplit(int64_t split_id, Tab* tab) {
  SplitView* split = FindSplit(split_id);
  if (!split || !tab || tab->pinned() || IndexOf(tab) < 0 || SplitOf(tab) == split) return false;
  const std::vector<int64_t> keys = SplitKeys(split->root);
  if (keys.size() >= static_cast<size_t>(kMaxSplitTabs)) return false;
  RemoveFromSplit(tab);
  split = FindSplit(split_id);
  if (!split) return false;
  // A new pane at the end of the top level, all panes there equal.
  SplitNode leaf;
  leaf.key = SplitKey(tab);
  split->root.children.push_back(std::move(leaf));
  const float share = 100.f / static_cast<float>(split->root.children.size());
  for (auto& child : split->root.children) child.size = share;
  StoreSplit(*split);
  NormalizeGroups();
  PruneEmptyGroups();
  PersistOrder();
  return true;
}

void Workspace::RelayoutSplit(int64_t split_id, SplitGrid grid) {
  SplitView* split = FindSplit(split_id);
  if (!split) return;
  split->root = BuildSplitTree(SplitKeys(split->root), grid);
  StoreSplit(*split);
}

void Workspace::RemoveFromSplit(Tab* tab) {
  const SplitView* found = SplitOf(tab);
  if (!found) return;
  SplitView* split = FindSplit(found->id);
  RemoveSplitKey(split->root, SplitKey(tab));
  if (split->root.leaf()) {
    RemoveSplit(split->id);
  } else {
    StoreSplit(*split);
  }
}

void Workspace::RemoveSplit(int64_t split_id) {
  if (split_id > 0 && !profile_->is_private()) {
    Application::Get()->store().DeleteSplitView(split_id);
  }
  std::erase_if(splits_, [&](const SplitView& s) { return s.id == split_id; });
}

void Workspace::MoveSplit(int64_t split_id, int new_index) {
  const SplitView* split = FindSplit(split_id);
  if (!split) return;
  std::vector<std::unique_ptr<Tab>> block;
  for (size_t i = 0; i < tabs_.size();) {
    if (SplitOf(tabs_[i].get()) == split) {
      block.push_back(std::move(tabs_[i]));
      tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      ++i;
    }
  }
  new_index = std::clamp(new_index, PinnedCount(), static_cast<int>(tabs_.size()));
  for (size_t i = 0; i < block.size(); ++i) {
    tabs_.insert(tabs_.begin() + new_index + static_cast<std::ptrdiff_t>(i), std::move(block[i]));
  }
  NormalizeGroups();
  PruneEmptyGroups();
  PersistOrder();
}

void Workspace::SaveSplit(int64_t split_id) {
  if (SplitView* split = FindSplit(split_id)) StoreSplit(*split);
}

void Workspace::LoadSplits(Store& store) {
  for (const SplitViewRecord& rec : store.LoadSplitViews(record_.id)) {
    SplitNode root;
    bool ok = ParseSplit(rec.layout, &root);
    if (ok) {
      // Tab ids back to tabs; tabs that are gone (or pinned) leave the split.
      std::vector<int64_t> ids = SplitKeys(root);
      std::vector<int64_t> keys;
      for (int64_t id : ids) {
        const Tab* found = nullptr;
        for (const auto& t : tabs_) {
          if (t->id() == id && !t->pinned() && !SplitOf(t.get())) found = t.get();
        }
        keys.push_back(found ? SplitKey(found) : 0);
      }
      ReplaceSplitKeys(root, ids, keys);
      while (!root.leaf() && RemoveSplitKey(root, 0)) {
      }
      ok = !root.leaf() && SplitKeys(root).size() >= 2;
    }
    if (!ok) {
      store.DeleteSplitView(rec.id);
      continue;
    }
    SplitView split;
    split.id = rec.id;
    split.root = std::move(root);
    splits_.push_back(std::move(split));
    StoreSplit(splits_.back());  // members that left are gone from the row too
  }
  if (!splits_.empty()) NormalizeGroups();
}

void Workspace::MoveTab(Tab* tab, int new_index) {
  const int from = IndexOf(tab);
  if (from < 0) return;
  std::unique_ptr<Tab> owned = std::move(tabs_[static_cast<size_t>(from)]);
  tabs_.erase(tabs_.begin() + from);
  const int pinned = PinnedCount();
  const int lo = owned->pinned() ? 0 : pinned;
  const int hi = owned->pinned() ? pinned : static_cast<int>(tabs_.size());
  new_index = std::clamp(new_index, lo, hi);
  tabs_.insert(tabs_.begin() + new_index, std::move(owned));
  NormalizeGroups();
  PruneEmptyGroups();
  PersistOrder();
}

void Workspace::AppendLoadedTab(std::unique_ptr<Tab> tab) {
  tab->set_workspace(this);
  tabs_.push_back(std::move(tab));
}

void Workspace::PersistOrder() {
  if (profile_->is_private()) return;
  std::vector<int64_t> ids;
  ids.reserve(tabs_.size());
  for (size_t i = 0; i < tabs_.size(); ++i) {
    ids.push_back(tabs_[i]->id());
    tabs_[i]->set_position(static_cast<int>(i));  // stored below; not dirty
  }
  Application::Get()->store().SetTabOrder(ids);
}

Profile::Profile(ProfileRecord record, std::filesystem::path data_dir, bool is_private)
    : record_(std::move(record)), data_dir_(std::move(data_dir)), is_private_(is_private) {}

Profile::~Profile() = default;

CefRefPtr<CefRequestContext> Profile::request_context() {
  if (!context_) {
    CefRequestContextSettings settings;
    if (!is_private_) {
      // Must be a child of CefSettings.root_cache_path; one directory per
      // profile gives Chromium-level isolation of all website data.
      CefString(&settings.cache_path) = data_dir_.wstring();
    }
    settings.persist_session_cookies = false;
    context_ = CefRequestContext::CreateContext(settings, nullptr);
    if (!is_private_ && context_->GetCachePath().ToWString() != data_dir_.wstring()) {
      // CEF falls back to an in-memory profile if it rejects the path. That
      // would silently lose all site data, so make it loud.
      ORBIT_LOG_ERROR("profile %lld: Chromium rejected its storage path; data will NOT persist",
                       record_.id);
    }
    // Chrome style gives a page that paints no background (plain text, JSON,
    // raw HTML) the dark browser theme's base colour: black text on dark.
    // Chrome shows white, so the base stays light; what pages are told to
    // prefer comes from Orbit's theme (Tab::ApplyWebColorScheme).
    context_->SetChromeColorScheme(CEF_COLOR_VARIANT_LIGHT, 0);
    RegisterInternalSchemeHandler(context_, record_.id, is_private_);
    ORBIT_LOG_INFO("created request context for profile %lld (private=%d)", record_.id,
                    is_private_ ? 1 : 0);
  }
  return context_;
}

Workspace* Profile::FindWorkspace(int64_t id) const {
  for (const auto* list : {&workspaces_, &linked_}) {
    for (const auto& w : *list) {
      if (w->id() == id) return w.get();
    }
  }
  return nullptr;
}

Workspace* Profile::AddWorkspace(std::unique_ptr<Workspace> workspace) {
  auto& list = workspace->is_linked() ? linked_ : workspaces_;
  list.push_back(std::move(workspace));
  return list.back().get();
}

std::unique_ptr<Workspace> Profile::RemoveWorkspace(Workspace* workspace) {
  for (auto* list : {&workspaces_, &linked_}) {
    for (auto it = list->begin(); it != list->end(); ++it) {
      if (it->get() == workspace) {
        std::unique_ptr<Workspace> out = std::move(*it);
        list->erase(it);
        return out;
      }
    }
  }
  return nullptr;
}

void Profile::SortWorkspaces() {
  std::stable_sort(workspaces_.begin(), workspaces_.end(), [](const auto& a, const auto& b) {
    return a->record().position < b->record().position;
  });
}

void Profile::MoveWorkspace(Workspace* workspace, int new_index) {
  auto owned = RemoveWorkspace(workspace);
  if (!owned) return;
  new_index = std::clamp(new_index, 0, static_cast<int>(workspaces_.size()));
  workspaces_.insert(workspaces_.begin() + new_index, std::move(owned));
  if (is_private_) return;
  std::vector<int64_t> ids;
  for (size_t i = 0; i < workspaces_.size(); ++i) {
    workspaces_[i]->mutable_record().position = static_cast<int>(i);
    ids.push_back(workspaces_[i]->id());
  }
  Application::Get()->store().SetWorkspaceOrder(ids);
}

std::vector<Workspace*> Profile::containers() const {
  std::vector<Workspace*> out;
  for (const auto& ws : workspaces_) {
    out.push_back(ws.get());
    if (ws->essentials()) out.push_back(ws->essentials());
  }
  for (const auto& ws : linked_) {
    out.push_back(ws.get());
    if (ws->essentials()) out.push_back(ws->essentials());
  }
  return out;
}

void Profile::LoadFromStore(Store& store) {
  workspaces_.clear();
  linked_.clear();
  std::vector<std::unique_ptr<Workspace>> grids;
  for (auto& wr : store.LoadWorkspaces(record_.id)) {
    auto ws = std::make_unique<Workspace>(this, wr);
    for (auto& g : store.LoadTabGroups(wr.id)) ws->AppendLoadedGroup(std::move(g));
    auto tabs = store.LoadTabs(wr.id);
    // Stored order is authoritative, but enforce pinned-first.
    std::stable_sort(tabs.begin(), tabs.end(),
                     [](const TabRecord& a, const TabRecord& b) { return a.pinned > b.pinned; });
    Workspace* raw = ws.get();
    if (wr.kind == WorkspaceRecord::kKindEssentials) {
      grids.push_back(std::move(ws));  // not a space: its owner adopts it below
    } else {
      AddWorkspace(std::move(ws));
    }
    // zen.pinned-tab-manager.restore-pinned-tabs-to-pinned-url (default off).
    const bool restore_pinned =
        store.GetPref(Store::kGlobalScope, L"restore_pinned_to_pinned_url") == L"1";
    for (auto& tr : tabs) {
      if (tr.pinned && tr.pinned_url.empty()) tr.pinned_url = tr.url;  // pre-v4 pins
      if (tr.pinned && restore_pinned && tr.url != tr.pinned_url) {
        tr.url = tr.pinned_url;
        tr.title.clear();
      }
      raw->AppendLoadedTab(std::make_unique<Tab>(raw, tr));
    }
    raw->LoadSplits(store);
    // An empty subfolder's hidden empty tab (ZenFolders keeps one in it).
    for (const auto& t : raw->tabs()) {
      if (t->record().group_id && !t->pinned() && t->record().custom_title.empty() &&
          t->url() == Application::kNewTabUrl) {
        t->set_empty_placeholder(true);
      }
    }
    // Zen restores `zen-empty-tab` (TabState zenIsEmpty). With replace-newtab
    // a lone, unpinned new-tab page can only be the empty-space placeholder,
    // so it comes back hidden instead of as a "New Tab" row.
    if (wr.kind != WorkspaceRecord::kKindEssentials && raw->tabs().size() == 1) {
      Tab* only = raw->tabs().front().get();
      if (!only->pinned() && only->record().custom_title.empty() &&
          only->url() == Application::kNewTabUrl) {
        only->set_empty_placeholder(true);
      }
    }
  }
  // A linked space whose parent is gone became an ordinary space (its
  // parent_id was cleared by ON DELETE SET NULL).
  for (size_t i = 0; i < linked_.size();) {
    Workspace* ws = linked_[i].get();
    Workspace* parent = FindWorkspace(ws->record().parent_id);
    if (parent && !parent->is_linked()) {
      ws->set_parent(parent);
      ++i;
      continue;
    }
    std::unique_ptr<Workspace> owned = RemoveWorkspace(ws);
    owned->mutable_record().kind = WorkspaceRecord::kKindSpace;
    owned->mutable_record().parent_id = 0;
    owned->mutable_record().position = 1 << 30;  // last; Application renumbers the order
    store.UpdateWorkspace(owned->record());
    workspaces_.push_back(std::move(owned));
  }
  // Each Essentials grid goes to its space; a grid without one is dropped.
  for (auto& grid : grids) {
    Workspace* owner = FindWorkspace(grid->record().owner_id);
    if (!owner || owner->essentials()) {
      store.DeleteWorkspace(grid->id());
      continue;
    }
    owner->AdoptEssentials(std::move(grid));
  }
}

bool Profile::HasLiveBrowsers() const {
  for (Workspace* ws : containers()) {
    for (const auto& t : ws->tabs()) {
      if (t->load_state() != Tab::LoadState::kUnloaded) return true;
    }
  }
  return false;
}

}  // namespace orbit
