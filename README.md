# ![Logo](chrome/app/theme/chromium/product_logo_64.png) Chromium

Chromium is an open-source browser project that aims to build a safer, faster,
and more stable way for all users to experience the web.

The project's web site is https://www.chromium.org.

To check out the source code locally, don't use `git clone`! Instead,
follow [the instructions on how to get the code](docs/get_the_code.md).

Documentation in the source is rooted in [docs/README.md](docs/README.md).

Learn how to [Get Around the Chromium Source Code Directory
Structure](https://www.chromium.org/developers/how-tos/getting-around-the-chrome-source-code).

For historical reasons, there are some small top level directories. Now the
guidance is that new top level directories are for product (e.g. Chrome,
Android WebView, Ash). Even if these products have multiple executables, the
code should be in subdirectories of the product.

If you found a bug, please file it at https://crbug.com/new.

---
---

# Autobrowse — Obscura, nativo in Chromium (branch `octo`)

**Autobrowse** porta l'intera superficie di comandi di *Obscura* dentro
Chromium, **in C++ nativo**. La ricerca e la navigazione sono **umane**: la
query viene digitata nella barra di ricerca **lettera per lettera** (eventi
`InputEvent`/`KeyboardEvent`), si preme Invio, e i link dei risultati vengono
letti dal DOM. **Nessuna API** di Google/Bing/DuckDuckGo viene mai usata (le API
si fanno bloccare e tradiscono il bot). Ogni comando funziona **sia con GUI
visibile sia in headless**, con lo **stesso identico fingerprint**.

Tutto il codice nuovo vive in **`chrome/browser/ui/autobrowse/`**.

Indice:
1. [Come si compila (build)](#1-come-si-compila-build)
2. [Come si lancia — le 3 superfici](#2-come-si-lancia--le-3-superfici)
3. [Comandi CLI (uno per uno, tutti gli switch)](#3-comandi-cli)
4. [Fingerprint: headless identico alla GUI](#4-fingerprint-headless-identico-alla-gui)
5. [Stealth mode — come funziona lo spoofing](#5-stealth-mode--come-funziona-lo-spoofing)
6. [Cookie consent: Reject all](#6-cookie-consent-reject-all)
7. [Server HTTP + WebSocket](#7-server-http--websocket)
8. [Server MCP](#8-server-mcp)
9. [Tabella completa degli switch](#9-tabella-completa-degli-switch)
10. [Architettura interna e file](#10-architettura-interna-e-file)

---

## 1. Come si compila (build)

Toolchain: Visual Studio 2022 + depot_tools. L'ambiente è preconfigurato in
`env.bat` (workspace root, cioè `E:\project-seri\chromium`), che mette
depot_tools in testa al `PATH`, forza `DEPOT_TOOLS_WIN_TOOLCHAIN=0` (VS locale)
e sposta TMP/cache sul disco E.

### Build con lo script (consigliato)

```bat
:: dalla root del workspace: E:\project-seri\chromium
build.bat
```

`build.bat` alla prima esecuzione lancia `gn gen out\Default` con questi args:

```
is_debug=false is_component_build=true symbol_level=1 blink_symbol_level=0
chrome_pgo_phase=0 enable_nacl=false use_remoteexec=false
```

poi compila con `autoninja -C out\Default chrome -j 6` (il `-j 6` tiene la RAM
sotto controllo su 16 GB). La prima build dura ore (V8 da sorgente); le build
incrementali dopo una modifica durano 1–3 minuti. L'eseguibile finale è:

```
E:\project-seri\chromium\src\out\Default\chrome.exe
```

### Build a mano (equivalente)

```bat
call E:\project-seri\chromium\env.bat
cd /d E:\project-seri\chromium\src
autoninja -C out\Default chrome -j 6
```

### Nota shell (git-bash / MSYS)

git-bash **converte** gli argomenti che iniziano con `/` (es. `/c`) in path
Windows, rompendo `cmd.exe`. Per invocare `cmd.exe` da git-bash anteporre
`MSYS_NO_PATHCONV=1`, e chiamare i `.bat` con `call` (altrimenti il primo `.bat`
termina la catena):

```bash
MSYS_NO_PATHCONV=1 cmd.exe /c "call E:\project-seri\chromium\env.bat && \
  cd /d E:\project-seri\chromium\src && call autoninja -C out\Default chrome -j 6"
```

### Debug logging

Le run di autobrowse silenziano i log di servizio così lo stdout resta pulito.
Per vedere il traffico interno (stati del driver di ricerca, ack DevTools dello
stealth) esporta `AB_DEBUG=1` prima di lanciare.

---

## 2. Come si lancia — le 3 superfici

Ci sono **tre superfici distinte** per pilotare il browser:

| Superficie | Come | Note |
|---|---|---|
| **CLI** | `--autobrowse=<cmd>` + switch `--ab-*` | i comandi qui sotto |
| **CDP** | `--remote-debugging-port=9222` | **già nativo**, per Puppeteer/Playwright; separato |
| **HTTP + WS + MCP** | `--autobrowse=serve --ab-port=8080` | un solo server per tutti i comandi |

### Forma dei comandi CLI

Due forme equivalenti:

* **Sub-comando unico** (preferito): `--autobrowse=<cmd>` con input via `--ab-*`.
* **Scorciatoia legacy** per la ricerca: `--autobrowse-search="query"`.

L'ultimo argomento è sempre una pagina qualsiasi, es. `about:blank` (serve solo
a far aprire un tab su cui agganciarsi).

### GUI vs headless

* **GUI visibile**: ometti `--headless`. A fine comando la **finestra resta
  aperta** sulla pagina/risultati (search e fetch).
* **Headless**: aggiungi `--headless` (o `--headless=new`). A fine comando il
  processo **esce** dopo aver stampato l'output.

Il comportamento (typing umano, estrazione, ecc.) è **identico** nelle due
modalità; cambia solo la visibilità della finestra e l'uscita del processo.

### Sessione / profilo

Il profilo `--user-data-dir=DIR` **è** la sessione: i cookie persistono tra i
run (equivalente al `--session` di Obscura). Un profilo "caldo" (con cookie
dell'engine già presenti) alza molto l'affidabilità anti-bot. Un profilo freddo
su Google può incontrare consent-wall/captcha per reputazione IP (datacenter).

### Flag globali nativi utili

| Flag nativo | Equivalente Obscura |
|---|---|
| `--proxy-server="socks5://127.0.0.1:1080"` | `--proxy` |
| `--user-agent="..."` | `--user-agent` |
| `--user-data-dir=DIR` | `--session DIR` |
| `--remote-debugging-port=9222` | `serve` (CDP) |
| `--stealth` | `--stealth` (vedi §5) |
| `--headless` | headless |

---

## 3. Comandi CLI

### `search` — ricerca umana

Naviga la home del motore, digita la query **lettera per lettera**, preme Invio,
gestisce consent-wall e captcha, ed estrae i primi risultati (titolo, url,
snippet). Output JSON Obscura-style. In GUI la finestra resta sui risultati.

| Switch | Default | Descrizione |
|---|---|---|
| `--autobrowse=search` / `--autobrowse-search="Q"` | — | Attiva; la 2ª forma passa anche la query. |
| `--ab-query="Q"` | — | Query (se non passata dalla forma legacy). |
| `--ab-engine=<e>` / `--autobrowse-engine=<e>` | `google` | `google` \| `bing` \| `duckduckgo`. |
| `--ab-max-results=N` / `--autobrowse-max-results=N` | `10` | Numero massimo di risultati. |
| `--ab-output=FILE` / `--autobrowse-output=FILE` | stdout | Scrive il JSON su file. |

```bash
# GUI, Google (resta aperta sui risultati)
chrome.exe --user-data-dir="E:/tmp/ab" \
  --autobrowse-search="rust tokio" --autobrowse-engine=google

# headless, profilo caldo
chrome.exe --headless --user-data-dir="E:/tmp/ab" \
  --autobrowse=search --ab-query="rust tokio" --ab-engine=google --ab-max-results=5
```

Output:
```json
{
   "count": 5,
   "engine": "google",
   "query": "rust tokio",
   "results": [
      { "rank": 1, "title": "Tokio - An asynchronous Rust runtime",
        "url": "https://tokio.rs/", "snippet": "Tokio is an asynchronous runtime…" }
   ],
   "took_ms": 5807
}
```

In caso di muro il JSON riporta `"error"` (`blocked` / `timeout`) e `"reason"`.
Vedi [§6](#6-cookie-consent-reject-all) per la gestione del consenso cookie.

### `fetch` — carica una pagina, dump o eval

Naviga a un URL, attende il caricamento (+ settle opzionale), poi **valuta una
espressione JS** (`--ab-eval`, in isolated world) oppure **dumpa** la pagina
renderizzata.

| Switch | Default | Descrizione |
|---|---|---|
| `--autobrowse=fetch` | — | Attiva il comando. |
| `--ab-url=URL` | — (**obbligatorio**) | Pagina da caricare. |
| `--ab-eval="EXPR"` | — | Espressione JS; ne stampa il valore. Ha priorità su `--ab-dump`. |
| `--ab-dump=<fmt>` | `html` | `html` \| `text` \| `links` \| `markdown`. |
| `--ab-wait=SEC` | `0` | Attesa extra dopo il load, per contenuti async. |
| `--ab-timeout=SEC` | `30` | Timeout di navigazione. |
| `--ab-output=FILE` | stdout | Scrive l'output su file. |

`--ab-dump`: `html` = outerHTML del documento renderizzato; `text` =
`body.innerText`; `links` = un `{text, href}` JSON per riga; `markdown` =
DOM→Markdown (heading, liste, link, code).

```bash
chrome.exe --headless --autobrowse=fetch --ab-url="https://example.com" --ab-eval="document.title"
chrome.exe --headless --autobrowse=fetch --ab-url="https://example.com" --ab-dump=text
chrome.exe --headless --autobrowse=fetch --ab-url="https://example.com" --ab-dump=links
chrome.exe --headless --autobrowse=fetch --ab-url="https://example.com" --ab-dump=markdown
```

### `scrape` — molti URL, un eval/dump ciascuno

Naviga in sequenza una lista di URL nel tab attivo; per ognuno attende il
render, valuta `--ab-eval` (o dumpa con `--ab-dump`) e raccoglie un record.
Emette un JSON array. Un URL lento/rotto va in `"error":"timeout"` e non blocca
il resto.

| Switch | Default | Descrizione |
|---|---|---|
| `--autobrowse=scrape` | — | Attiva il comando. |
| `--ab-urls="u1,u2,..."` | — (**obbligatorio**) | URL separati da virgola/spazio. |
| `--ab-eval="EXPR"` | — | JS valutato su ogni pagina. Priorità su `--ab-dump`. |
| `--ab-dump=<fmt>` | `text` | `text` \| `html` \| `links`. |
| `--ab-wait=SEC` | `0` | Settle extra per pagina. |
| `--ab-timeout=SEC` | `30` | Timeout per pagina. |
| `--ab-output=FILE` | stdout | Scrive su file. |

```bash
chrome.exe --headless --autobrowse=scrape \
  --ab-urls="https://example.com,https://example.org" --ab-eval="document.title"
```
```json
{
   "count": 2,
   "results": [
      { "url": "https://example.com", "result": "Example Domain" },
      { "url": "https://example.org", "result": "Example Domain" }
   ],
   "took_ms": 2916
}
```

### `monitor` — watch pagina, NDJSON sui cambiamenti

Ricarica l'URL a intervallo, estrae un valore da `--ab-selector` valutando
`--ab-on-change` (con l'elemento in scope, così `textContent` funziona nudo),
ne calcola l'hash ed emette una riga NDJSON **solo quando cambia**.

| Switch | Default | Descrizione |
|---|---|---|
| `--autobrowse=monitor` | — | Attiva il comando. |
| `--ab-url=URL` | — (**obbligatorio**) | Pagina da osservare. |
| `--ab-selector=CSS` | `<body>` | Elemento da osservare. |
| `--ab-on-change="JS"` | `textContent` | JS che produce il valore (elemento in scope). |
| `--ab-interval=SEC` | `60` | Intervallo di polling. |
| `--ab-max-runs=N` | `0` (sempre) | Stop dopo N poll. |
| `--ab-output=FILE` | stdout | Appende le righe NDJSON su file. |

```bash
chrome.exe --headless --autobrowse=monitor --ab-url="https://example.com" \
  --ab-selector="h1" --ab-interval=30 --ab-max-runs=5
```
```json
{"t":"1784658645703","url":"https://example.com","value":"Example Domain"}
```

### `session-info` — riepilogo del cookie jar

Stampa un riepilogo leggibile dei cookie del profilo (`--user-data-dir`):
totale, domini, persistent/session/secure/expired e i top domini per numero di
cookie. Legge via `CookieManager` del profilo — niente navigazione, niente API.

| Switch | Default | Descrizione |
|---|---|---|
| `--autobrowse=session-info` | — | Attiva il comando. |
| `--ab-top=N` | `15` | Quanti top domini elencare. |

```bash
chrome.exe --headless --user-data-dir="E:/tmp/ab" --autobrowse=session-info --ab-top=10
```
```
cookies: 103 total (30 domains) - 67 persistent, 36 session, 79 secure, 1 expired
top domains:
     16  weather.com
     10  bing.com
```

### `warmup` — matura una sessione riusabile

Naviga davvero il motore per N minuti: digita query reali lettera-per-lettera,
apre il primo risultato di ognuna, fa pause naturali (randomizzate), opzional-
mente visita URL tuoi (`--ab-urls`), e i cookie si accumulano nel profilo. È
storia di visitatore-di-ritorno **autentica**, non fabbricata.

| Switch | Default | Descrizione |
|---|---|---|
| `--autobrowse=warmup` | — | Attiva il comando. |
| `--ab-engine=<e>` | `bing` | `google` \| `bing` \| `duckduckgo`. |
| `--ab-minutes=N` | `15` | Durata (accetta frazioni, es. `0.5`). |
| `--ab-query="q1,q2"` | set generico | Query da usare (separate da virgola). |
| `--ab-urls="u1,u2"` | — | URL target da visitare, interlacciati. |

```bash
chrome.exe --headless --user-data-dir="E:/tmp/ab" \
  --autobrowse=warmup --ab-engine=google --ab-minutes=15
```
Progresso su stderr:
```
[warmup] query: rust programming
[warmup] opening result https://rust-lang.org/
[warmup] done after 15.0 min, 42 queries
```

### `serve` / `mcp` / `octo-serve` — server

Vedi [§7](#7-server-http--websocket) e [§8](#8-server-mcp). `--autobrowse=mcp` e
`--autobrowse=octo-serve` sono **alias** di `serve`: lo stesso server espone sia
`POST /<comando>` e WebSocket, sia `POST /mcp`.

---

## 4. Fingerprint: headless identico alla GUI

Di default `--headless` espone segnali che i motori riconoscono come bot. In
modalità autobrowse questi vengono neutralizzati, così **headless è
indistinguibile dalla GUI**:

| Segnale | Headless normale | Headless + autobrowse |
|---|---|---|
| `navigator.userAgent` | `HeadlessChrome/…` | `Chrome/…` |
| `navigator.webdriver` | `false` | `false` |
| `sec-ch-ua` brands | Chrome | Chrome |

**Come.** In `--headless` il token `Headless` viene anteposto al prodotto dello
User-Agent in `components/embedder_support/user_agent_utils.cc`
(`GetUserAgentInternal()`: `product.insert(0, "Headless")`). La patch
**salta** quell'inserimento quando è attiva la modalità autobrowse (switch
`autobrowse-search` o `autobrowse` presente), lasciando il prodotto `Chrome`.
I client-hint (`sec-ch-ua`) in `--headless=new` derivano già dal nome prodotto
`Chrome`, non da `Headless`, quindi non serve altro; `navigator.webdriver` è già
`false` in headless semplice (diventa `true` solo con `--enable-automation`).

Verifica:
```bash
# atteso: Chrome/…  (NON HeadlessChrome/…)
chrome.exe --headless --autobrowse=fetch --ab-url="https://example.com" \
  --ab-eval="navigator.userAgent"
```

> Nota: la parità di fingerprint non elimina il muro captcha su un **profilo
> freddo** da IP datacenter — quello è reputazione IP. Si recupera con profilo
> caldo/`warmup`, `--fallback` a un altro motore, o proxy residenziale.

---

## 5. Stealth mode — come funziona lo spoofing

Aggiungendo **`--stealth`** a qualunque comando, uno script anti-fingerprint
viene iniettato a **document-start nel main world** di **ogni** nuovo documento.
Così gli script della pagina vedono un Chrome desktop normale, e l'automazione
non è rilevabile.

### Il meccanismo (perché è robusto)

Iniettare a document-start nel *main world* dal lato browser non è banale: gli
isolated world non bastano (una `defineProperty` su `navigator` in isolated
world non è visibile agli script della pagina, che vivono nel main world). La
soluzione usa la primitiva **supportata** di Chromium:

1. Si attacca una **sessione DevTools in-process** al tab:
   `content::DevToolsAgentHost::GetOrCreateFor(web_contents)` + `AttachClient`.
2. Si invia `Page.enable`, poi
   **`Page.addScriptToEvaluateOnNewDocument`** con il payload di spoofing: questo
   registra uno script che il renderer esegue nel **main world** *prima* degli
   script della pagina, a **ogni** nuovo documento.
3. **Race fix (importante):** la registrazione via DevTools è asincrona. Se si
   naviga subito, il primo documento può committare *prima* che lo script sia
   registrato. Perciò la navigazione parte **solo dopo l'ack** del comando
   (`id:2`) tramite una callback `on_ready` — così anche il **primo** documento
   è già patchato. (Un ritardo fisso non era sufficiente.)

Il client DevTools dichiara `IsTrusted() = true` per poter usare i comandi
privilegiati. Codice in `autobrowse_stealth.{h,cc}`; ogni runner naviga *dentro*
la callback di `InstallStealthIfEnabled(web_contents, on_ready)`.

### Cosa viene spoofato (il payload)

Il payload (main world, in un IIFE che non rompe mai la pagina) corregge i
segnali a più alto rischio, con valori internamente coerenti:

| Segnale | Prima (headless) | Dopo (stealth) | Come |
|---|---|---|---|
| `navigator.webdriver` | (già `false`) | **`undefined`** | `defineProperty` getter → `undefined` su `Navigator.prototype` (come Chrome reale) |
| `navigator.plugins` | vuoto | **5 plugin PDF realistici** | oggetti `Plugin` finti (PDF Viewer, Chrome PDF Viewer, …) via getter |
| `navigator.languages` | può mancare | **`['en-US','en']`** | getter su `Navigator.prototype` |
| `window.chrome` / `.runtime` | assente in headless | **presente** | stub `{}` se mancante |
| `Notification.permission` | incoerente | **`'default'`** | getter + patch di `permissions.query('notifications')` coerente |
| `navigator.hardwareConcurrency` | valore host | **`8`** | getter fisso realistico |
| `navigator.deviceMemory` | mancante | **`8`** | getter fisso realistico |
| WebGL `UNMASKED_VENDOR` (37445) | SwiftShader/ANGLE | **`Google Inc. (Intel)`** | override di `getParameter` su `WebGL(2)RenderingContext.prototype` |
| WebGL `UNMASKED_RENDERER` (37446) | SwiftShader | **`ANGLE (Intel … Direct3D11 …)`** | idem |

Verifica (lo script inline della pagina gira nel main world a parse-time, quindi
"vede" le patch):
```bash
# hc=8, deviceMemory=8, webdriver=undefined (omesso da JSON.stringify)
chrome.exe --headless --stealth --autobrowse=fetch \
  --ab-url="data:text/html,<body></body><script>document.body.textContent=JSON.stringify({webdriver:navigator.webdriver,plugins:navigator.plugins.length,hc:navigator.hardwareConcurrency,mem:navigator.deviceMemory})</script>" \
  --ab-dump=text
```

`--stealth` è **trasversale**: vale per search, fetch, scrape, monitor, warmup e
per tutte le richieste servite dal server.

---

## 6. Cookie consent: Reject all

Il driver di ricerca rileva i banner di consenso cookie — sia la pagina
`consent.google.com`, sia il dialog inline "before you continue" su una pagina
normale — e clicca **sempre "Reject all"** (la scelta privacy-preserving) prima
di proseguire con la ricerca. Una funzione `findReject()` cerca il controllo per
testo/`aria-label` (`reject all`, `rifiuta tutto`, `tout refuser`,
`alle ablehnen`, `decline`, …); sull'inline dialog è idempotente
(`window.__ab_consent`).

> I **CAPTCHA / "unusual traffic"** non vengono aggirati: quella casella *è* il
> meccanismo anti-bot. Il recupero avviene con profilo caldo/`warmup`,
> `--fallback` a un altro motore, o proxy residenziale.

---

## 7. Server HTTP + WebSocket

Un **unico** server (`net::HttpServer`, che gestisce HTTP **e** WebSocket
insieme) espone **tutti** i comandi sulla rete. Poiché i comandi pilotano
l'unico tab attivo, le richieste sono **serializzate** (una alla volta, le altre
in coda). Il server resta vivo (in GUI la finestra resta aperta).

| Switch | Default | Descrizione |
|---|---|---|
| `--autobrowse=serve` | — | Avvia il server. |
| `--ab-port=N` | `8080` | Porta HTTP/WS (bind su `127.0.0.1`). |

* `GET /health` → `ok`.
* `POST /<comando>` con body JSON → risultato JSON.
* **WebSocket**: una frame JSON di richiesta (`{"command":"<cmd>", ...}`) → una
  frame JSON di risposta. Stesso dispatch dell'HTTP.

Comandi esposti e campi del body JSON:

| Endpoint | Campi JSON |
|---|---|
| `/search` | `query` (obbl.), `engine`, `max_results` |
| `/fetch` | `url` (obbl.), `eval`, `dump`, `selector`, `wait`, `timeout` |
| `/scrape` | `urls` (obbl., separati da virgola), `eval`, `dump`, `wait`, `timeout` |
| `/monitor` | `url` (obbl.), `selector`, `on_change` — **un solo poll**, ritorna il valore |
| `/session-info` | `top` |
| `/warmup` | `engine`, `minutes`, `query`, `urls` |

```bash
chrome.exe --user-data-dir="E:/tmp/ab" --autobrowse=serve --ab-port=8080 about:blank &
curl -s localhost:8080/health
curl -s localhost:8080/fetch  -d '{"url":"https://example.com","eval":"document.title"}'
curl -s localhost:8080/scrape -d '{"urls":"https://a.com,https://b.com","eval":"document.title"}'
curl -s localhost:8080/search -d '{"query":"python asyncio","engine":"google","max_results":3}'
```

Internamente: ogni comando espone `SetCompletionCallback` che consegna il
risultato al server invece di stampare+uscire; **lo stesso layer di servizio**
alimenta CLI, HTTP, WS e MCP. Il runner appena finito viene distrutto su un task
successivo (non dall'interno della sua stessa callback, per evitare un
use-after-free).

---

## 8. Server MCP

Transport **Model Context Protocol** (JSON-RPC 2.0) sullo **stesso** server, su
`POST /mcp`. Espone i comandi autobrowse come **tool MCP**, così un client
(Claude Desktop, Cursor, …) può usarli. `--autobrowse=mcp` è un **alias** di
`serve`; `/mcp` è disponibile anche partendo da `--autobrowse=serve`.

Metodi supportati:
* `initialize` → `protocolVersion`, `capabilities.tools`, `serverInfo`.
* `tools/list` → i tool `search`, `fetch`, `scrape`, `monitor`, `session-info`,
  `warmup`, ciascuno con il proprio `inputSchema`.
* `tools/call` con `{name, arguments}` → esegue il comando e ritorna il risultato
  come content testuale MCP (`{"content":[{"type":"text","text":...}]}`).
* `notifications/*` → accettate senza risultato.

```bash
chrome.exe --user-data-dir="E:/tmp/ab" --autobrowse=mcp --ab-port=8080 about:blank &
curl -s localhost:8080/mcp -d '{"jsonrpc":"2.0","id":1,"method":"initialize"}'
curl -s localhost:8080/mcp -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
curl -s localhost:8080/mcp -d '{"jsonrpc":"2.0","id":3,"method":"tools/call",
  "params":{"name":"fetch","arguments":{"url":"https://example.com","eval":"document.title"}}}'
```

> Il **primo** request HTTP subito dopo l'avvio del server può tornare vuoto
> (race di avvio): basta ripeterlo; le richieste a server caldo sono affidabili.

---

## 9. Tabella completa degli switch

### Selettore comando

| Switch | Descrizione |
|---|---|
| `--autobrowse=<cmd>` | `search` \| `fetch` \| `scrape` \| `monitor` \| `session-info` \| `warmup` \| `serve` \| `mcp` \| `octo-serve` |
| `--autobrowse-search="Q"` | scorciatoia legacy: attiva search + query |
| `--autobrowse-engine=<e>` | legacy: engine di search |
| `--autobrowse-max-results=N` | legacy: max risultati |
| `--autobrowse-output=FILE` | legacy: file di output |

### Input condivisi `--ab-*`

| Switch | Usato da | Descrizione |
|---|---|---|
| `--ab-url=URL` | fetch, monitor | pagina target |
| `--ab-urls="u1,u2"` | scrape, warmup | lista URL (virgola/spazio) |
| `--ab-query="Q"` | search, warmup | query (warmup: lista) |
| `--ab-engine=<e>` | search, warmup | motore |
| `--ab-max-results=N` | search | ceiling risultati |
| `--ab-dump=<fmt>` | fetch, scrape | formato dump |
| `--ab-eval="EXPR"` | fetch, scrape | espressione JS |
| `--ab-selector=CSS` | monitor (e fetch) | selettore elemento |
| `--ab-on-change="JS"` | monitor | JS che produce il valore |
| `--ab-interval=SEC` | monitor | intervallo polling |
| `--ab-max-runs=N` | monitor | stop dopo N poll |
| `--ab-wait-until=<ev>` | fetch | `load`\|`domcontentloaded`\|`networkidle0` |
| `--ab-wait=SEC` | fetch, scrape | settle extra |
| `--ab-timeout=SEC` | fetch, scrape | timeout navigazione |
| `--ab-top=N` | session-info | top domini |
| `--ab-minutes=N` | warmup | durata |
| `--ab-port=N` | serve/mcp | porta HTTP/WS |
| `--ab-output=FILE` | vari | file di output |

### Flag globali nativi / env

| Flag/env | Descrizione |
|---|---|
| `--stealth` | anti-fingerprint completo (§5), trasversale |
| `--headless` / `--headless=new` | esegue senza finestra |
| `--user-data-dir=DIR` | profilo/sessione persistente |
| `--proxy-server=URL` | proxy HTTP/SOCKS5 |
| `--user-agent="UA"` | override User-Agent |
| `--remote-debugging-port=N` | server CDP (Puppeteer/Playwright) |
| `AB_DEBUG=1` (env) | log interni (driver, ack DevTools stealth) |

---

## 10. Architettura interna e file

### File nuovi (`chrome/browser/ui/autobrowse/`)

| File | Ruolo |
|---|---|
| `autobrowse_switches.h` | nomi di tutti gli switch (`--autobrowse*`, `--ab-*`) |
| `autobrowse_main_extra_parts.{h,cc}` | entrypoint: `ChromeBrowserMainExtraParts` che, in `PostBrowserStart()`, seleziona il sub-comando e istanzia il runner/server; in `PostEarlyInitialization()` silenzia i log di servizio |
| `autobrowse_search_runner.{h,cc}` | runner di `search` (typing umano + estrazione + consent) |
| `autobrowse_fetch_runner.{h,cc}` | runner di `fetch` |
| `autobrowse_scrape_runner.{h,cc}` | runner di `scrape` |
| `autobrowse_monitor_runner.{h,cc}` | runner di `monitor` |
| `autobrowse_session_info_runner.{h,cc}` | runner di `session-info` (CookieManager) |
| `autobrowse_warmup_runner.{h,cc}` | runner di `warmup` (macchina a fasi) |
| `autobrowse_stealth.{h,cc}` | stealth: DevTools in-process + `addScriptToEvaluateOnNewDocument` |
| `autobrowse_server.{h,cc}` | server unificato HTTP + WS + MCP |

### File Chromium modificati

| File | Modifica |
|---|---|
| `chrome/browser/chrome_browser_main.cc` | aggiunge `AutobrowseMainExtraParts` |
| `chrome/browser/ui/BUILD.gn` | aggiunge i sorgenti autobrowse al target `ui` |
| `components/embedder_support/user_agent_utils.cc` | niente token `Headless` nello UA in modalità autobrowse (§4) |

### Pattern di un runner (`content::WebContentsObserver`)

1. `Start()` — trova il tab attivo (ritenta finché non esiste), poi
   `InstallStealthIfEnabled(wc, on_ready)`; la navigazione parte **dentro**
   `on_ready` (subito se niente stealth, dopo l'ack se `--stealth`).
2. `DocumentOnLoadCompletedInPrimaryMainFrame()` — pagina caricata.
3. Esegue lo script driver in **isolated world**
   (`ExecuteJavaScriptInIsolatedWorld`, `ISOLATED_WORLD_ID_CHROME_INTERNAL`) così
   le pagine non lo vedono. `search` ripolla finché non arriva il JSON finale;
   `fetch`/`scrape` estraggono una volta dopo il settle.
4. `Finish()` — consegna l'output: in **server mode** via
   `SetCompletionCallback`; altrimenti stampa/scrive e, in **headless**, chiama
   `chrome::AttemptExit()` (in **GUI** lascia la finestra aperta).
5. `OnTimeout()` — deadline di sicurezza con output di errore.

Perché **isolated world**: lo script del driver gira in un mondo JS separato,
invisibile alla pagina, quindi l'automazione non è rilevabile via override di
`document`/DOM. Lo **stealth** invece deve stare nel **main world** (vede la
pagina): per questo usa il canale DevTools `addScriptToEvaluateOnNewDocument`
(§5), non l'isolated world.
