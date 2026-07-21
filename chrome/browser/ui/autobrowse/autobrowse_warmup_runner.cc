// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/autobrowse/autobrowse_warmup_runner.h"

#include "chrome/browser/ui/autobrowse/autobrowse_stealth.h"

#include <cstdio>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/string_escape.h"
#include "base/rand_util.h"
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

// A short randomized human-like pause.
base::TimeDelta HumanPause() {
  return base::Milliseconds(1500 + base::RandIntInclusive(0, 2500));
}

}  // namespace

AutobrowseWarmupRunner::AutobrowseWarmupRunner(std::string engine,
                                               std::vector<std::string> queries,
                                               std::vector<std::string> urls,
                                               double minutes)
    : engine_(std::move(engine)),
      queries_(std::move(queries)),
      urls_(std::move(urls)),
      minutes_(minutes) {}

AutobrowseWarmupRunner::~AutobrowseWarmupRunner() = default;

std::string AutobrowseWarmupRunner::EngineHomeUrl() const {
  if (engine_ == "bing") {
    return "https://www.bing.com/";
  }
  if (engine_ == "duckduckgo" || engine_ == "ddg") {
    return "https://duckduckgo.com/";
  }
  return "https://www.google.com/ncr";
}

void AutobrowseWarmupRunner::Start() {
  start_time_ = base::TimeTicks::Now();
  budget_ = base::Seconds(static_cast<int64_t>(minutes_ * 60));
  TryAttach();
  if (!attached_) {
    attach_timer_.Start(FROM_HERE, kAttachInterval,
                        base::BindRepeating(&AutobrowseWarmupRunner::TryAttach,
                                            weak_factory_.GetWeakPtr()));
  }
}

void AutobrowseWarmupRunner::TryAttach() {
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
  InstallStealthIfEnabled(wc,
                          base::BindOnce(&AutobrowseWarmupRunner::NextStep,
                                         weak_factory_.GetWeakPtr()));
}

bool AutobrowseWarmupRunner::PastDeadline() const {
  return (base::TimeTicks::Now() - start_time_) >= budget_;
}

void AutobrowseWarmupRunner::NextStep() {
  if (finished_ || !web_contents()) {
    return;
  }
  if (PastDeadline()) {
    Finish();
    return;
  }
  // Interleave: run a query, then occasionally visit a target URL.
  if (!urls_.empty() && query_index_ > 0 &&
      (query_index_ % 2 == 0)) {
    const std::string& url = urls_[url_index_ % urls_.size()];
    ++url_index_;
    fprintf(stderr, "[warmup] visiting %s\n", url.c_str());
    phase_ = Phase::kTargetUrl;
    content::NavigationController::LoadURLParams params{GURL(url)};
    params.transition_type = ui::PAGE_TRANSITION_TYPED;
    web_contents()->GetController().LoadURLWithParams(params);
    return;
  }
  NavigateHome();
}

void AutobrowseWarmupRunner::NavigateHome() {
  if (finished_ || !web_contents()) {
    return;
  }
  const std::string& q = queries_[query_index_ % queries_.size()];
  fprintf(stderr, "[warmup] query: %s\n", q.c_str());
  phase_ = Phase::kHome;
  content::NavigationController::LoadURLParams params{GURL(EngineHomeUrl())};
  params.transition_type = ui::PAGE_TRANSITION_TYPED;
  web_contents()->GetController().LoadURLWithParams(params);
}

void AutobrowseWarmupRunner::DocumentOnLoadCompletedInPrimaryMainFrame() {
  if (finished_) {
    return;
  }
  switch (phase_) {
    case Phase::kHome: {
      // Type the current query and submit, after a short pause.
      const std::string& q = queries_[query_index_ % queries_.size()];
      const std::string script = TypingScript(q);
      type_timer_.Start(
          FROM_HERE, HumanPause(),
          base::BindOnce(
              [](base::WeakPtr<AutobrowseWarmupRunner> self, std::string s) {
                if (!self || self->finished_ || !self->web_contents()) {
                  return;
                }
                content::RenderFrameHost* rfh =
                    self->web_contents()->GetPrimaryMainFrame();
                if (rfh) {
                  rfh->ExecuteJavaScriptInIsolatedWorld(
                      base::UTF8ToUTF16(s), base::DoNothing(),
                      ISOLATED_WORLD_ID_CHROME_INTERNAL);
                }
                self->phase_ = Phase::kSerp;
              },
              weak_factory_.GetWeakPtr(), script));
      break;
    }
    case Phase::kSerp:
      OnSerpLoaded();
      break;
    case Phase::kResult:
      OnResultLoaded();
      break;
    case Phase::kTargetUrl:
      // Pause on the target, then move on.
      ++query_index_;
      pause_timer_.Start(FROM_HERE, HumanPause(),
                         base::BindOnce(&AutobrowseWarmupRunner::NextStep,
                                        weak_factory_.GetWeakPtr()));
      break;
    case Phase::kIdle:
      break;
  }
}

void AutobrowseWarmupRunner::OnSerpLoaded() {
  // After the SERP renders, pause, then open the first result.
  pause_timer_.Start(FROM_HERE, HumanPause(),
                     base::BindOnce(&AutobrowseWarmupRunner::ExtractFirstResult,
                                    weak_factory_.GetWeakPtr()));
}

void AutobrowseWarmupRunner::ExtractFirstResult() {
  if (finished_ || !web_contents()) {
    return;
  }
  content::RenderFrameHost* rfh = web_contents()->GetPrimaryMainFrame();
  if (!rfh) {
    return;
  }
  // Grab the first organic result href (skip engine-internal links).
  const char* script =
      "(function(){"
      "  var bad=/(^|\\.)(google|gstatic|bing|duckduckgo|microsoft)\\.[a-z]|"
      "\\/search\\?|accounts\\.|support\\.|policies\\./i;"
      "  var as=[...document.querySelectorAll('a[href] h1,a[href] h2,a[href] h3')]"
      ".map(h=>h.closest('a'));"
      "  for(const a of as){ if(a&&/^https?:/.test(a.href)&&!bad.test(a.href)) "
      "return a.href; }"
      "  return '';"
      "})()";
  rfh->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(std::string(script)),
      base::BindOnce(&AutobrowseWarmupRunner::OnFirstResult,
                     weak_factory_.GetWeakPtr()),
      ISOLATED_WORLD_ID_CHROME_INTERNAL);
}

void AutobrowseWarmupRunner::OnFirstResult(base::Value value) {
  if (finished_ || !web_contents()) {
    return;
  }
  const std::string url = value.is_string() ? value.GetString() : std::string();
  ++query_index_;
  if (url.empty()) {
    // No result (block/consent): just move on.
    NextStep();
    return;
  }
  fprintf(stderr, "[warmup] opening result %s\n", url.c_str());
  phase_ = Phase::kResult;
  content::NavigationController::LoadURLParams params{GURL(url)};
  params.transition_type = ui::PAGE_TRANSITION_LINK;
  web_contents()->GetController().LoadURLWithParams(params);
}

void AutobrowseWarmupRunner::OnResultLoaded() {
  // Dwell on the result page, then start the next query.
  pause_timer_.Start(FROM_HERE, HumanPause(),
                     base::BindOnce(&AutobrowseWarmupRunner::NextStep,
                                    weak_factory_.GetWeakPtr()));
}

void AutobrowseWarmupRunner::Finish() {
  if (finished_) {
    return;
  }
  finished_ = true;
  attach_timer_.Stop();
  pause_timer_.Stop();
  type_timer_.Stop();
  Observe(nullptr);
  fprintf(stderr, "[warmup] done after %.1f min, %zu queries\n",
          (base::TimeTicks::Now() - start_time_).InSecondsF() / 60.0,
          query_index_);
  // Cookies are already persisted in the profile dir. Exit cleanly so the jar
  // is flushed.
  chrome::AttemptExit();
}

std::string AutobrowseWarmupRunner::TypingScript(const std::string& query) const {
  std::string q;
  base::EscapeJSONString(query, /*put_in_quotes=*/true, &q);
  return base::StrCat({
      "(function(){",
      "  const Q=", q, ";",
      "  const box=document.querySelector('textarea[name=q],input[name=q],"
      "input[name=p],input#sb_form_q,input[name=query]');",
      "  if(!box) return;",
      "  box.focus(); box.value='';",
      "  let i=0;",
      "  (function step(){",
      "    if(i<Q.length){",
      "      box.value+=Q[i];",
      "      box.dispatchEvent(new InputEvent('input',{bubbles:true,data:Q[i],"
      "inputType:'insertText'}));",
      "      i++; setTimeout(step,55+Math.random()*110);",
      "    } else {",
      "      box.dispatchEvent(new KeyboardEvent('keydown',{bubbles:true,"
      "key:'Enter',keyCode:13,which:13,code:'Enter'}));",
      "      const f=box.form||document.querySelector('form');",
      "      setTimeout(function(){ if(location.pathname.indexOf('/search')<0 && f){"
      "        if(f.requestSubmit) f.requestSubmit(); else f.submit(); } },250);",
      "    }",
      "  })();",
      "})()",
  });
}

}  // namespace autobrowse
