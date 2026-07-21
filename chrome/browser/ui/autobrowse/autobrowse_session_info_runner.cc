// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/autobrowse/autobrowse_session_info_runner.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "content/public/browser/storage_partition.h"
#include "net/cookies/canonical_cookie.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"

namespace autobrowse {

AutobrowseSessionInfoRunner::AutobrowseSessionInfoRunner(int top) : top_(top) {}

AutobrowseSessionInfoRunner::~AutobrowseSessionInfoRunner() = default;

void AutobrowseSessionInfoRunner::Start() {
  Profile* profile = ProfileManager::GetLastUsedProfileIfLoaded();
  if (!profile) {
    fprintf(stderr, "[autobrowse] session-info: no active profile\n");
    chrome::AttemptExit();
    return;
  }
  network::mojom::CookieManager* cookie_manager =
      profile->GetDefaultStoragePartition()
          ->GetCookieManagerForBrowserProcess();
  if (!cookie_manager) {
    fprintf(stderr, "[autobrowse] session-info: no cookie manager\n");
    chrome::AttemptExit();
    return;
  }
  cookie_manager->GetAllCookies(
      base::BindOnce(&AutobrowseSessionInfoRunner::OnGotCookies,
                     weak_factory_.GetWeakPtr()));
}

void AutobrowseSessionInfoRunner::OnGotCookies(
    const std::vector<net::CanonicalCookie>& cookies) {
  const base::Time now = base::Time::Now();
  int persistent = 0, session = 0, secure = 0, expired = 0;
  std::map<std::string, int> per_domain;

  for (const net::CanonicalCookie& c : cookies) {
    if (c.IsPersistent()) {
      ++persistent;
    } else {
      ++session;
    }
    if (c.SecureAttribute()) {
      ++secure;
    }
    if (c.IsExpired(now)) {
      ++expired;
    }
    std::string domain = c.Domain();
    if (!domain.empty() && domain[0] == '.') {
      domain.erase(0, 1);
    }
    ++per_domain[domain];
  }

  fprintf(stdout, "cookies: %zu total (%zu domains) - %d persistent, "
                  "%d session, %d secure, %d expired\n",
          cookies.size(), per_domain.size(), persistent, session, secure,
          expired);

  // Top domains by cookie count.
  std::vector<std::pair<std::string, int>> sorted(per_domain.begin(),
                                                  per_domain.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  const int top = top_ > 0 ? top_ : 15;
  fprintf(stdout, "top domains:\n");
  int shown = 0;
  for (const auto& [domain, count] : sorted) {
    if (shown >= top) {
      break;
    }
    fprintf(stdout, "  %5d  %s\n", count, domain.c_str());
    ++shown;
  }
  const int remaining = static_cast<int>(sorted.size()) - shown;
  if (remaining > 0) {
    fprintf(stdout, "  ... and %d more\n", remaining);
  }
  fflush(stdout);

  chrome::AttemptExit();
}

}  // namespace autobrowse
