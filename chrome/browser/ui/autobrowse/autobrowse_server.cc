// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/autobrowse/autobrowse_server.h"

#include <cstdio>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/strings/string_split.h"
#include "chrome/browser/ui/autobrowse/autobrowse_fetch_runner.h"
#include "chrome/browser/ui/autobrowse/autobrowse_monitor_runner.h"
#include "chrome/browser/ui/autobrowse/autobrowse_scrape_runner.h"
#include "chrome/browser/ui/autobrowse/autobrowse_search_runner.h"
#include "chrome/browser/ui/autobrowse/autobrowse_session_info_runner.h"
#include "chrome/browser/ui/autobrowse/autobrowse_warmup_runner.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/server/http_server_request_info.h"
#include "net/server/http_server_response_info.h"
#include "net/socket/tcp_server_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"

namespace autobrowse {

namespace {

constexpr int kBacklog = 10;

net::NetworkTrafficAnnotationTag TrafficAnnotation() {
  return net::DefineNetworkTrafficAnnotation("autobrowse_server", R"(
      semantics {
        sender: "Autobrowse local server"
        description: "Local HTTP/WS server exposing autobrowse commands."
        trigger: "User starts 'autobrowse=serve'."
        data: "Command results (search results, page dumps)."
        destination: LOCAL
      }
      policy {
        cookies_allowed: NO
        setting: "Only runs when explicitly started via the command line."
      })");
}

// Extracts the command name from a path like "/search" -> "search".
std::string CommandFromPath(const std::string& path) {
  std::string p = path;
  while (!p.empty() && p.front() == '/') {
    p.erase(0, 1);
  }
  const size_t q = p.find('?');
  if (q != std::string::npos) {
    p = p.substr(0, q);
  }
  return p;
}

}  // namespace

AutobrowseServer::AutobrowseServer() = default;
AutobrowseServer::~AutobrowseServer() = default;

bool AutobrowseServer::Start(int port) {
  auto socket =
      std::make_unique<net::TCPServerSocket>(nullptr, net::NetLogSource());
  if (socket->ListenWithAddressAndPort("127.0.0.1", port, kBacklog) != net::OK) {
    fprintf(stderr, "[autobrowse] serve: cannot bind 127.0.0.1:%d\n", port);
    return false;
  }
  server_ = std::make_unique<net::HttpServer>(std::move(socket), this);
  fprintf(stderr,
          "[autobrowse] serving on http://127.0.0.1:%d  "
          "(POST /search, /fetch, ... ; WS same host)\n",
          port);
  return true;
}

void AutobrowseServer::OnHttpRequest(int connection_id,
                                     const net::HttpServerRequestInfo& info) {
  if (info.method == "GET" &&
      (info.path == "/health" || info.path == "/")) {
    server_->Send200(connection_id, "ok\n", "text/plain", TrafficAnnotation());
    return;
  }
  if (info.method != "POST") {
    server_->Send404(connection_id, TrafficAnnotation());
    return;
  }

  Job job;
  job.command = CommandFromPath(info.path);
  job.connection_id = connection_id;
  job.is_websocket = false;

  std::optional<base::Value> body =
      base::JSONReader::Read(info.data, base::JSON_PARSE_RFC);
  if (body && body->is_dict()) {
    job.params = std::move(body->GetDict());
  }
  Enqueue(std::move(job));
}

void AutobrowseServer::OnWebSocketRequest(
    int connection_id,
    const net::HttpServerRequestInfo& info) {
  server_->AcceptWebSocket(connection_id, info, TrafficAnnotation());
}

void AutobrowseServer::OnWebSocketMessage(int connection_id, std::string data) {
  Job job;
  job.connection_id = connection_id;
  job.is_websocket = true;

  std::optional<base::Value> msg =
      base::JSONReader::Read(data, base::JSON_PARSE_RFC);
  if (msg && msg->is_dict()) {
    base::DictValue& d = msg->GetDict();
    if (const std::string* c = d.FindString("command")) {
      job.command = *c;
    }
    job.params = std::move(d);
  }
  Enqueue(std::move(job));
}

void AutobrowseServer::Enqueue(Job job) {
  queue_.push_back(std::move(job));
  MaybeRunNext();
}

void AutobrowseServer::MaybeRunNext() {
  if (busy_ || queue_.empty()) {
    return;
  }
  busy_ = true;
  current_ = std::move(queue_.front());
  queue_.pop_front();
  if (!StartCommand()) {
    // StartCommand delivered an error synchronously via OnJobComplete.
  }
}

bool AutobrowseServer::StartCommand() {
  const base::DictValue& p = current_.params;
  auto cb = base::BindOnce(&AutobrowseServer::OnJobComplete,
                           weak_factory_.GetWeakPtr());

  auto str = [&p](const char* k, const char* def) -> std::string {
    const std::string* v = p.FindString(k);
    return v ? *v : std::string(def);
  };
  auto num = [&p](const char* k, int def) -> int {
    return p.FindInt(k).value_or(def);
  };

  const std::string& cmd = current_.command;

  if (cmd == "search") {
    const std::string q = str("query", "");
    if (q.empty()) {
      OnJobComplete(R"({"error":"missing 'query'"})");
      return false;
    }
    search_runner_ = std::make_unique<AutobrowseSearchRunner>(
        str("engine", "google"), q, num("max_results", 10), base::FilePath());
    search_runner_->SetCompletionCallback(std::move(cb));
    search_runner_->Start();
    return true;
  }

  if (cmd == "fetch") {
    const std::string url = str("url", "");
    if (url.empty()) {
      OnJobComplete(R"({"error":"missing 'url'"})");
      return false;
    }
    fetch_runner_ = std::make_unique<AutobrowseFetchRunner>(
        url, str("dump", ""), str("eval", ""), str("selector", ""),
        str("wait_until", ""), num("wait", 0), num("timeout", 30),
        base::FilePath());
    fetch_runner_->SetCompletionCallback(std::move(cb));
    fetch_runner_->Start();
    return true;
  }

  if (cmd == "scrape") {
    std::vector<std::string> urls = base::SplitString(
        str("urls", ""), ", ", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    if (urls.empty()) {
      OnJobComplete(R"({"error":"missing 'urls'"})");
      return false;
    }
    scrape_runner_ = std::make_unique<AutobrowseScrapeRunner>(
        std::move(urls), str("dump", ""), str("eval", ""), num("wait", 0),
        num("timeout", 30), base::FilePath());
    scrape_runner_->SetCompletionCallback(std::move(cb));
    scrape_runner_->Start();
    return true;
  }

  if (cmd == "monitor") {
    const std::string url = str("url", "");
    if (url.empty()) {
      OnJobComplete(R"({"error":"missing 'url'"})");
      return false;
    }
    // Over the server, monitor does a single poll and returns the value.
    monitor_runner_ = std::make_unique<AutobrowseMonitorRunner>(
        url, str("selector", ""), str("on_change", ""), /*interval=*/60,
        /*max_runs=*/1, base::FilePath());
    monitor_runner_->SetCompletionCallback(std::move(cb));
    monitor_runner_->Start();
    return true;
  }

  if (cmd == "session-info") {
    session_info_runner_ =
        std::make_unique<AutobrowseSessionInfoRunner>(num("top", 15));
    session_info_runner_->SetCompletionCallback(std::move(cb));
    session_info_runner_->Start();
    return true;
  }

  if (cmd == "warmup") {
    std::vector<std::string> queries = base::SplitString(
        str("query", ""), ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    if (queries.empty()) {
      queries = {"rust programming", "weather today", "latest news"};
    }
    std::vector<std::string> urls = base::SplitString(
        str("urls", ""), ", ", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    double minutes = 15.0;
    if (const base::Value* v = p.Find("minutes")) {
      if (v->is_double() || v->is_int()) {
        minutes = v->GetDouble();
      }
    }
    warmup_runner_ = std::make_unique<AutobrowseWarmupRunner>(
        str("engine", "bing"), std::move(queries), std::move(urls), minutes);
    warmup_runner_->SetCompletionCallback(std::move(cb));
    warmup_runner_->Start();
    return true;
  }

  OnJobComplete(R"({"error":"unknown command"})");
  return false;
}

void AutobrowseServer::ResetRunners() {
  search_runner_.reset();
  fetch_runner_.reset();
  scrape_runner_.reset();
  monitor_runner_.reset();
  session_info_runner_.reset();
  warmup_runner_.reset();
}

void AutobrowseServer::OnJobComplete(std::string result_json) {
  Reply(current_, result_json, /*ok=*/true);
  busy_ = false;
  // Destroy the just-finished runner on a fresh task: it is still on the stack
  // here (it invoked this callback), so freeing it now would be a UAF.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&AutobrowseServer::ResetRunners,
                                weak_factory_.GetWeakPtr()));
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&AutobrowseServer::MaybeRunNext,
                                weak_factory_.GetWeakPtr()));
}

void AutobrowseServer::Reply(const Job& job,
                             const std::string& body,
                             bool ok) {
  if (!server_) {
    return;
  }
  if (job.is_websocket) {
    server_->SendOverWebSocket(job.connection_id, body, TrafficAnnotation());
  } else {
    server_->Send200(job.connection_id, body, "application/json",
                     TrafficAnnotation());
  }
}

}  // namespace autobrowse
