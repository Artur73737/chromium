// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SWITCHES_H_
#define CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SWITCHES_H_

namespace autobrowse {
namespace switches {

// Runs a human-like web search at browser startup. The value is the query
// string, e.g. --autobrowse-search="rust tokio". Faithful to the Obscura
// `search` command: the query is typed into the engine's search box
// letter-by-letter, Enter is pressed, and result links are opened, instead of
// using a search API. Works headed and headless.
inline constexpr char kAutobrowseSearch[] = "autobrowse-search";

// Selects the search engine for --autobrowse-search: "google" (default),
// "bing", or "duckduckgo".
inline constexpr char kAutobrowseEngine[] = "autobrowse-engine";

// Maximum number of result links to collect (default 10).
inline constexpr char kAutobrowseMaxResults[] = "autobrowse-max-results";

// Optional file path to write the JSON result to instead of stdout.
inline constexpr char kAutobrowseOutput[] = "autobrowse-output";

// Single-entrypoint sub-command selector, Obscura-style. The value picks the
// command to run at startup: "search", "fetch", ... e.g. --autobrowse=fetch.
// The per-command inputs come from the --ab-* switches below. The legacy
// --autobrowse-search=QUERY switch above keeps working as a shortcut.
inline constexpr char kAutobrowse[] = "autobrowse";

// Shared --ab-* inputs for the sub-commands.
inline constexpr char kAbUrl[] = "ab-url";              // fetch/monitor target
inline constexpr char kAbUrls[] = "ab-urls";            // scrape targets (comma/space sep)
inline constexpr char kAbQuery[] = "ab-query";          // search query
inline constexpr char kAbEngine[] = "ab-engine";        // search engine
inline constexpr char kAbMaxResults[] = "ab-max-results";
inline constexpr char kAbDump[] = "ab-dump";            // html|text|links|markdown
inline constexpr char kAbEval[] = "ab-eval";            // JS expression to evaluate
inline constexpr char kAbSelector[] = "ab-selector";    // wait-for CSS selector
inline constexpr char kAbWaitUntil[] = "ab-wait-until"; // load|domcontentloaded|networkidle0
inline constexpr char kAbWait[] = "ab-wait";            // extra settle seconds
inline constexpr char kAbTimeout[] = "ab-timeout";      // navigation timeout seconds
inline constexpr char kAbOutput[] = "ab-output";        // write output to file
inline constexpr char kAbOnChange[] = "ab-on-change";   // monitor: JS producing the value
inline constexpr char kAbInterval[] = "ab-interval";    // monitor: poll interval seconds
inline constexpr char kAbMaxRuns[] = "ab-max-runs";     // monitor: stop after N polls (0=forever)
inline constexpr char kAbTop[] = "ab-top";              // session-info: top N domains
inline constexpr char kAbMinutes[] = "ab-minutes";      // warmup: browse duration (minutes)
inline constexpr char kAbPort[] = "ab-port";            // serve: HTTP/WS port

}  // namespace switches
}  // namespace autobrowse

#endif  // CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SWITCHES_H_
