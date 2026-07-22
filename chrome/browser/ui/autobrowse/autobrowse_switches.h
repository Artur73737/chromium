// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SWITCHES_H_
#define CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SWITCHES_H_

namespace autobrowse {
namespace switches {

// Single-entrypoint sub-command selector. The value picks the command to run at
// startup: "search", "fetch", "scrape", "monitor", "session-info", "warmup",
// "serve", "mcp", e.g. --autobrowse=fetch. The per-command inputs come from the
// plain switches below.
inline constexpr char kAutobrowse[] = "autobrowse";

// Legacy search shortcut: --autobrowse-search="QUERY" (kept for back-compat).
inline constexpr char kAutobrowseSearch[] = "autobrowse-search";
inline constexpr char kAutobrowseEngine[] = "autobrowse-engine";
inline constexpr char kAutobrowseMaxResults[] = "autobrowse-max-results";
inline constexpr char kAutobrowseOutput[] = "autobrowse-output";

// Shared, plain-English inputs for the sub-commands.
inline constexpr char kUrl[] = "url";                // fetch/monitor target
inline constexpr char kUrls[] = "urls";              // scrape targets (comma/space sep)
inline constexpr char kQuery[] = "query";            // search query
inline constexpr char kEngine[] = "engine";          // search engine
inline constexpr char kMaxResults[] = "max-results"; // search: max results
inline constexpr char kDump[] = "dump";              // html|text|links|markdown
inline constexpr char kScrape[] = "scrape";          // search: scrape each result (text|html|links)
inline constexpr char kEval[] = "eval";              // JS expression to evaluate
inline constexpr char kSelector[] = "selector";      // wait-for / watch CSS selector
inline constexpr char kWaitUntil[] = "wait-until";   // load|domcontentloaded|networkidle0
inline constexpr char kWait[] = "wait";              // extra settle seconds
inline constexpr char kTimeout[] = "timeout";        // navigation timeout seconds
inline constexpr char kOutput[] = "output";          // write output to file
inline constexpr char kOnChange[] = "on-change";     // monitor: JS producing the value
inline constexpr char kInterval[] = "interval";      // monitor: poll interval seconds
inline constexpr char kMaxRuns[] = "max-runs";       // monitor: stop after N polls (0=forever)
inline constexpr char kTop[] = "top";                // session-info: top N domains
inline constexpr char kMinutes[] = "minutes";        // warmup: browse duration (minutes)
inline constexpr char kPort[] = "port";              // serve: HTTP/WS port
inline constexpr char kConcurrency[] = "concurrency"; // scrape: parallel tabs

}  // namespace switches
}  // namespace autobrowse

#endif  // CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SWITCHES_H_
