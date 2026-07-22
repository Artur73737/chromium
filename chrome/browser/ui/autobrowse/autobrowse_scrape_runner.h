// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SCRAPE_RUNNER_H_
#define CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SCRAPE_RUNNER_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/values.h"

namespace content {
class BrowserContext;
}

namespace autobrowse {

// Scrapes many URLs **in parallel** inside a single process: it opens up to
// `concurrency` off-screen tabs at once, each navigating to a URL, letting the
// page render, and evaluating --eval (or dumping text/html/links with --dump).
// Results are collected in the original URL order and emitted as a JSON array to
// stdout or a file. A real navigation + DOM read per URL, never an API.
class AutobrowseScrapeRunner {
 public:
  AutobrowseScrapeRunner(std::vector<std::string> urls,
                         std::string dump,
                         std::string eval,
                         int extra_wait_seconds,
                         int timeout_seconds,
                         int concurrency,
                         base::FilePath output_path);

  AutobrowseScrapeRunner(const AutobrowseScrapeRunner&) = delete;
  AutobrowseScrapeRunner& operator=(const AutobrowseScrapeRunner&) = delete;

  ~AutobrowseScrapeRunner();

  void Start();

  // Server mode: deliver the JSON array here instead of printing/exiting.
  void SetCompletionCallback(base::OnceCallback<void(std::string)> cb) {
    on_complete_ = std::move(cb);
  }

 private:
  // One off-screen tab scraping one URL; defined in the .cc.
  class Tab;

  std::string BuildExtractScript() const;
  content::BrowserContext* GetContext();
  void LaunchMore();
  void OnTabDone(size_t index, base::Value result);
  void EraseTabAndContinue(size_t index);
  void Finish();
  void MaybeExit();

  const std::vector<std::string> urls_;
  const std::string dump_;
  const std::string eval_;
  const int extra_wait_seconds_;
  const int timeout_seconds_;
  const int concurrency_;
  const base::FilePath output_path_;
  base::OnceCallback<void(std::string)> on_complete_;

  std::vector<base::Value> results_;  // one slot per URL, in order
  size_t next_index_ = 0;             // next URL to launch
  size_t completed_ = 0;
  bool finished_ = false;
  base::TimeTicks start_time_;
  std::map<size_t, std::unique_ptr<Tab>> tabs_;  // live tabs, keyed by URL index

  base::WeakPtrFactory<AutobrowseScrapeRunner> weak_factory_{this};
};

}  // namespace autobrowse

#endif  // CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SCRAPE_RUNNER_H_
