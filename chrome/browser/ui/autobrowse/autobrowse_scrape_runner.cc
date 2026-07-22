// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/autobrowse/autobrowse_scrape_runner.h"

#include <cstdio>
#include <utility>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/autobrowse/autobrowse_stealth.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

namespace autobrowse {

namespace {

// Runs on the thread pool (MayBlock): writes the output file, result ignored.
void WriteOutputFile(const base::FilePath& path, const std::string& data) {
  base::WriteFile(path, data);
}

}  // namespace

// ---------------------------------------------------------------------------
// AutobrowseScrapeRunner::Tab — one off-screen WebContents scraping one URL.
// ---------------------------------------------------------------------------
class AutobrowseScrapeRunner::Tab : public content::WebContentsObserver {
 public:
  Tab(content::BrowserContext* context,
      size_t index,
      const GURL& url,
      std::u16string script,
      base::TimeDelta settle,
      base::TimeDelta timeout,
      base::OnceCallback<void(size_t, base::Value)> done)
      : index_(index),
        script_(std::move(script)),
        settle_(settle),
        done_(std::move(done)) {
    web_contents_ = content::WebContents::Create(
        content::WebContents::CreateParams(context));
    Observe(web_contents_.get());
    // Mark visible so background throttling doesn't stall timers/JS.
    web_contents_->WasShown();

    timeout_timer_.Start(FROM_HERE, timeout,
                         base::BindOnce(&Tab::OnTimeout,
                                        weak_factory_.GetWeakPtr()));
    // Register stealth (if enabled) before the first navigation, then navigate.
    InstallStealthIfEnabled(
        web_contents_.get(),
        base::BindOnce(&Tab::Navigate, weak_factory_.GetWeakPtr(), url));
  }

  ~Tab() override = default;

 private:
  void Navigate(const GURL& url) {
    if (!web_contents_) {
      return;
    }
    content::NavigationController::LoadURLParams params{url};
    params.transition_type = ui::PAGE_TRANSITION_TYPED;
    web_contents_->GetController().LoadURLWithParams(params);
    // Fallback: off-screen contents may defer the onload event (paint-holding),
    // so extract a few seconds after navigation even if the load event never
    // arrives. The onload path below fires sooner for fast pages.
    fallback_timer_.Start(FROM_HERE, base::Seconds(5) + settle_,
                          base::BindOnce(&Tab::Extract,
                                         weak_factory_.GetWeakPtr()));
  }

  // content::WebContentsObserver:
  void DocumentOnLoadCompletedInPrimaryMainFrame() override {
    if (extracted_) {
      return;
    }
    settle_timer_.Start(
        FROM_HERE, settle_,
        base::BindOnce(&Tab::Extract, weak_factory_.GetWeakPtr()));
  }

  void Extract() {
    if (extracted_ || !web_contents_) {
      return;
    }
    content::RenderFrameHost* rfh = web_contents_->GetPrimaryMainFrame();
    if (!rfh) {
      return;
    }
    extracted_ = true;
    rfh->ExecuteJavaScriptInIsolatedWorld(
        script_,
        base::BindOnce(&Tab::OnResult, weak_factory_.GetWeakPtr()),
        ISOLATED_WORLD_ID_CHROME_INTERNAL);
  }

  void OnResult(base::Value value) {
    timeout_timer_.Stop();
    if (done_) {
      std::move(done_).Run(index_, std::move(value));
    }
  }

  void OnTimeout() {
    if (extracted_) {
      return;
    }
    extracted_ = true;
    if (done_) {
      std::move(done_).Run(index_, base::Value("error: timeout"));
    }
  }

  size_t index_;
  std::u16string script_;
  base::TimeDelta settle_;
  base::OnceCallback<void(size_t, base::Value)> done_;
  bool extracted_ = false;
  std::unique_ptr<content::WebContents> web_contents_;
  base::OneShotTimer settle_timer_;
  base::OneShotTimer fallback_timer_;
  base::OneShotTimer timeout_timer_;
  base::WeakPtrFactory<Tab> weak_factory_{this};
};

// ---------------------------------------------------------------------------
// AutobrowseScrapeRunner
// ---------------------------------------------------------------------------
AutobrowseScrapeRunner::AutobrowseScrapeRunner(std::vector<std::string> urls,
                                               std::string dump,
                                               std::string eval,
                                               int extra_wait_seconds,
                                               int timeout_seconds,
                                               int concurrency,
                                               base::FilePath output_path)
    : urls_(std::move(urls)),
      dump_(std::move(dump)),
      eval_(std::move(eval)),
      extra_wait_seconds_(extra_wait_seconds),
      timeout_seconds_(timeout_seconds),
      concurrency_(concurrency > 0 ? concurrency : 4),
      output_path_(std::move(output_path)) {}

AutobrowseScrapeRunner::~AutobrowseScrapeRunner() = default;

content::BrowserContext* AutobrowseScrapeRunner::GetContext() {
  Profile* profile = ProfileManager::GetLastUsedProfileIfLoaded();
  return profile;
}

void AutobrowseScrapeRunner::Start() {
  start_time_ = base::TimeTicks::Now();
  results_.resize(urls_.size());
  if (urls_.empty() || !GetContext()) {
    Finish();
    return;
  }
  LaunchMore();
}

void AutobrowseScrapeRunner::LaunchMore() {
  content::BrowserContext* context = GetContext();
  if (!context) {
    return;
  }
  const std::u16string script = base::UTF8ToUTF16(BuildExtractScript());
  while (tabs_.size() < static_cast<size_t>(concurrency_) &&
         next_index_ < urls_.size()) {
    const size_t index = next_index_++;
    tabs_[index] = std::make_unique<Tab>(
        context, index, GURL(urls_[index]), script,
        base::Seconds(extra_wait_seconds_ > 0 ? extra_wait_seconds_ : 0),
        base::Seconds(timeout_seconds_),
        base::BindOnce(&AutobrowseScrapeRunner::OnTabDone,
                       weak_factory_.GetWeakPtr()));
  }
}

void AutobrowseScrapeRunner::OnTabDone(size_t index, base::Value result) {
  if (finished_) {
    return;
  }
  if (index < results_.size()) {
    base::DictValue record;
    record.Set("url", urls_[index]);
    record.Set("result", std::move(result));
    results_[index] = base::Value(std::move(record));
  }
  ++completed_;

  // The finished Tab invoked this callback and is still on the stack, so free
  // it (and launch the next URL) on a fresh task to avoid a use-after-free.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&AutobrowseScrapeRunner::EraseTabAndContinue,
                                weak_factory_.GetWeakPtr(), index));
}

void AutobrowseScrapeRunner::EraseTabAndContinue(size_t index) {
  if (finished_) {
    return;
  }
  tabs_.erase(index);
  if (completed_ >= urls_.size()) {
    Finish();
    return;
  }
  LaunchMore();
}

void AutobrowseScrapeRunner::Finish() {
  if (finished_) {
    return;
  }
  finished_ = true;

  base::ListValue list;
  int count = 0;
  for (auto& v : results_) {
    if (v.is_none()) {
      continue;
    }
    list.Append(std::move(v));
    ++count;
  }

  base::DictValue out;
  out.Set("count", count);
  out.Set("results", std::move(list));
  out.Set("took_ms",
          static_cast<int>((base::TimeTicks::Now() - start_time_).InMilliseconds()));

  std::string pretty;
  base::JSONWriter::WriteWithOptions(
      base::Value(std::move(out)), base::JSONWriter::OPTIONS_PRETTY_PRINT,
      &pretty);

  // Drop all tabs on a fresh task, then deliver output.
  tabs_.clear();

  if (on_complete_) {
    std::move(on_complete_).Run(pretty);
    return;
  }
  if (output_path_.empty()) {
    fprintf(stdout, "%s\n", pretty.c_str());
    fflush(stdout);
    MaybeExit();
  } else {
    base::ThreadPool::PostTaskAndReply(
        FROM_HERE, {base::MayBlock()},
        base::BindOnce(&WriteOutputFile, output_path_, pretty),
        base::BindOnce(&AutobrowseScrapeRunner::MaybeExit,
                       weak_factory_.GetWeakPtr()));
  }
}

void AutobrowseScrapeRunner::MaybeExit() {
  if (base::CommandLine::ForCurrentProcess()->HasSwitch("headless")) {
    chrome::AttemptExit();
  }
}

std::string AutobrowseScrapeRunner::BuildExtractScript() const {
  if (!eval_.empty()) {
    return base::StrCat({"(function(){try{return (", eval_,
                         ");}catch(e){return 'error: '+e;}})()"});
  }
  const std::string dump = dump_.empty() ? "text" : dump_;
  if (dump == "html") {
    return "(document.documentElement?document.documentElement.outerHTML:'')";
  }
  if (dump == "links") {
    return "(function(){return [...document.querySelectorAll('a[href]')]"
           ".map(a=>a.href);})()";
  }
  return "(document.body?document.body.innerText:'')";
}

}  // namespace autobrowse
