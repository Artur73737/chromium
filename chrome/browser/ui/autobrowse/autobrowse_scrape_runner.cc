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
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/ui/browser_window/public/browser_collection.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

namespace autobrowse {

namespace {

constexpr base::TimeDelta kAttachInterval = base::Milliseconds(250);

content::WebContents* FindActiveTab() {
  GlobalBrowserCollection* collection = GlobalBrowserCollection::GetInstance();
  if (!collection) {
    return nullptr;
  }
  content::WebContents* found = nullptr;
  collection->ForEach(
      [&found](BrowserWindowInterface* browser) {
        if (browser) {
          if (content::WebContents* wc =
                  browser->GetTabStripModel()->GetActiveWebContents()) {
            found = wc;
            return false;
          }
        }
        return true;
      },
      BrowserCollection::Order::kActivation);
  return found;
}

}  // namespace

AutobrowseScrapeRunner::AutobrowseScrapeRunner(std::vector<std::string> urls,
                                               std::string dump,
                                               std::string eval,
                                               int extra_wait_seconds,
                                               int timeout_seconds,
                                               base::FilePath output_path)
    : urls_(std::move(urls)),
      dump_(std::move(dump)),
      eval_(std::move(eval)),
      extra_wait_seconds_(extra_wait_seconds),
      timeout_seconds_(timeout_seconds),
      output_path_(std::move(output_path)) {}

AutobrowseScrapeRunner::~AutobrowseScrapeRunner() = default;

void AutobrowseScrapeRunner::Start() {
  start_time_ = base::TimeTicks::Now();
  if (urls_.empty()) {
    Finish();
    return;
  }
  TryAttach();
  if (!attached_) {
    attach_timer_.Start(FROM_HERE, kAttachInterval,
                        base::BindRepeating(&AutobrowseScrapeRunner::TryAttach,
                                            weak_factory_.GetWeakPtr()));
  }
}

void AutobrowseScrapeRunner::TryAttach() {
  if (attached_ || finished_) {
    return;
  }
  content::WebContents* wc = FindActiveTab();
  if (!wc) {
    return;
  }
  attached_ = true;
  attach_timer_.Stop();
  Observe(wc);
  NavigateCurrent();
}

void AutobrowseScrapeRunner::NavigateCurrent() {
  if (finished_ || index_ >= urls_.size() || !web_contents()) {
    return;
  }
  extracted_current_ = false;
  item_deadline_timer_.Start(FROM_HERE, base::Seconds(timeout_seconds_),
                             base::BindOnce(&AutobrowseScrapeRunner::OnItemTimeout,
                                            weak_factory_.GetWeakPtr()));
  content::NavigationController::LoadURLParams load_params{GURL(urls_[index_])};
  load_params.transition_type = ui::PAGE_TRANSITION_TYPED;
  web_contents()->GetController().LoadURLWithParams(load_params);
}

void AutobrowseScrapeRunner::DocumentOnLoadCompletedInPrimaryMainFrame() {
  OnLoaded();
}

void AutobrowseScrapeRunner::OnLoaded() {
  if (extracted_current_ || finished_) {
    return;
  }
  const int settle = extra_wait_seconds_ > 0 ? extra_wait_seconds_ : 0;
  settle_timer_.Start(FROM_HERE, base::Seconds(settle),
                      base::BindOnce(&AutobrowseScrapeRunner::RunExtract,
                                     weak_factory_.GetWeakPtr()));
}

void AutobrowseScrapeRunner::RunExtract() {
  if (extracted_current_ || finished_ || !web_contents()) {
    return;
  }
  content::RenderFrameHost* rfh = web_contents()->GetPrimaryMainFrame();
  if (!rfh) {
    return;
  }
  extracted_current_ = true;
  rfh->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(BuildExtractScript()),
      base::BindOnce(&AutobrowseScrapeRunner::OnExtractResult,
                     weak_factory_.GetWeakPtr()),
      ISOLATED_WORLD_ID_CHROME_INTERNAL);
}

void AutobrowseScrapeRunner::OnExtractResult(base::Value value) {
  if (finished_) {
    return;
  }
  item_deadline_timer_.Stop();
  base::DictValue record;
  record.Set("url", urls_[index_]);
  record.Set("result", std::move(value));
  results_.Append(base::Value(std::move(record)));
  AdvanceOrFinish();
}

void AutobrowseScrapeRunner::AdvanceOrFinish() {
  ++index_;
  if (index_ >= urls_.size()) {
    Finish();
    return;
  }
  NavigateCurrent();
}

void AutobrowseScrapeRunner::OnItemTimeout() {
  if (finished_ || extracted_current_) {
    return;
  }
  // Record an error for this URL and move on, so one bad page never stalls the
  // whole batch.
  base::DictValue record;
  record.Set("url", urls_[index_]);
  record.Set("error", "timeout");
  results_.Append(base::Value(std::move(record)));
  extracted_current_ = true;
  AdvanceOrFinish();
}

void AutobrowseScrapeRunner::Finish() {
  if (finished_) {
    return;
  }
  finished_ = true;
  attach_timer_.Stop();
  item_deadline_timer_.Stop();
  settle_timer_.Stop();

  base::DictValue out;
  out.Set("count", static_cast<int>(results_.size()));
  out.Set("results", std::move(results_));
  out.Set("took_ms",
          static_cast<int>((base::TimeTicks::Now() - start_time_).InMilliseconds()));

  std::string pretty;
  base::JSONWriter::WriteWithOptions(
      base::Value(std::move(out)), base::JSONWriter::OPTIONS_PRETTY_PRINT,
      &pretty);

  if (output_path_.empty()) {
    fprintf(stdout, "%s\n", pretty.c_str());
    fflush(stdout);
  } else {
    if (!base::WriteFile(output_path_, pretty)) {
      fprintf(stderr, "[autobrowse] failed to write output file: %s\n",
              output_path_.AsUTF8Unsafe().c_str());
    }
  }

  Observe(nullptr);
  const bool headless =
      base::CommandLine::ForCurrentProcess()->HasSwitch("headless");
  if (headless) {
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
  // default text
  return "(document.body?document.body.innerText:'')";
}

}  // namespace autobrowse
