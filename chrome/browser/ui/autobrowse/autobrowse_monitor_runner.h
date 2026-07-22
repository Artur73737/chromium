// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_MONITOR_RUNNER_H_
#define CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_MONITOR_RUNNER_H_

#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "content/public/browser/web_contents_observer.h"

namespace autobrowse {

// Watches a page and emits a record whenever a watched value changes. Each poll
// (re)loads the URL, extracts a value from --ab-selector via --ab-on-change JS
// (with the element in scope, so bare `textContent` works), hashes it, and emits
// an NDJSON line only when the hash differs from the previous one. Runs forever
// or until --ab-max-runs polls. Mirrors the Obscura `monitor` command.
class AutobrowseMonitorRunner : public content::WebContentsObserver {
 public:
  AutobrowseMonitorRunner(std::string url,
                          std::string selector,
                          std::string on_change,
                          int interval_seconds,
                          int max_runs,
                          base::FilePath output_path);

  AutobrowseMonitorRunner(const AutobrowseMonitorRunner&) = delete;
  AutobrowseMonitorRunner& operator=(const AutobrowseMonitorRunner&) = delete;

  ~AutobrowseMonitorRunner() override;

  void Start();

  // Server mode: deliver the first extracted value here (single poll) instead
  // of streaming NDJSON, then stop.
  void SetCompletionCallback(base::OnceCallback<void(std::string)> cb) {
    on_complete_ = std::move(cb);
  }

 private:
  // content::WebContentsObserver:
  void DocumentOnLoadCompletedInPrimaryMainFrame() override;

  std::string BuildExtractScript() const;

  void TryAttach();
  void LoadOnce();
  void RunExtract();
  void OnPollTimeout();
  void OnExtractResult(base::Value value);
  void ScheduleNextOrStop();
  void Stop();

  base::OnceCallback<void(std::string)> on_complete_;

  const std::string url_;
  const std::string selector_;
  const std::string on_change_;
  const int interval_seconds_;
  const int max_runs_;
  const base::FilePath output_path_;

  bool attached_ = false;
  bool extracted_this_poll_ = false;
  bool stopped_ = false;
  int runs_ = 0;
  size_t last_hash_ = 0;
  bool have_last_ = false;

  base::RepeatingTimer attach_timer_;
  base::OneShotTimer interval_timer_;
  base::OneShotTimer poll_deadline_timer_;

  base::WeakPtrFactory<AutobrowseMonitorRunner> weak_factory_{this};
};

}  // namespace autobrowse

#endif  // CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_MONITOR_RUNNER_H_
