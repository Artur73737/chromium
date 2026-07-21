// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SERVER_H_
#define CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SERVER_H_

#include <list>
#include <memory>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "net/server/http_server.h"

namespace autobrowse {

class AutobrowseSearchRunner;
class AutobrowseFetchRunner;
class AutobrowseScrapeRunner;
class AutobrowseMonitorRunner;
class AutobrowseSessionInfoRunner;
class AutobrowseWarmupRunner;

// A single HTTP + WebSocket server that exposes every autobrowse command over
// the network. `POST /<command>` (e.g. /search) takes a JSON body and returns a
// JSON result; a WebSocket connection accepts one JSON request frame per message
// and replies with a JSON result frame. Because the commands drive the browser's
// single active tab, requests are serialized: one runs at a time, the rest queue.
// Mirrors the Obscura `octo-serve`, generalized to all commands.
class AutobrowseServer : public net::HttpServer::Delegate {
 public:
  AutobrowseServer();
  ~AutobrowseServer() override;

  AutobrowseServer(const AutobrowseServer&) = delete;
  AutobrowseServer& operator=(const AutobrowseServer&) = delete;

  // Binds to 127.0.0.1:`port` and starts serving. Returns false on failure.
  bool Start(int port);

  // net::HttpServer::Delegate:
  void OnConnect(int connection_id) override {}
  void OnHttpRequest(int connection_id,
                     const net::HttpServerRequestInfo& info) override;
  void OnWebSocketRequest(int connection_id,
                          const net::HttpServerRequestInfo& info) override;
  void OnWebSocketMessage(int connection_id, std::string data) override;
  void OnClose(int connection_id) override {}

 private:
  // A queued unit of work: run `command` with `params`, then deliver the JSON
  // string result to the originating HTTP connection or WebSocket.
  struct Job {
    std::string command;
    base::DictValue params;
    int connection_id = 0;
    bool is_websocket = false;
  };

  void Enqueue(Job job);
  void MaybeRunNext();
  bool StartCommand();  // Builds+starts the runner for current_; false if bad.
  void OnJobComplete(std::string result_json);
  void ResetRunners();
  void Reply(const Job& job, const std::string& body, bool ok);

  std::unique_ptr<net::HttpServer> server_;
  std::list<Job> queue_;
  bool busy_ = false;
  Job current_;

  // The runner for the in-flight job (exactly one is set at a time).
  std::unique_ptr<AutobrowseSearchRunner> search_runner_;
  std::unique_ptr<AutobrowseFetchRunner> fetch_runner_;
  std::unique_ptr<AutobrowseScrapeRunner> scrape_runner_;
  std::unique_ptr<AutobrowseMonitorRunner> monitor_runner_;
  std::unique_ptr<AutobrowseSessionInfoRunner> session_info_runner_;
  std::unique_ptr<AutobrowseWarmupRunner> warmup_runner_;

  base::WeakPtrFactory<AutobrowseServer> weak_factory_{this};
};

}  // namespace autobrowse

#endif  // CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SERVER_H_
