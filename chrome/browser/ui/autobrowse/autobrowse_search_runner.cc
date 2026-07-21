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
    Finish(s);
  }
  // "CONSENT" / "TYPING" / "WAIT": keep polling until a final JSON arrives.
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
  } else {
    if (!base::WriteFile(output_path_, out)) {
      fprintf(stderr, "[autobrowse] failed to write output file: %s\n",
              output_path_.AsUTF8Unsafe().c_str());
    }
  }

  // In headless mode there is no visible UI, so the run is only useful for its
  // stdout/file output: exit once done. With a visible GUI, keep the window
  // open on the results page so the user can inspect it; the search is over but
  // the browser stays alive like a normal session.
  const bool headless =
      base::CommandLine::ForCurrentProcess()->HasSwitch("headless");
  if (headless) {
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
