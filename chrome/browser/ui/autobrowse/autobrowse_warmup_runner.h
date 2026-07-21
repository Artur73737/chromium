// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_WARMUP_RUNNER_H_
#define CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_WARMUP_RUNNER_H_

#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "content/public/browser/web_contents_observer.h"

namespace autobrowse {

// Matures a reusable session (the --user-data-dir cookie jar) by really browsing
// a search engine for a set duration: it runs genuine SERP queries typed
// letter-by-letter, opens the first result of each, pauses naturally, and moves
// on, so the engine and result sites accumulate real cookies. Nothing is faked;
// it is a real returning-visitor history. Mirrors the Obscura `warmup` command.
class AutobrowseWarmupRunner : public content::WebContentsObserver {
 public:
  AutobrowseWarmupRunner(std::string engine,
                         std::vector<std::string> queries,
                         std::vector<std::string> urls,
                         double minutes);

  AutobrowseWarmupRunner(const AutobrowseWarmupRunner&) = delete;
  AutobrowseWarmupRunner& operator=(const AutobrowseWarmupRunner&) = delete;

  ~AutobrowseWarmupRunner() override;

  void Start();

 private:
  enum class Phase { kIdle, kHome, kSerp, kResult, kTargetUrl };

  // content::WebContentsObserver:
  void DocumentOnLoadCompletedInPrimaryMainFrame() override;

  std::string EngineHomeUrl() const;
  std::string TypingScript(const std::string& query) const;

  void TryAttach();
  void NextStep();
  void NavigateHome();
  void OnSerpLoaded();
  void OnResultLoaded();
  void ExtractFirstResult();
  void OnFirstResult(base::Value value);
  bool PastDeadline() const;
  void Finish();

  const std::string engine_;
  const std::vector<std::string> queries_;
  const std::vector<std::string> urls_;
  const double minutes_;

  base::TimeTicks start_time_;
  base::TimeDelta budget_;
  Phase phase_ = Phase::kIdle;
  size_t query_index_ = 0;
  size_t url_index_ = 0;
  bool attached_ = false;
  bool finished_ = false;

  base::RepeatingTimer attach_timer_;
  base::OneShotTimer pause_timer_;
  base::OneShotTimer type_timer_;

  base::WeakPtrFactory<AutobrowseWarmupRunner> weak_factory_{this};
};

}  // namespace autobrowse

#endif  // CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_WARMUP_RUNNER_H_
