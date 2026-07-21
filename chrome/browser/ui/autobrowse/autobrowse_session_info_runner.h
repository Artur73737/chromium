// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SESSION_INFO_RUNNER_H_
#define CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SESSION_INFO_RUNNER_H_

#include <vector>

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

 private:
  void OnGotCookies(const std::vector<net::CanonicalCookie>& cookies);

  const int top_;
  base::WeakPtrFactory<AutobrowseSessionInfoRunner> weak_factory_{this};
};

}  // namespace autobrowse

#endif  // CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_SESSION_INFO_RUNNER_H_
