// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/autobrowse/autobrowse_server.h"

#include <cstdio>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/ui/autobrowse/autobrowse_search_runner.h"
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

  if (current_.command == "search") {
    const std::string* q = current_.params.FindString("query");
    if (!q || q->empty()) {
      OnJobComplete(R"({"error":"missing 'query'"})");
      return;
    }
    std::string engine = "google";
    if (const std::string* e = current_.params.FindString("engine")) {
      engine = *e;
    }
    int max_results = 10;
    if (std::optional<int> n = current_.params.FindInt("max_results")) {
      max_results = *n;
    }
    search_runner_ = std::make_unique<AutobrowseSearchRunner>(
        engine, *q, max_results, base::FilePath());
    search_runner_->SetCompletionCallback(
        base::BindOnce(&AutobrowseServer::OnJobComplete,
                       weak_factory_.GetWeakPtr()));
    search_runner_->Start();
    return;
  }

  OnJobComplete(R"({"error":"unknown command"})");
}

void AutobrowseServer::OnJobComplete(std::string result_json) {
  Reply(current_, result_json, /*ok=*/true);
  search_runner_.reset();
  busy_ = false;
  MaybeRunNext();
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
