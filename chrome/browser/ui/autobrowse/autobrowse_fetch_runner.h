// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_FETCH_RUNNER_H_
#define CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_FETCH_RUNNER_H_

#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "content/public/browser/web_contents_observer.h"

namespace autobrowse {

// Fetches and renders a single page in the browser's active tab, then either
// evaluates a JavaScript expression (--ab-eval) or dumps the rendered result in
// a chosen format (--ab-dump: html|text|links|markdown). Output goes to stdout
// or a file; in headless mode the browser then exits, while a visible GUI stays
// open on the page. Mirrors the Obscura `fetch` command. This never uses a
// network/search API: it is a real page navigation and DOM read.
class AutobrowseFetchRunner : public content::WebContentsObserver {
 public:
  AutobrowseFetchRunner(std::string url,
                        std::string dump,
                        std::string eval,
                        std::string selector,
                        std::string wait_until,
                        int extra_wait_seconds,
                        int timeout_seconds,
                        base::FilePath output_path);

  AutobrowseFetchRunner(const AutobrowseFetchRunner&) = delete;
  AutobrowseFetchRunner& operator=(const AutobrowseFetchRunner&) = delete;

  ~AutobrowseFetchRunner() override;

  // Server mode: deliver the output here instead of printing/exiting.
  void SetCompletionCallback(base::OnceCallback<void(std::string)> cb) {
    on_complete_ = std::move(cb);
  }

  // Finds the active tab (retrying until one exists) and navigates to the URL.
  void Start();

 private:
  // content::WebContentsObserver:
  void DocumentOnLoadCompletedInPrimaryMainFrame() override;

  std::string BuildExtractScript() const;

  void TryAttachAndNavigate();
  void DoNavigate();
  void OnLoaded();
  void RunExtract();
  void OnExtractResult(base::Value value);
  void Finish(const std::string& output);
  void OnTimeout();

  base::OnceCallback<void(std::string)> on_complete_;

  const std::string url_;
  const std::string dump_;
  const std::string eval_;
  const std::string selector_;
  const std::string wait_until_;
  const int extra_wait_seconds_;
  const int timeout_seconds_;
  const base::FilePath output_path_;

  base::TimeTicks start_time_;
  base::RepeatingTimer attach_timer_;
  base::OneShotTimer deadline_timer_;
  base::OneShotTimer settle_timer_;
  bool navigated_ = false;
  bool extracted_ = false;
  bool finished_ = false;

  base::WeakPtrFactory<AutobrowseFetchRunner> weak_factory_{this};
};

}  // namespace autobrowse

#endif  // CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_FETCH_RUNNER_H_
