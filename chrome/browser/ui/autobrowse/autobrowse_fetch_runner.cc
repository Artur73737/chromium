// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/autobrowse/autobrowse_fetch_runner.h"

#include <cstdio>
#include <utility>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/json/string_escape.h"
#include "base/logging.h"
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

AutobrowseFetchRunner::AutobrowseFetchRunner(std::string url,
                                             std::string dump,
                                             std::string eval,
                                             std::string selector,
                                             std::string wait_until,
                                             int extra_wait_seconds,
                                             int timeout_seconds,
                                             base::FilePath output_path)
    : url_(std::move(url)),
      dump_(std::move(dump)),
      eval_(std::move(eval)),
      selector_(std::move(selector)),
      wait_until_(std::move(wait_until)),
      extra_wait_seconds_(extra_wait_seconds),
      timeout_seconds_(timeout_seconds),
      output_path_(std::move(output_path)) {}

AutobrowseFetchRunner::~AutobrowseFetchRunner() = default;

void AutobrowseFetchRunner::Start() {
  start_time_ = base::TimeTicks::Now();
  deadline_timer_.Start(FROM_HERE, base::Seconds(timeout_seconds_),
                        base::BindOnce(&AutobrowseFetchRunner::OnTimeout,
                                       weak_factory_.GetWeakPtr()));
  TryAttachAndNavigate();
  if (!navigated_) {
    attach_timer_.Start(
        FROM_HERE, kAttachInterval,
        base::BindRepeating(&AutobrowseFetchRunner::TryAttachAndNavigate,
                            weak_factory_.GetWeakPtr()));
  }
}

void AutobrowseFetchRunner::TryAttachAndNavigate() {
  if (navigated_ || finished_) {
    return;
  }
  content::WebContents* wc = FindActiveTab();
  if (!wc) {
    return;
  }
  navigated_ = true;
  attach_timer_.Stop();
  Observe(wc);
  content::NavigationController::LoadURLParams load_params{GURL(url_)};
  load_params.transition_type = ui::PAGE_TRANSITION_TYPED;
  wc->GetController().LoadURLWithParams(load_params);
}

void AutobrowseFetchRunner::DocumentOnLoadCompletedInPrimaryMainFrame() {
  OnLoaded();
}

void AutobrowseFetchRunner::OnLoaded() {
  if (extracted_ || finished_) {
    return;
  }
  // Let late async work settle (extra --ab-wait, min 0), then extract once.
  const int settle = extra_wait_seconds_ > 0 ? extra_wait_seconds_ : 0;
  settle_timer_.Start(FROM_HERE, base::Seconds(settle),
                      base::BindOnce(&AutobrowseFetchRunner::RunExtract,
                                     weak_factory_.GetWeakPtr()));
}

void AutobrowseFetchRunner::RunExtract() {
  if (extracted_ || finished_ || !web_contents()) {
    return;
  }
  content::RenderFrameHost* rfh = web_contents()->GetPrimaryMainFrame();
  if (!rfh) {
    return;
  }
  extracted_ = true;
  rfh->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(BuildExtractScript()),
      base::BindOnce(&AutobrowseFetchRunner::OnExtractResult,
                     weak_factory_.GetWeakPtr()),
      ISOLATED_WORLD_ID_CHROME_INTERNAL);
}

void AutobrowseFetchRunner::OnExtractResult(base::Value value) {
  if (finished_) {
    return;
  }
  std::string out;
  if (value.is_string()) {
    out = value.GetString();
  } else {
    // --ab-eval may return a non-string (number/bool/object); serialize it.
    base::JSONWriter::Write(value, &out);
  }
  Finish(out);
}

void AutobrowseFetchRunner::Finish(const std::string& output) {
  if (finished_) {
    return;
  }
  finished_ = true;
  deadline_timer_.Stop();
  settle_timer_.Stop();

  if (output_path_.empty()) {
    fprintf(stdout, "%s\n", output.c_str());
    fflush(stdout);
  } else {
    if (!base::WriteFile(output_path_, output)) {
      fprintf(stderr, "[autobrowse] failed to write output file: %s\n",
              output_path_.AsUTF8Unsafe().c_str());
    }
  }

  Observe(nullptr);

  // Headless has no visible UI, so exit once the output is produced. With a GUI,
  // leave the window open on the fetched page.
  const bool headless =
      base::CommandLine::ForCurrentProcess()->HasSwitch("headless");
  if (headless) {
    chrome::AttemptExit();
  }
}

void AutobrowseFetchRunner::OnTimeout() {
  if (finished_) {
    return;
  }
  // Best effort: if the page did load enough to attach, try one last extract;
  // otherwise emit a small error object so callers always get something.
  if (web_contents() && !extracted_) {
    RunExtract();
    return;
  }
  fprintf(stderr, "[autobrowse] fetch timeout for %s\n", url_.c_str());
  Finish("");
}

std::string AutobrowseFetchRunner::BuildExtractScript() const {
  // --ab-eval takes precedence: evaluate the expression and return its value.
  if (!eval_.empty()) {
    return base::StrCat({"(function(){try{return (", eval_,
                         ");}catch(e){return 'error: '+e;}})()"});
  }

  const std::string dump = dump_.empty() ? "html" : dump_;
  if (dump == "text") {
    return "(document.body?document.body.innerText:'')";
  }
  if (dump == "links") {
    // One {text, href} JSON object per line, like Obscura --dump links.
    return "(function(){return [...document.querySelectorAll('a[href]')]"
           ".map(a=>JSON.stringify({text:(a.innerText||'').trim(),href:a.href}))"
           ".join('\\n');})()";
  }
  if (dump == "markdown") {
    // Lightweight DOM->Markdown: headings, links, list items, paragraphs.
    return base::StrCat({
        "(function(){",
        "  function esc(t){return (t||'').replace(/\\s+/g,' ').trim();}",
        "  const out=[];",
        "  document.querySelectorAll('h1,h2,h3,h4,h5,h6,p,li,a,pre').forEach(el=>{",
        "    const tag=el.tagName.toLowerCase(); const t=esc(el.innerText);",
        "    if(!t) return;",
        "    if(tag[0]==='h'){const n=+tag[1]; out.push('#'.repeat(n)+' '+t);}",
        "    else if(tag==='li'){out.push('- '+t);}",
        "    else if(tag==='pre'){out.push('```\\n'+t+'\\n```');}",
        "    else if(tag==='a'){const h=el.href; if(h) out.push('['+t+']('+h+')');}",
        "    else {out.push(t);}",
        "  });",
        "  return out.join('\\n\\n');",
        "})()",
    });
  }
  // Default: rendered outer HTML of the document.
  return "(document.documentElement?document.documentElement.outerHTML:'')";
}

}  // namespace autobrowse
