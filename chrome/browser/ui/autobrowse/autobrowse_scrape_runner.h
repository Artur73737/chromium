// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SCRAPE_RUNNER_H_
#define CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SCRAPE_RUNNER_H_

#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "content/public/browser/web_contents_observer.h"

namespace autobrowse {

// Scrapes many URLs in sequence inside the browser's active tab: for each URL it
// navigates, lets the page render, evaluates --ab-eval (or dumps text/html/links
// with --ab-dump), and collects one record per URL. Emits a JSON array to stdout
// or a file. Mirrors the Obscura `scrape` command (many pages, one eval each).
// A real navigation + DOM read per URL, never an API.
class AutobrowseScrapeRunner : public content::WebContentsObserver {
 public:
  AutobrowseScrapeRunner(std::vector<std::string> urls,
                         std::string dump,
                         std::string eval,
                         int extra_wait_seconds,
                         int timeout_seconds,
                         base::FilePath output_path);

  AutobrowseScrapeRunner(const AutobrowseScrapeRunner&) = delete;
  AutobrowseScrapeRunner& operator=(const AutobrowseScrapeRunner&) = delete;

  ~AutobrowseScrapeRunner() override;

  void Start();

  // Server mode: deliver the JSON array here instead of printing/exiting.
  void SetCompletionCallback(base::OnceCallback<void(std::string)> cb) {
    on_complete_ = std::move(cb);
  }

 private:
  // content::WebContentsObserver:
  void DocumentOnLoadCompletedInPrimaryMainFrame() override;

  std::string BuildExtractScript() const;

  void TryAttach();
  void NavigateCurrent();
  void OnLoaded();
  void RunExtract();
  void OnExtractResult(base::Value value);
  void AdvanceOrFinish();
  void Finish();
  void MaybeExit();
  void OnItemTimeout();

  base::OnceCallback<void(std::string)> on_complete_;

  const std::vector<std::string> urls_;
  const std::string dump_;
  const std::string eval_;
  const int extra_wait_seconds_;
  const int timeout_seconds_;
  const base::FilePath output_path_;

  size_t index_ = 0;
  bool attached_ = false;
  bool extracted_current_ = false;
  bool finished_ = false;
  base::ListValue results_;

  base::TimeTicks start_time_;
  base::RepeatingTimer attach_timer_;
  base::OneShotTimer item_deadline_timer_;
  base::OneShotTimer settle_timer_;

  base::WeakPtrFactory<AutobrowseScrapeRunner> weak_factory_{this};
};

}  // namespace autobrowse

#endif  // CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SCRAPE_RUNNER_H_
