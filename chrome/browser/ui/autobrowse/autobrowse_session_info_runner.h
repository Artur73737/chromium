// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SESSION_INFO_RUNNER_H_
#define CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SESSION_INFO_RUNNER_H_

#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"

namespace net {
class CanonicalCookie;
}

namespace autobrowse {

// Prints a human-readable summary of the current profile's cookie jar (the
// --user-data-dir session): total cookies, distinct domains, and a breakdown of
// persistent / session / secure / expired, plus the top domains by count.
// Mirrors the Obscura `session-info` command. Reads cookies via the profile's
// CookieManager (no navigation, no API).
class AutobrowseSessionInfoRunner {
 public:
  explicit AutobrowseSessionInfoRunner(int top);
  ~AutobrowseSessionInfoRunner();

  void Start();

  // Server mode: deliver the summary text here instead of printing/exiting.
  void SetCompletionCallback(base::OnceCallback<void(std::string)> cb) {
    on_complete_ = std::move(cb);
  }

 private:
  void OnGotCookies(const std::vector<net::CanonicalCookie>& cookies);
  void Deliver(const std::string& text);

  base::OnceCallback<void(std::string)> on_complete_;
  const int top_;
  base::WeakPtrFactory<AutobrowseSessionInfoRunner> weak_factory_{this};
};

}  // namespace autobrowse

#endif  // CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SESSION_INFO_RUNNER_H_
