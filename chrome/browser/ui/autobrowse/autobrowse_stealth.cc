// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/autobrowse/autobrowse_stealth.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "base/json/string_escape.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client.h"
#include "content/public/browser/web_contents.h"

namespace autobrowse {

namespace {

// The anti-fingerprinting payload, injected at document start in the main world.
// It patches the highest-signal automation/headless tells so a page's own
// scripts see a normal desktop Chrome. Values are internally consistent.
const char kStealthScript[] = R"JS(
(function(){
  try {
    // navigator.webdriver -> undefined (matches real Chrome).
    try { Object.defineProperty(Navigator.prototype, 'webdriver',
      { get: () => undefined, configurable: true }); } catch(e){}

    // A realistic plugin/mimeType set (headless has none).
    const mk = (name, filename, desc) => {
      const p = Object.create(Plugin.prototype);
      Object.defineProperties(p, {
        name: { value: name }, filename: { value: filename },
        description: { value: desc }, length: { value: 1 },
      });
      return p;
    };
    const plugins = [
      mk('PDF Viewer', 'internal-pdf-viewer', 'Portable Document Format'),
      mk('Chrome PDF Viewer', 'internal-pdf-viewer', 'Portable Document Format'),
      mk('Chromium PDF Viewer', 'internal-pdf-viewer', 'Portable Document Format'),
      mk('Microsoft Edge PDF Viewer', 'internal-pdf-viewer', 'Portable Document Format'),
      mk('WebKit built-in PDF', 'internal-pdf-viewer', 'Portable Document Format'),
    ];
    try {
      Object.defineProperty(Navigator.prototype, 'plugins',
        { get: () => plugins, configurable: true });
    } catch(e){}

    // languages: ensure a plausible non-empty list.
    try {
      Object.defineProperty(Navigator.prototype, 'languages',
        { get: () => ['en-US', 'en'], configurable: true });
    } catch(e){}

    // window.chrome runtime stub (present in real Chrome, absent in headless).
    if (!window.chrome) { window.chrome = {}; }
    if (!window.chrome.runtime) { window.chrome.runtime = {}; }

    // Notification permission consistency (headless returns 'denied' oddly).
    try {
      const orig = window.Notification;
      if (orig) {
        Object.defineProperty(orig, 'permission',
          { get: () => 'default', configurable: true });
      }
      const q = navigator.permissions && navigator.permissions.query;
      if (q) {
        navigator.permissions.query = (params) =>
          params && params.name === 'notifications'
            ? Promise.resolve({ state: Notification.permission })
            : q.call(navigator.permissions, params);
      }
    } catch(e){}

    // hardwareConcurrency / deviceMemory: realistic desktop values.
    try { Object.defineProperty(Navigator.prototype, 'hardwareConcurrency',
      { get: () => 8, configurable: true }); } catch(e){}
    try { Object.defineProperty(Navigator.prototype, 'deviceMemory',
      { get: () => 8, configurable: true }); } catch(e){}

    // WebGL vendor/renderer: report a common real GPU instead of SwiftShader.
    const patchGL = (proto) => {
      if (!proto) return;
      const gp = proto.getParameter;
      proto.getParameter = function(p) {
        if (p === 37445) return 'Google Inc. (Intel)';           // UNMASKED_VENDOR
        if (p === 37446) return 'ANGLE (Intel, Intel(R) UHD Graphics Direct3D11 vs_5_0 ps_5_0)'; // UNMASKED_RENDERER
        return gp.call(this, p);
      };
    };
    try { patchGL(window.WebGLRenderingContext && WebGLRenderingContext.prototype); } catch(e){}
    try { patchGL(window.WebGL2RenderingContext && WebGL2RenderingContext.prototype); } catch(e){}
  } catch (e) { /* never break the page */ }
})();
)JS";

// One DevTools client per WebContents; kept alive for the process lifetime.
class StealthClient : public content::DevToolsAgentHostClient {
 public:
  StealthClient(content::WebContents* wc, base::OnceClosure on_ready)
      : on_ready_(std::move(on_ready)) {
    agent_host_ = content::DevToolsAgentHost::GetOrCreateFor(wc);
    if (!agent_host_) {
      RunReady();
      return;
    }
    agent_host_->AttachClient(this);
    Send(R"({"id":1,"method":"Page.enable"})");
    std::string source;
    base::EscapeJSONString(kStealthScript, /*put_in_quotes=*/true, &source);
    Send(base::StrCat(
        {R"({"id":2,"method":"Page.addScriptToEvaluateOnNewDocument",)"
         R"("params":{"source":)",
         source, "}}"}));
  }

  StealthClient(const StealthClient&) = delete;
  StealthClient& operator=(const StealthClient&) = delete;
  ~StealthClient() override = default;

  // content::DevToolsAgentHostClient:
  void DispatchProtocolMessage(content::DevToolsAgentHost*,
                               base::span<const uint8_t> msg) override {
    const std::string text(msg.begin(), msg.end());
    if (getenv("AB_DEBUG")) {
      fprintf(stderr, "[stealth] devtools <- %s\n", text.c_str());
    }
    // The reply to command id:2 (addScriptToEvaluateOnNewDocument) means the
    // script is registered; it is now safe to navigate.
    if (text.find("\"id\":2") != std::string::npos) {
      RunReady();
    }
  }
  void AgentHostClosed(content::DevToolsAgentHost*) override { RunReady(); }
  // Trusted so privileged protocol commands are allowed.
  bool IsTrusted() override { return true; }

 private:
  void Send(const std::string& message) {
    if (agent_host_) {
      agent_host_->DispatchProtocolMessage(this, base::as_byte_span(message));
    }
  }

  void RunReady() {
    if (on_ready_) {
      std::move(on_ready_).Run();
    }
  }

  scoped_refptr<content::DevToolsAgentHost> agent_host_;
  base::OnceClosure on_ready_;
};

std::vector<std::unique_ptr<StealthClient>>& Clients() {
  static base::NoDestructor<std::vector<std::unique_ptr<StealthClient>>> c;
  return *c;
}

}  // namespace

void InstallStealthIfEnabled(content::WebContents* web_contents,
                             base::OnceClosure on_ready) {
  if (!web_contents ||
      !base::CommandLine::ForCurrentProcess()->HasSwitch("stealth")) {
    if (on_ready) {
      std::move(on_ready).Run();
    }
    return;
  }
  Clients().push_back(
      std::make_unique<StealthClient>(web_contents, std::move(on_ready)));
}

}  // namespace autobrowse
