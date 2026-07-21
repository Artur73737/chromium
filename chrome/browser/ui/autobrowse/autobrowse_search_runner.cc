// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/autobrowse/autobrowse_search_runner.h"

#include "chrome/browser/ui/autobrowse/autobrowse_stealth.h"

#include <cstdio>
#include <utility>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/json/string_escape.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
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

// Overall wall-clock budget for a single search before we give up.
constexpr base::TimeDelta kDeadline = base::Seconds(35);
// How often we re-run the in-page driver script.
constexpr base::TimeDelta kPollInterval = base::Milliseconds(700);
// How often we retry to find the active tab at startup.
constexpr base::TimeDelta kAttachInterval = base::Milliseconds(250);

// Returns the visible active tab of the frontmost browser window, or nullptr.
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
            return false;  // Stop iterating.
          }
        }
        return true;  // Continue.
      },
      BrowserCollection::Order::kActivation);
  return found;
}

std::string JsStringLiteral(const std::string& s) {
  std::string out;
  base::EscapeJSONString(s, /*put_in_quotes=*/true, &out);
  return out;
}

// Runs on the thread pool (MayBlock): writes the output file, result ignored.
void WriteOutputFile(const base::FilePath& path, const std::string& data) {
  base::WriteFile(path, data);
}

}  // namespace

AutobrowseSearchRunner::AutobrowseSearchRunner(std::string engine,
                                               std::string query,
                                               int max_results,
                                               base::FilePath output_path)
    : engine_(std::move(engine)),
      query_(std::move(query)),
      max_results_(max_results),
      output_path_(std::move(output_path)) {}

AutobrowseSearchRunner::~AutobrowseSearchRunner() = default;

std::string AutobrowseSearchRunner::StartUrl() const {
  if (engine_ == "bing") {
    return "https://www.bing.com/";
  }
  if (engine_ == "duckduckgo" || engine_ == "ddg") {
    return "https://duckduckgo.com/";
  }
  // Google, with the "no country redirect" entry point.
  return "https://www.google.com/ncr";
}

void AutobrowseSearchRunner::Start() {
  start_time_ = base::TimeTicks::Now();
  // Also start the overall deadline now, so a browser that never produces a
  // usable tab still terminates cleanly.
  deadline_timer_.Start(FROM_HERE, kDeadline,
                        base::BindOnce(&AutobrowseSearchRunner::OnTimeout,
                                       weak_factory_.GetWeakPtr()));
  TryAttachAndNavigate();
  if (!navigated_) {
    attach_timer_.Start(
        FROM_HERE, kAttachInterval,
        base::BindRepeating(&AutobrowseSearchRunner::TryAttachAndNavigate,
                            weak_factory_.GetWeakPtr()));
  }
}

void AutobrowseSearchRunner::TryAttachAndNavigate() {
  if (navigated_ || finished_) {
    return;
  }
  content::WebContents* wc = FindActiveTab();
  if (!wc) {
    return;  // Retry on the next attach tick.
  }
  navigated_ = true;
  attach_timer_.Stop();
  Observe(wc);
  InstallStealthIfEnabled(
      wc, base::BindOnce(&AutobrowseSearchRunner::DoNavigate,
                         weak_factory_.GetWeakPtr()));
}

void AutobrowseSearchRunner::DoNavigate() {
  if (finished_ || !web_contents()) {
    return;
  }
  content::NavigationController::LoadURLParams load_params{GURL(StartUrl())};
  load_params.transition_type = ui::PAGE_TRANSITION_TYPED;
  web_contents()->GetController().LoadURLWithParams(load_params);
}

void AutobrowseSearchRunner::DocumentOnLoadCompletedInPrimaryMainFrame() {
  if (scraping_) {
    // A result page finished loading: extract its content after a short settle.
    if (scrape_extracted_) {
      return;
    }
    scrape_settle_timer_.Start(
        FROM_HERE, base::Seconds(1),
        base::BindOnce(&AutobrowseSearchRunner::ExtractScrapeContent,
                       weak_factory_.GetWeakPtr()));
    return;
  }
  EnsurePolling();
  RunDriver();
}

void AutobrowseSearchRunner::EnsurePolling() {
  if (polling_started_ || finished_) {
    return;
  }
  polling_started_ = true;
  poll_timer_.Start(FROM_HERE, kPollInterval,
                    base::BindRepeating(&AutobrowseSearchRunner::RunDriver,
                                        weak_factory_.GetWeakPtr()));
}

void AutobrowseSearchRunner::RunDriver() {
  if (finished_) {
    return;
  }
  if (!web_contents()) {
    return;
  }
  content::RenderFrameHost* rfh = web_contents()->GetPrimaryMainFrame();
  if (!rfh) {
    return;
  }
  rfh->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(BuildDriverScript()),
      base::BindOnce(&AutobrowseSearchRunner::OnDriverResult,
                     weak_factory_.GetWeakPtr()),
      ISOLATED_WORLD_ID_CHROME_INTERNAL);
}

void AutobrowseSearchRunner::OnDriverResult(base::Value value) {
  if (finished_ || !value.is_string()) {
    return;
  }
  const std::string& s = value.GetString();
  if (getenv("AB_DEBUG")) {
    LOG(ERROR) << "[autobrowse] driver -> " << s;
  }
  if (!s.empty() && s.front() == '{') {
    MaybeScrapeThenFinish(s);
  }
  // "CONSENT" / "TYPING" / "WAIT": keep polling until a final JSON arrives.
}

void AutobrowseSearchRunner::MaybeScrapeThenFinish(
    const std::string& final_json) {
  if (finished_ || scraping_) {
    return;
  }
  // No scrape requested: emit the SERP result as-is.
  if (scrape_kind_.empty()) {
    Finish(final_json);
    return;
  }
  std::optional<base::Value> parsed =
      base::JSONReader::Read(final_json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    Finish(final_json);
    return;
  }
  base::ListValue* results = parsed->GetDict().FindList("results");
  if (!results || results->empty()) {
    Finish(final_json);
    return;
  }
  // Enter the scrape phase: stop the SERP driver, then open each result.
  pending_result_ = std::move(*parsed);
  scraping_ = true;
  scrape_index_ = 0;
  poll_timer_.Stop();
  deadline_timer_.Stop();  // per-item timers bound the scrape phase instead.
  NavigateToScrapeTarget();
}

void AutobrowseSearchRunner::NavigateToScrapeTarget() {
  if (finished_ || !web_contents()) {
    return;
  }
  base::ListValue* results = pending_result_.GetDict().FindList("results");
  if (!results || scrape_index_ >= results->size()) {
    FinishScrape();
    return;
  }
  const std::string* url =
      (*results)[scrape_index_].GetDict().FindString("url");
  if (!url) {
    ++scrape_index_;
    NavigateToScrapeTarget();
    return;
  }
  scrape_extracted_ = false;
  scrape_item_timer_.Start(
      FROM_HERE, base::Seconds(20),
      base::BindOnce(&AutobrowseSearchRunner::OnScrapeItemTimeout,
                     weak_factory_.GetWeakPtr()));
  content::NavigationController::LoadURLParams params{GURL(*url)};
  params.transition_type = ui::PAGE_TRANSITION_LINK;
  web_contents()->GetController().LoadURLWithParams(params);
}

void AutobrowseSearchRunner::ExtractScrapeContent() {
  if (finished_ || scrape_extracted_ || !web_contents()) {
    return;
  }
  content::RenderFrameHost* rfh = web_contents()->GetPrimaryMainFrame();
  if (!rfh) {
    return;
  }
  scrape_extracted_ = true;
  rfh->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(BuildScrapeScript()),
      base::BindOnce(&AutobrowseSearchRunner::OnScrapeContent,
                     weak_factory_.GetWeakPtr()),
      ISOLATED_WORLD_ID_CHROME_INTERNAL);
}

void AutobrowseSearchRunner::OnScrapeContent(base::Value value) {
  if (finished_) {
    return;
  }
  scrape_item_timer_.Stop();
  base::ListValue* results = pending_result_.GetDict().FindList("results");
  if (results && scrape_index_ < results->size()) {
    (*results)[scrape_index_].GetDict().Set("scraped", std::move(value));
  }
  ++scrape_index_;
  NavigateToScrapeTarget();
}

void AutobrowseSearchRunner::OnScrapeItemTimeout() {
  if (finished_ || scrape_extracted_) {
    return;
  }
  // This result page never settled: mark it and move on.
  scrape_extracted_ = true;
  base::ListValue* results = pending_result_.GetDict().FindList("results");
  if (results && scrape_index_ < results->size()) {
    (*results)[scrape_index_].GetDict().Set("scraped", "error: timeout");
  }
  ++scrape_index_;
  NavigateToScrapeTarget();
}

void AutobrowseSearchRunner::FinishScrape() {
  std::string json;
  base::JSONWriter::Write(pending_result_, &json);
  Finish(json);
}

std::string AutobrowseSearchRunner::BuildScrapeScript() const {
  if (scrape_kind_ == "html") {
    return "(document.documentElement?document.documentElement.outerHTML:'')";
  }
  if (scrape_kind_ == "links") {
    return "(function(){return [...document.querySelectorAll('a[href]')]"
           ".map(a=>a.href);})()";
  }
  // default: visible text
  return "(document.body?document.body.innerText:'')";
}

void AutobrowseSearchRunner::Finish(const std::string& json) {
  if (finished_) {
    return;
  }
  finished_ = true;
  poll_timer_.Stop();
  deadline_timer_.Stop();

  // Re-emit pretty-printed with a took_ms field, Obscura-style.
  std::string out = json;
  std::optional<base::Value> parsed =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (parsed && parsed->is_dict()) {
    const int64_t took = (base::TimeTicks::Now() - start_time_).InMilliseconds();
    parsed->GetDict().Set("took_ms", static_cast<int>(took));
    std::string pretty;
    if (base::JSONWriter::WriteWithOptions(
            *parsed, base::JSONWriter::OPTIONS_PRETTY_PRINT, &pretty)) {
      out = pretty;
    }
  }

  Observe(nullptr);

  // Server mode: hand the result to the callback and stay alive for more work.
  if (on_complete_) {
    std::move(on_complete_).Run(out);
    return;
  }

  if (output_path_.empty()) {
    fprintf(stdout, "%s\n", out.c_str());
    fflush(stdout);
    MaybeExit();
  } else {
    // Disk I/O must not block the UI thread: write on the thread pool, then
    // exit (headless) once the file is on disk.
    base::ThreadPool::PostTaskAndReply(
        FROM_HERE, {base::MayBlock()},
        base::BindOnce(&WriteOutputFile, output_path_, out),
        base::BindOnce(&AutobrowseSearchRunner::MaybeExit,
                       weak_factory_.GetWeakPtr()));
  }
}

void AutobrowseSearchRunner::MaybeExit() {
  // Headless has no visible UI, so exit once the output is produced. A visible
  // GUI keeps the window open on the results page.
  if (base::CommandLine::ForCurrentProcess()->HasSwitch("headless")) {
    chrome::AttemptExit();
  }
}

void AutobrowseSearchRunner::OnTimeout() {
  if (finished_) {
    return;
  }
  base::DictValue dict;
  dict.Set("query", query_);
  dict.Set("engine", engine_);
  dict.Set("error", "timeout");
  dict.Set("reason", "no results within deadline (block, consent wall, or slow)");
  dict.Set("results", base::ListValue());
  std::string json;
  base::JSONWriter::Write(base::Value(std::move(dict)), &json);
  Finish(json);
}

std::string AutobrowseSearchRunner::BuildDriverScript() const {
  const std::string q = JsStringLiteral(query_);
  const std::string e = JsStringLiteral(engine_);
  const std::string n = base::NumberToString(max_results_);

  // A single "driver" evaluated repeatedly in an isolated world. It classifies
  // the current page and acts:
  //   - consent wall  -> click accept/reject, return "CONSENT"
  //   - block/captcha -> return final JSON with an error
  //   - results found -> return final JSON with the links
  //   - home page     -> type the query letter-by-letter, submit, return "TYPING"
  // Typing is guarded by window.__ab_typed so repeated polls don't retype.
  return base::StrCat({
      "(function(){",
      "  const Q=", q, ";",
      "  const N=", n, ";",
      "  const E=", e, ";",
      "  const host=location.host, path=location.pathname;",
      "  const body=document.body?document.body.innerText:'';",
      // Cookie consent: ALWAYS reject (privacy-preserving). A dedicated finder
      // matches a 'reject all' control by text/aria-label; used both on the
      // consent.google.com wall and on an inline 'before you continue' dialog.
      "  function findReject(){",
      "    const els=[...document.querySelectorAll('button,input[type=submit],"
      "div[role=button],a[role=button],[aria-label]')];",
      "    const rx=/^(reject all|reject|rifiuta tutto|rifiuta|tout refuser|"
      "alle ablehnen|ablehnen|decline|no,? thanks)$/i;",
      "    return els.find(x=>rx.test(((x.textContent||x.value||"
      "x.getAttribute('aria-label')||'').trim())));",
      "  }",
      "  if(host.indexOf('consent')>=0){",
      "    const b=findReject()||[...document.querySelectorAll('button,"
      "input[type=submit],div[role=button]')].find(x=>/reject|rifiuta|ablehnen|"
      "refuser|decline/i.test((x.textContent||x.value||'')));",
      "    if(b){b.click();return 'CONSENT';} return 'WAIT';",
      "  }",
      // Inline consent dialog on a normal page: reject once, then carry on.
      "  if(!window.__ab_consent){const r=findReject();"
      "if(r){window.__ab_consent=1;r.click();return 'CONSENT';}}",
      "  if(host.indexOf('sorry')>=0||/unusual traffic|detected unusual|are not a robot/i.test(body)){",
      "    return JSON.stringify({query:Q,engine:E,error:'blocked',"
      "reason:'captcha / unusual-traffic wall',results:[]});",
      "  }",
      "  function extract(){",
      "    const bad=/(^|\\.)(google|gstatic|googleusercontent|bing|duckduckgo)\\.[a-z]|"
      "\\/search\\?|accounts\\.google|support\\.google|policies\\.google|maps\\.google|webcache|"
      "duckduckgo\\.com\\/l\\/|\\/aclk\\?|go\\.microsoft/i;",
      "    const anchors=new Set();",
      // Title heading inside the anchor (Google-style: <a><h3>).
      "    document.querySelectorAll('a[href] h1,a[href] h2,a[href] h3').forEach(h=>{"
      "      const a=h.closest('a'); if(a) anchors.add(a);});",
      // Anchor inside the title heading (Bing-style: <h2><a>).
      "    document.querySelectorAll('h1 a[href],h2 a[href],h3 a[href]').forEach(a=>anchors.add(a));",
      // Explicit result-title anchors (DuckDuckGo React results).
      "    document.querySelectorAll('a[data-testid=result-title-a]').forEach(a=>anchors.add(a));",
      "    const seen=new Set(); const out=[];",
      "    for(const a of anchors){",
      "      if(!a) continue;",
      "      let url=a.href; if(!url||!/^https?:/.test(url)) continue;",
      "      if(bad.test(url)) continue;",
      "      if(seen.has(url)) continue; seen.add(url);",
      "      const h=a.querySelector('h1,h2,h3')||a.closest('h1,h2,h3')||a;",
      "      const title=(h.innerText||a.innerText||'').trim(); if(!title) continue;",
      "      let snip='';",
      "      let blk=a.closest('div[data-hveid]')||a.closest('div.g')||a.closest('li.b_algo')||"
      "a.closest('article')||a.closest('[data-testid=result]')||a.parentElement;",
      "      if(blk){const c=blk.querySelector('div[data-sncf],.VwiC3b,[data-result=snippet],.b_caption p,"
      "[data-testid=result-snippet]'); if(c) snip=(c.innerText||'').trim();}",
      "      out.push({rank:out.length+1,title:title,url:url,snippet:snip});",
      "      if(out.length>=N) break;",
      "    }",
      "    return out;",
      "  }",
      "  const res=extract();",
      "  if(res.length>0) return JSON.stringify({query:Q,engine:E,results:res,count:res.length});",
      "  const box=document.querySelector('textarea[name=q],input[name=q],input[name=p],"
      "input#sb_form_q,input[name=query]');",
      "  const dbg='|'+location.href+'|box='+(box?1:0)+'|typed='+(window.__ab_typed?1:0);",
      "  if(box){",
      "    if(!window.__ab_typed){",
      "      window.__ab_typed=1; box.focus(); let i=0;",
      "      (function step(){",
      "        if(i<Q.length){",
      "          box.value+=Q[i];",
      "          box.dispatchEvent(new InputEvent('input',{bubbles:true,data:Q[i],inputType:'insertText'}));",
      "          box.dispatchEvent(new KeyboardEvent('keydown',{bubbles:true,key:Q[i]}));",
      "          box.dispatchEvent(new KeyboardEvent('keyup',{bubbles:true,key:Q[i]}));",
      "          i++; setTimeout(step,55+Math.random()*110);",
      "        } else {",
      "          box.dispatchEvent(new KeyboardEvent('keydown',"
      "{bubbles:true,key:'Enter',keyCode:13,which:13,code:'Enter'}));",
      "          const f=box.form||document.querySelector('form');",
      "          setTimeout(function(){ if(location.pathname.indexOf('/search')<0 && f){"
      "            if(f.requestSubmit) f.requestSubmit(); else f.submit(); } },250);",
      "        }",
      "      })();",
      "    }",
      "    return 'TYPING'+dbg;",
      "  }",
      "  return 'WAIT'+dbg;",
      "})()",
  });
}

}  // namespace autobrowse
