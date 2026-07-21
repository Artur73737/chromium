// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/autobrowse/autobrowse_main_extra_parts.h"

#include <string>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "chrome/browser/ui/autobrowse/autobrowse_fetch_runner.h"
#include "chrome/browser/ui/autobrowse/autobrowse_monitor_runner.h"
#include "chrome/browser/ui/autobrowse/autobrowse_scrape_runner.h"
#include "chrome/browser/ui/autobrowse/autobrowse_search_runner.h"
#include "chrome/browser/ui/autobrowse/autobrowse_session_info_runner.h"
#include "chrome/browser/ui/autobrowse/autobrowse_warmup_runner.h"
#include "chrome/browser/ui/autobrowse/autobrowse_switches.h"

namespace autobrowse {

namespace {

// The selected sub-command, honoring both the single-entrypoint --autobrowse=CMD
// switch and the legacy --autobrowse-search=QUERY shortcut. Returns "" if
// autobrowse is not active.
std::string SelectedCommand() {
  const base::CommandLine& cmd = *base::CommandLine::ForCurrentProcess();
  if (cmd.HasSwitch(switches::kAutobrowseSearch)) {
    return "search";
  }
  if (cmd.HasSwitch(switches::kAutobrowse)) {
    std::string v = cmd.GetSwitchValueASCII(switches::kAutobrowse);
    return v.empty() ? "search" : v;
  }
  return std::string();
}

int GetIntSwitch(const char* name, int fallback) {
  const base::CommandLine& cmd = *base::CommandLine::ForCurrentProcess();
  if (cmd.HasSwitch(name)) {
    int parsed = 0;
    if (base::StringToInt(cmd.GetSwitchValueASCII(name), &parsed) && parsed > 0) {
      return parsed;
    }
  }
  return fallback;
}

}  // namespace

AutobrowseMainExtraParts::AutobrowseMainExtraParts() = default;

AutobrowseMainExtraParts::~AutobrowseMainExtraParts() = default;

void AutobrowseMainExtraParts::PostEarlyInitialization() {
  // In autobrowse mode the wanted output is only the JSON/dump on stdout, so
  // suppress the background-service log noise a keyless developer build emits.
  if (!SelectedCommand().empty() && !getenv("AB_DEBUG")) {
    logging::SetMinLogLevel(logging::LOGGING_FATAL);
  }
}

void AutobrowseMainExtraParts::PostBrowserStart() {
  const std::string command = SelectedCommand();
  if (command.empty()) {
    return;
  }
  if (command == "search") {
    StartSearch();
  } else if (command == "fetch") {
    StartFetch();
  } else if (command == "scrape") {
    StartScrape();
  } else if (command == "monitor") {
    StartMonitor();
  } else if (command == "session-info") {
    StartSessionInfo();
  } else if (command == "warmup") {
    StartWarmup();
  } else {
    fprintf(stderr, "[autobrowse] unknown command: %s\n", command.c_str());
  }
}

void AutobrowseMainExtraParts::StartSearch() {
  const base::CommandLine& cmd = *base::CommandLine::ForCurrentProcess();

  // Query: legacy --autobrowse-search=Q wins, else --ab-query.
  std::string query = cmd.GetSwitchValueASCII(switches::kAutobrowseSearch);
  if (query.empty()) {
    query = cmd.GetSwitchValueASCII(switches::kAbQuery);
  }

  // Engine: --autobrowse-engine (legacy) or --ab-engine, default google.
  std::string engine = cmd.GetSwitchValueASCII(switches::kAutobrowseEngine);
  if (engine.empty()) {
    engine = cmd.GetSwitchValueASCII(switches::kAbEngine);
  }
  if (engine.empty()) {
    engine = "google";
  }

  int max_results = GetIntSwitch(switches::kAutobrowseMaxResults, 0);
  if (max_results == 0) {
    max_results = GetIntSwitch(switches::kAbMaxResults, 10);
  }

  base::FilePath output_path =
      cmd.GetSwitchValuePath(switches::kAutobrowseOutput);
  if (output_path.empty()) {
    output_path = cmd.GetSwitchValuePath(switches::kAbOutput);
  }

  search_runner_ = std::make_unique<AutobrowseSearchRunner>(
      std::move(engine), query, max_results, output_path);
  search_runner_->Start();
}

void AutobrowseMainExtraParts::StartFetch() {
  const base::CommandLine& cmd = *base::CommandLine::ForCurrentProcess();

  const std::string url = cmd.GetSwitchValueASCII(switches::kAbUrl);
  if (url.empty()) {
    fprintf(stderr, "[autobrowse] fetch requires --ab-url=URL\n");
    return;
  }
  const std::string dump = cmd.GetSwitchValueASCII(switches::kAbDump);
  const std::string eval = cmd.GetSwitchValueASCII(switches::kAbEval);
  const std::string selector = cmd.GetSwitchValueASCII(switches::kAbSelector);
  const std::string wait_until = cmd.GetSwitchValueASCII(switches::kAbWaitUntil);
  const int extra_wait = GetIntSwitch(switches::kAbWait, 0);
  const int timeout = GetIntSwitch(switches::kAbTimeout, 30);
  const base::FilePath output_path = cmd.GetSwitchValuePath(switches::kAbOutput);

  fetch_runner_ = std::make_unique<AutobrowseFetchRunner>(
      url, dump, eval, selector, wait_until, extra_wait, timeout, output_path);
  fetch_runner_->Start();
}

void AutobrowseMainExtraParts::StartScrape() {
  const base::CommandLine& cmd = *base::CommandLine::ForCurrentProcess();

  const std::string urls_raw = cmd.GetSwitchValueASCII(switches::kAbUrls);
  std::vector<std::string> urls = base::SplitString(
      urls_raw, ", ", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  if (urls.empty()) {
    fprintf(stderr,
            "[autobrowse] scrape requires --ab-urls=\"u1,u2,...\"\n");
    return;
  }
  const std::string dump = cmd.GetSwitchValueASCII(switches::kAbDump);
  const std::string eval = cmd.GetSwitchValueASCII(switches::kAbEval);
  const int extra_wait = GetIntSwitch(switches::kAbWait, 0);
  const int timeout = GetIntSwitch(switches::kAbTimeout, 30);
  const base::FilePath output_path = cmd.GetSwitchValuePath(switches::kAbOutput);

  scrape_runner_ = std::make_unique<AutobrowseScrapeRunner>(
      std::move(urls), dump, eval, extra_wait, timeout, output_path);
  scrape_runner_->Start();
}

void AutobrowseMainExtraParts::StartMonitor() {
  const base::CommandLine& cmd = *base::CommandLine::ForCurrentProcess();

  const std::string url = cmd.GetSwitchValueASCII(switches::kAbUrl);
  if (url.empty()) {
    fprintf(stderr, "[autobrowse] monitor requires --ab-url=URL\n");
    return;
  }
  const std::string selector = cmd.GetSwitchValueASCII(switches::kAbSelector);
  const std::string on_change = cmd.GetSwitchValueASCII(switches::kAbOnChange);
  const int interval = GetIntSwitch(switches::kAbInterval, 60);
  // max-runs: 0 = forever; allow explicit 0, so parse manually.
  int max_runs = 0;
  if (cmd.HasSwitch(switches::kAbMaxRuns)) {
    base::StringToInt(cmd.GetSwitchValueASCII(switches::kAbMaxRuns), &max_runs);
  }
  const base::FilePath output_path = cmd.GetSwitchValuePath(switches::kAbOutput);

  monitor_runner_ = std::make_unique<AutobrowseMonitorRunner>(
      url, selector, on_change, interval, max_runs, output_path);
  monitor_runner_->Start();
}

void AutobrowseMainExtraParts::StartWarmup() {
  const base::CommandLine& cmd = *base::CommandLine::ForCurrentProcess();

  std::string engine = cmd.GetSwitchValueASCII(switches::kAbEngine);
  if (engine.empty()) {
    engine = cmd.GetSwitchValueASCII(switches::kAutobrowseEngine);
  }
  if (engine.empty()) {
    engine = "bing";
  }

  double minutes = 15.0;
  if (cmd.HasSwitch(switches::kAbMinutes)) {
    double parsed = 0;
    if (base::StringToDouble(cmd.GetSwitchValueASCII(switches::kAbMinutes),
                             &parsed) &&
        parsed > 0) {
      minutes = parsed;
    }
  }

  std::vector<std::string> queries = base::SplitString(
      cmd.GetSwitchValueASCII(switches::kAbQuery), ",", base::TRIM_WHITESPACE,
      base::SPLIT_WANT_NONEMPTY);
  if (queries.empty()) {
    queries = {"rust programming", "weather today", "latest news",
               "best laptops 2026", "how to cook pasta", "python tutorial"};
  }

  std::vector<std::string> urls = base::SplitString(
      cmd.GetSwitchValueASCII(switches::kAbUrls), ", ", base::TRIM_WHITESPACE,
      base::SPLIT_WANT_NONEMPTY);

  warmup_runner_ = std::make_unique<AutobrowseWarmupRunner>(
      std::move(engine), std::move(queries), std::move(urls), minutes);
  warmup_runner_->Start();
}

void AutobrowseMainExtraParts::StartSessionInfo() {
  const int top = GetIntSwitch(switches::kAbTop, 15);
  session_info_runner_ = std::make_unique<AutobrowseSessionInfoRunner>(top);
  session_info_runner_->Start();
}

}  // namespace autobrowse
