// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_STEALTH_H_
#define CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_STEALTH_H_

#include "base/functional/callback_forward.h"

namespace content {
class WebContents;
}

namespace autobrowse {

// If --stealth is present on the command line, installs an anti-fingerprinting
// script that runs at document start in the main world of every new document in
// `web_contents` (via an in-process DevTools session and
// Page.addScriptToEvaluateOnNewDocument), so the automation is indistinguishable
// from a normal browser. `on_ready` is run once the script is registered (so the
// caller can navigate and have the very first document already patched). Without
// --stealth this installs nothing and runs `on_ready` immediately.
void InstallStealthIfEnabled(content::WebContents* web_contents,
                             base::OnceClosure on_ready);

}  // namespace autobrowse

#endif  // CHROME_BROWSER_UI_AUTOBROWSE_AUTOBROWSE_STEALTH_H_
