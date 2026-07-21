// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/autobrowse/autobrowse_monitor_runner.h"

#include "chrome/browser/ui/autobrowse/autobrowse_stealth.h"

#include <cstdio>
#include <functional>
#include <utility>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/json/string_escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
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

AutobrowseMonitorRunner::AutobrowseMonitorRunner(std::string url,
                                                 std::string selector,
                                                 std::string on_change,
                                                 int interval_seconds,
                                                 int max_runs,
                                                 base::FilePath output_path)
    : url_(std::move(url)),
      selector_(std::move(selector)),
      on_change_(std::move(on_change)),
      interval_seconds_(interval_seconds),
      max_runs_(max_runs),
      output_path_(std::move(output_path)) {}

AutobrowseMonitorRunner::~AutobrowseMonitorRunner() = default;

void AutobrowseMonitorRunner::Start() {
  TryAttach();
  if (!attached_) {
    attach_timer_.Start(FROM_HERE, kAttachInterval,
                        base::BindRepeating(&AutobrowseMonitorRunner::TryAttach,
                                            weak_factory_.GetWeakPtr()));
  }
}

void AutobrowseMonitorRunner::TryAttach() {
  if (attached_ || stopped_) {
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
                          base::BindOnce(&AutobrowseMonitorRunner::LoadOnce,
                                         weak_factory_.GetWeakPtr()));
}

void AutobrowseMonitorRunner::LoadOnce() {
  if (stopped_ || !web_contents()) {
    return;
  }
  extracted_this_poll_ = false;
  content::NavigationController::LoadURLParams load_params{GURL(url_)};
  load_params.transition_type = ui::PAGE_TRANSITION_RELOAD;
  web_contents()->GetController().LoadURLWithParams(load_params);
}

void AutobrowseMonitorRunner::DocumentOnLoadCompletedInPrimaryMainFrame() {
  RunExtract();
}

void AutobrowseMonitorRunner::RunExtract() {
  if (stopped_ || extracted_this_poll_ || !web_contents()) {
    return;
  }
  content::RenderFrameHost* rfh = web_contents()->GetPrimaryMainFrame();
  if (!rfh) {
    return;
  }
  extracted_this_poll_ = true;
  rfh->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(BuildExtractScript()),
      base::BindOnce(&AutobrowseMonitorRunner::OnExtractResult,
                     weak_factory_.GetWeakPtr()),
      ISOLATED_WORLD_ID_CHROME_INTERNAL);
}

void AutobrowseMonitorRunner::OnExtractResult(base::Value value) {
  if (stopped_) {
    return;
  }
  ++runs_;

  std::string value_str;
  if (value.is_string()) {
    value_str = value.GetString();
  } else {
    base::JSONWriter::Write(value, &value_str);
  }

  const size_t hash = std::hash<std::string>{}(value_str);
  const bool changed = !have_last_ || hash != last_hash_;
  if (changed) {
    have_last_ = true;
    last_hash_ = hash;

    base::DictValue rec;
    rec.Set("t", base::NumberToString(
                     base::Time::Now().InMillisecondsSinceUnixEpoch()));
    rec.Set("url", url_);
    rec.Set("value", value_str);
    std::string line;
    base::JSONWriter::Write(base::Value(std::move(rec)), &line);

    if (output_path_.empty()) {
      fprintf(stdout, "%s\n", line.c_str());
      fflush(stdout);
    } else {
      line += "\n";
      base::AppendToFile(output_path_, line);
    }
  }

  ScheduleNextOrStop();
}

void AutobrowseMonitorRunner::ScheduleNextOrStop() {
  if (max_runs_ > 0 && runs_ >= max_runs_) {
    Stop();
    return;
  }
  interval_timer_.Start(FROM_HERE, base::Seconds(interval_seconds_),
                        base::BindOnce(&AutobrowseMonitorRunner::LoadOnce,
                                       weak_factory_.GetWeakPtr()));
}

void AutobrowseMonitorRunner::Stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  attach_timer_.Stop();
  interval_timer_.Stop();
  Observe(nullptr);
  const bool headless =
      base::CommandLine::ForCurrentProcess()->HasSwitch("headless");
  if (headless) {
    chrome::AttemptExit();
  }
}

std::string AutobrowseMonitorRunner::BuildExtractScript() const {
  std::string selector_js;
  base::EscapeJSONString(selector_, /*put_in_quotes=*/true, &selector_js);
  const std::string on_change = on_change_.empty() ? "textContent" : on_change_;

  // Resolve the element (selector or <body>), then evaluate the on-change
  // expression with the element in scope so a bare `textContent` works.
  return base::StrCat({
      "(function(){",
      "  var sel=", selector_js, ";",
      "  var el=sel?document.querySelector(sel):document.body;",
      "  if(!el) return null;",
      "  try{ with(el){ return (", on_change, "); } }",
      "  catch(e){ return 'error: '+e; }",
      "})()",
  });
}

}  // namespace autobrowse
