#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "include/cef_download_handler.h"
#include "persistence/store.h"

namespace orbit {

class Profile;

// Tracks downloads using Chromium's real download events. Persistent profiles
// record downloads in the database; private profiles keep them in memory for
// the session only. UI thread only.
class DownloadManager {
 public:
  struct Item {
    DownloadRecord record;  // record.id: database id (> 0) or session id (< 0)
    bool is_private = false;
    int64_t speed = 0;      // bytes/s while in progress
    int percent = -1;
  };

  DownloadManager() = default;

  // CefDownloadHandler hooks (from BrowserClient). |save_as|: the user chose
  // "Save link/image/video as" for it, so Orbit asks where to save (as with
  // the "always ask" setting) in a dialog owned by |owner|. Returns false
  // when the download waits for that dialog.
  bool OnBeforeDownload(Profile* profile, CefRefPtr<CefDownloadItem> item,
                        const CefString& suggested_name,
                        CefRefPtr<CefBeforeDownloadCallback> callback, bool save_as, HWND owner);
  void OnDownloadUpdated(CefRefPtr<CefDownloadItem> item,
                         CefRefPtr<CefDownloadItemCallback> callback);

  // Newest first: live downloads plus stored history for |profile|.
  std::vector<Item> List(Profile* profile);
  int ActiveCount(const Profile* profile) const;
  // Aggregate progress of active downloads (0-100, -1 if unknown).
  int ActivePercent(const Profile* profile) const;

  bool Pause(Profile* profile, int64_t id);
  bool Resume(Profile* profile, int64_t id);
  bool Cancel(Profile* profile, int64_t id);
  bool Open(Profile* profile, int64_t id, HWND owner);
  bool ShowInFolder(Profile* profile, int64_t id);
  bool Remove(Profile* profile, int64_t id);  // forget (does not delete the file)

  // Forgets a private profile's session downloads (window closed).
  void ForgetProfile(const Profile* profile);

  static std::filesystem::path DefaultDirectory();
  std::filesystem::path Directory() const;
  // Returns a path in |dir| that no existing file or in-flight download uses.
  std::filesystem::path UniquePath(const std::filesystem::path& dir,
                                   const std::wstring& file_name) const;
  // A download is written to "<name>.crdownload" and renamed to its name when
  // it completes, so an unfinished file never looks finished.
  static constexpr wchar_t kPartialSuffix[] = L".crdownload";

  // Self-tests: answers the "Save as" dialog (nullopt: cancelled).
  static inline std::function<std::optional<std::filesystem::path>(const std::filesystem::path& suggested)>
      test_choose_path;

 private:
  struct Live {
    const Profile* profile = nullptr;
    Item item;  // item.record.path: the finished file's path
    std::filesystem::path partial;  // where Chromium writes until it completes
    bool chosen = false;            // the user picked the path (may replace a file)
    CefRefPtr<CefDownloadItemCallback> control;
    ULONGLONG last_persist = 0;
  };
  // What a download needs from Chromium's item to start once a path is known.
  struct Pending {
    uint32_t cef_id = 0;
    std::wstring url;
    std::wstring mime_type;
    int64_t total_bytes = 0;
  };
  void Start(Profile* profile, const Pending& pending, const std::filesystem::path& path, bool chosen,
             CefRefPtr<CefBeforeDownloadCallback> callback);
  bool MovePartialToFinal(Live& live);
  struct PrivateDone {
    const Profile* profile = nullptr;
    Item item;
  };
  Live* FindLive(const Profile* profile, int64_t id);
  std::optional<Item> FindItem(Profile* profile, int64_t id);
  void Persist(Live& live);
  void OnFinished(Live& live);
  void NotifyWindows(const Profile* profile);

  std::map<uint32_t, Live> live_;  // keyed by Chromium download id
  std::vector<PrivateDone> private_done_;  // finished private downloads (session only)
  int64_t next_session_id_ = -1;
};

// Ensures a completed download carries the Windows Mark-of-the-Web so that
// SmartScreen / Office Protected View apply when it is opened. Returns true if
// the stream was already present or was written.
bool EnsureMarkOfTheWeb(const std::filesystem::path& file, const std::wstring& source_url,
                        bool is_private);

// True for file types that run code when opened (confirm before opening).
bool IsExecutableFileType(const std::filesystem::path& file);

}  // namespace orbit
