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
#include "chrome/browser/ui/autobrowse/autobrowse_server.h"
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
  } else if (command == "serve" || command == "octo-serve" ||
             command == "mcp") {
    StartServe();
  } else {
    fprintf(stderr, "[autobrowse] unknown command: %s\n", command.c_str());
  }
}

void AutobrowseMainExtraParts::StartSearch() {
  const base::CommandLine& cmd = *base::CommandLine::ForCurrentProcess();

  // Query: legacy --autobrowse-search=Q wins, else --query.
  std::string query = cmd.GetSwitchValueASCII(switches::kAutobrowseSearch);
  if (query.empty()) {
    query = cmd.GetSwitchValueASCII(switches::kQuery);
  }

  // Engine: --autobrowse-engine (legacy) or --engine, default google.
  std::string engine = cmd.GetSwitchValueASCII(switches::kAutobrowseEngine);
  if (engine.empty()) {
    engine = cmd.GetSwitchValueASCII(switches::kEngine);
  }
  if (engine.empty()) {
    engine = "google";
  }

  int max_results = GetIntSwitch(switches::kAutobrowseMaxResults, 0);
  if (max_results == 0) {
    max_results = GetIntSwitch(switches::kMaxResults, 10);
  }

  base::FilePath output_path =
      cmd.GetSwitchValuePath(switches::kAutobrowseOutput);
  if (output_path.empty()) {
    output_path = cmd.GetSwitchValuePath(switches::kOutput);
  }

  search_runner_ = std::make_unique<AutobrowseSearchRunner>(
      std::move(engine), query, max_results, output_path);
  if (cmd.HasSwitch(switches::kScrape)) {
    search_runner_->SetScrape(cmd.GetSwitchValueASCII(switches::kScrape));
  }
  search_runner_->Start();
}

void AutobrowseMainExtraParts::StartFetch() {
  const base::CommandLine& cmd = *base::CommandLine::ForCurrentProcess();

  const std::string url = cmd.GetSwitchValueASCII(switches::kUrl);
  if (url.empty()) {
    fprintf(stderr, "[autobrowse] fetch requires --url=URL\n");
    return;
  }
  const std::string dump = cmd.GetSwitchValueASCII(switches::kDump);
  const std::string eval = cmd.GetSwitchValueASCII(switches::kEval);
  const std::string selector = cmd.GetSwitchValueASCII(switches::kSelector);
  const std::string wait_until = cmd.GetSwitchValueASCII(switches::kWaitUntil);
  const int extra_wait = GetIntSwitch(switches::kWait, 0);
  const int timeout = GetIntSwitch(switches::kTimeout, 30);
  const base::FilePath output_path = cmd.GetSwitchValuePath(switches::kOutput);

  fetch_runner_ = std::make_unique<AutobrowseFetchRunner>(
      url, dump, eval, selector, wait_until, extra_wait, timeout, output_path);
  fetch_runner_->Start();
}

void AutobrowseMainExtraParts::StartScrape() {
  const base::CommandLine& cmd = *base::CommandLine::ForCurrentProcess();

  const std::string urls_raw = cmd.GetSwitchValueASCII(switches::kUrls);
  std::vector<std::string> urls = base::SplitString(
      urls_raw, ", ", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  if (urls.empty()) {
    fprintf(stderr,
            "[autobrowse] scrape requires --urls=\"u1,u2,...\"\n");
    return;
  }
  const std::string dump = cmd.GetSwitchValueASCII(switches::kDump);
  const std::string eval = cmd.GetSwitchValueASCII(switches::kEval);
  const int extra_wait = GetIntSwitch(switches::kWait, 0);
  const int timeout = GetIntSwitch(switches::kTimeout, 30);
  const int concurrency = GetIntSwitch(switches::kConcurrency, 4);
  const base::FilePath output_path = cmd.GetSwitchValuePath(switches::kOutput);

  scrape_runner_ = std::make_unique<AutobrowseScrapeRunner>(
      std::move(urls), dump, eval, extra_wait, timeout, concurrency,
      output_path);
  scrape_runner_->Start();
}

void AutobrowseMainExtraParts::StartMonitor() {
  const base::CommandLine& cmd = *base::CommandLine::ForCurrentProcess();

  const std::string url = cmd.GetSwitchValueASCII(switches::kUrl);
  if (url.empty()) {
    fprintf(stderr, "[autobrowse] monitor requires --url=URL\n");
    return;
  }
  const std::string selector = cmd.GetSwitchValueASCII(switches::kSelector);
  const std::string on_change = cmd.GetSwitchValueASCII(switches::kOnChange);
  const int interval = GetIntSwitch(switches::kInterval, 60);
  // max-runs: 0 = forever; allow explicit 0, so parse manually.
  int max_runs = 0;
  if (cmd.HasSwitch(switches::kMaxRuns)) {
    base::StringToInt(cmd.GetSwitchValueASCII(switches::kMaxRuns), &max_runs);
  }
  const base::FilePath output_path = cmd.GetSwitchValuePath(switches::kOutput);

  monitor_runner_ = std::make_unique<AutobrowseMonitorRunner>(
      url, selector, on_change, interval, max_runs, output_path);
  monitor_runner_->Start();
}

void AutobrowseMainExtraParts::StartWarmup() {
  const base::CommandLine& cmd = *base::CommandLine::ForCurrentProcess();

  std::string engine = cmd.GetSwitchValueASCII(switches::kEngine);
  if (engine.empty()) {
    engine = cmd.GetSwitchValueASCII(switches::kAutobrowseEngine);
  }
  if (engine.empty()) {
    engine = "bing";
  }

  double minutes = 15.0;
  if (cmd.HasSwitch(switches::kMinutes)) {
    double parsed = 0;
    if (base::StringToDouble(cmd.GetSwitchValueASCII(switches::kMinutes),
                             &parsed) &&
        parsed > 0) {
      minutes = parsed;
    }
  }

  std::vector<std::string> queries = base::SplitString(
      cmd.GetSwitchValueASCII(switches::kQuery), ",", base::TRIM_WHITESPACE,
      base::SPLIT_WANT_NONEMPTY);
  if (queries.empty()) {
    queries = {"rust programming", "weather today", "latest news",
               "best laptops 2026", "how to cook pasta", "python tutorial"};
  }

  std::vector<std::string> urls = base::SplitString(
      cmd.GetSwitchValueASCII(switches::kUrls), ", ", base::TRIM_WHITESPACE,
      base::SPLIT_WANT_NONEMPTY);

  warmup_runner_ = std::make_unique<AutobrowseWarmupRunner>(
      std::move(engine), std::move(queries), std::move(urls), minutes);
  warmup_runner_->Start();
}

void AutobrowseMainExtraParts::StartServe() {
  const int port = GetIntSwitch(switches::kPort, 8080);
  server_ = std::make_unique<AutobrowseServer>();
  if (!server_->Start(port)) {
    server_.reset();
  }
  // The server keeps running; the browser stays alive to serve requests.
}

void AutobrowseMainExtraParts::StartSessionInfo() {
  const int top = GetIntSwitch(switches::kTop, 15);
  session_info_runner_ = std::make_unique<AutobrowseSessionInfoRunner>(top);
  session_info_runner_->Start();
}

}  // namespace autobrowse
