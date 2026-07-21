// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SEARCH_RUNNER_H_
#define CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SEARCH_RUNNER_H_

#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "content/public/browser/web_contents_observer.h"

namespace autobrowse {

// Drives a human-like web search inside the browser's visible active tab:
// navigates it to the engine home page, types the query letter-by-letter into
// the search box, submits with Enter, waits for the SERP, and collects the top
// result links (never via a search API). The final result is emitted as JSON
// (Obscura-compatible shape) to stdout or to a file, and the browser exits.
// The exact same flow runs headless, just without a visible window.
class AutobrowseSearchRunner : public content::WebContentsObserver {
 public:
  AutobrowseSearchRunner(std::string engine,
                         std::string query,
                         int max_results,
                         base::FilePath output_path);

  AutobrowseSearchRunner(const AutobrowseSearchRunner&) = delete;
  AutobrowseSearchRunner& operator=(const AutobrowseSearchRunner&) = delete;

  ~AutobrowseSearchRunner() override;

  // When set, the final JSON is delivered here and the process is NOT exited
  // (server mode). When unset, the runner prints/writes the result and exits in
  // headless (CLI mode).
  void SetCompletionCallback(base::OnceCallback<void(std::string)> cb) {
    on_complete_ = std::move(cb);
  }

  // Optional: after collecting the SERP links, open each result and scrape its
  // content, attaching it as a "scraped" field on each result. `kind` is
  // "text", "html", or "links" (empty = don't scrape, the default).
  void SetScrape(std::string kind) { scrape_kind_ = std::move(kind); }

  // Finds the active tab (retrying until one exists) and starts navigation.
  void Start();

 private:
  // content::WebContentsObserver:
  void DocumentOnLoadCompletedInPrimaryMainFrame() override;

  std::string StartUrl() const;
  std::string BuildDriverScript() const;

  void TryAttachAndNavigate();
  void DoNavigate();
  void EnsurePolling();
  void RunDriver();
  void OnDriverResult(base::Value value);
  void Finish(const std::string& json);
  void MaybeExit();
  void OnTimeout();

  // Scrape phase (only when SetScrape was called with a non-empty kind).
  void MaybeScrapeThenFinish(const std::string& final_json);
  void NavigateToScrapeTarget();
  void ExtractScrapeContent();
  void OnScrapeContent(base::Value value);
  void OnScrapeItemTimeout();
  std::string BuildScrapeScript() const;
  void FinishScrape();

  base::OnceCallback<void(std::string)> on_complete_;

  const std::string engine_;
  const std::string query_;
  const int max_results_;
  const base::FilePath output_path_;
  std::string scrape_kind_;

  // Scrape-phase state.
  bool scraping_ = false;
  bool scrape_extracted_ = false;
  size_t scrape_index_ = 0;
  base::Value pending_result_;  // parsed final JSON dict, mutated with scrapes
  base::OneShotTimer scrape_settle_timer_;
  base::OneShotTimer scrape_item_timer_;

  base::TimeTicks start_time_;
  base::RepeatingTimer attach_timer_;
  base::RepeatingTimer poll_timer_;
  base::OneShotTimer deadline_timer_;
  bool navigated_ = false;
  bool polling_started_ = false;
  bool finished_ = false;

  base::WeakPtrFactory<AutobrowseSearchRunner> weak_factory_{this};
};

}  // namespace autobrowse

#endif  // CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SEARCH_RUNNER_H_
