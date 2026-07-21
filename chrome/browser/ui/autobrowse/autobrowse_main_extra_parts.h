// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_MAIN_EXTRA_PARTS_H_
#define CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_MAIN_EXTRA_PARTS_H_

#include <memory>

#include "chrome/browser/chrome_browser_main_extra_parts.h"

namespace autobrowse {

class AutobrowseSearchRunner;
class AutobrowseFetchRunner;
class AutobrowseScrapeRunner;
class AutobrowseMonitorRunner;
class AutobrowseSessionInfoRunner;
class AutobrowseWarmupRunner;

// Browser-process entry point for the native automation feature (the Chromium
// port of the Obscura command surface). When --autobrowse-search is present it
// launches a human-like search once the browser is up, and quiets background
// service log noise so the JSON output on stdout stays clean.
class AutobrowseMainExtraParts : public ChromeBrowserMainExtraParts {
 public:
  AutobrowseMainExtraParts();

  AutobrowseMainExtraParts(const AutobrowseMainExtraParts&) = delete;
  AutobrowseMainExtraParts& operator=(const AutobrowseMainExtraParts&) = delete;

  ~AutobrowseMainExtraParts() override;

 private:
  // ChromeBrowserMainExtraParts:
  void PostEarlyInitialization() override;
  void PostBrowserStart() override;

  void StartSearch();
  void StartFetch();
  void StartScrape();
  void StartMonitor();
  void StartSessionInfo();
  void StartWarmup();

  std::unique_ptr<AutobrowseSearchRunner> search_runner_;
  std::unique_ptr<AutobrowseFetchRunner> fetch_runner_;
  std::unique_ptr<AutobrowseScrapeRunner> scrape_runner_;
  std::unique_ptr<AutobrowseMonitorRunner> monitor_runner_;
  std::unique_ptr<AutobrowseSessionInfoRunner> session_info_runner_;
  std::unique_ptr<AutobrowseWarmupRunner> warmup_runner_;
};

}  // namespace autobrowse

#endif  // CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_MAIN_EXTRA_PARTS_H_
