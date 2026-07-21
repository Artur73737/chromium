# Autobrowse — Obscura command surface, native in Chromium

Autobrowse porta i comandi di **Obscura** dentro Chromium, **in C++ nativo**.
La ricerca e la navigazione sono **umane**: la query viene digitata nella barra
di ricerca lettera-per-lettera, si preme Invio, e i link dei risultati vengono
letti dal DOM. **Nessuna API** di Google/Bing/DuckDuckGo viene mai usata (le API
vengono bloccate e tradiscono il bot). Ogni comando funziona **sia con GUI
visibile sia in headless**, con lo **stesso identico fingerprint**.

Codice: `chrome/browser/ui/autobrowse/`.

---

## 1. Come si compila e si lancia

```bash
# build (dalla root del workspace)
build.bat            # oppure: env.bat && autoninja -C src/out/Default chrome -j 6

# eseguibile
src/out/Default/chrome.exe
```

Nota shell (git-bash su Windows): per invocare `cmd.exe` con argomenti che
iniziano con `/`, anteporre `MSYS_NO_PATHCONV=1` (altrimenti `/c` viene
convertito in un path Windows).

### Struttura comando

Due forme equivalenti:

* **Sub-comando unico** (stile Obscura, preferito):
  `--autobrowse=<cmd>` + input via switch `--ab-*`.
* **Scorciatoia legacy** per la ricerca: `--autobrowse-search="query"`.

L'ultimo argomento è sempre una pagina di partenza qualsiasi, es. `about:blank`
(serve solo a far aprire un tab su cui agganciarsi).

### GUI vs headless

* **GUI visibile**: ometti `--headless`. Al termine la **finestra resta aperta**
  sulla pagina/risultati.
* **Headless**: aggiungi `--headless` (o `--headless=new`). Al termine il
  processo **esce** dopo aver stampato l'output.

Il comportamento (typing umano, estrazione, ecc.) è **identico** nelle due modalità.

### Sessione / profilo

Il profilo (`--user-data-dir=DIR`) è la **sessione**: i cookie persistono tra i
run, esattamente come `--session` in Obscura. Un profilo "caldo" (con cookie
dell'engine già presenti) alza molto l'affidabilità anti-bot. Un profilo freddo
su Google può incontrare captcha/consent-wall per reputazione IP (datacenter).

---

## 2. Fingerprint: headless identico alla GUI

Di default `--headless` espone segnali che i motori riconoscono come bot. In
modalità autobrowse questi vengono neutralizzati, così **headless è
indistinguibile dalla GUI**:

| Segnale | Headless normale | Headless + autobrowse |
|---|---|---|
| `navigator.userAgent` | `HeadlessChrome/…` | `Chrome/…` |
| `navigator.webdriver` | `false` | `false` |
| `sec-ch-ua` brands | Chrome | Chrome |

Il fix vive in `components/embedder_support/user_agent_utils.cc`: il token
`Headless` non viene aggiunto allo User-Agent quando è attiva la modalità
autobrowse. Verifica:

```bash
# atteso: Chrome/… (NON HeadlessChrome/…)
chrome.exe --headless --autobrowse=fetch --ab-url="https://example.com" \
  --ab-eval="navigator.userAgent"
```

### `--stealth` — anti-fingerprint completo (IMPLEMENTATO)

Aggiungendo `--stealth` a qualunque comando, un file di patch viene iniettato a
**document-start nel main world** di ogni nuovo documento (via una sessione
DevTools in-process e `Page.addScriptToEvaluateOnNewDocument`), così gli script
della pagina vedono un Chrome desktop normale. La navigazione parte solo dopo
che lo script è registrato, quindi anche il **primo** documento è coperto.

Segnali corretti: `navigator.webdriver` → `undefined`; `navigator.plugins`
(lista PDF realistica invece di vuota); `navigator.languages`; `window.chrome`
runtime; `Notification.permission` / `permissions.query` coerenti;
`hardwareConcurrency`/`deviceMemory` = 8; WebGL vendor/renderer (Intel invece di
SwiftShader). Implementazione in `autobrowse_stealth.{h,cc}`.

```bash
# hc=8, deviceMemory=8, webdriver=undefined
chrome.exe --headless --stealth --autobrowse=fetch \
  --ab-url="data:text/html,<body></body><script>document.body.textContent=navigator.hardwareConcurrency</script>" \
  --ab-dump=text
```

---

## 3. Comandi

### `search` — ricerca umana (IMPLEMENTATO)

Naviga la home del motore, digita la query lettera-per-lettera con eventi
`InputEvent`/`KeyboardEvent`, preme Invio, gestisce consent-wall e captcha, ed
estrae i primi risultati (titolo, url, snippet). Output JSON Obscura-style.

| Switch | Default | Descrizione |
|---|---|---|
| `--autobrowse=search` / `--autobrowse-search="Q"` | — | Attiva la ricerca; la 2ª forma passa anche la query. |
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

### `fetch` — carica una pagina, dump o eval (IMPLEMENTATO)

Naviga a un URL, attende il caricamento (+ settle opzionale), poi **valuta una
espressione JS** (`--ab-eval`) oppure **dumpa** la pagina renderizzata.

| Switch | Default | Descrizione |
|---|---|---|
| `--autobrowse=fetch` | — | Attiva il comando. |
| `--ab-url=URL` | — (**obbligatorio**) | Pagina da caricare. |
| `--ab-eval="EXPR"` | — | Espressione JS; ne stampa il valore. Ha priorità su `--ab-dump`. |
| `--ab-dump=<fmt>` | `html` | `html` \| `text` \| `links` \| `markdown`. |
| `--ab-wait=SEC` | `0` | Attesa extra dopo il load, per contenuti async. |
| `--ab-timeout=SEC` | `30` | Timeout di navigazione. |
| `--ab-output=FILE` | stdout | Scrive l'output su file. |

```bash
chrome.exe --headless --autobrowse=fetch --ab-url="https://example.com" --ab-eval="document.title"
chrome.exe --headless --autobrowse=fetch --ab-url="https://example.com" --ab-dump=text
chrome.exe --headless --autobrowse=fetch --ab-url="https://example.com" --ab-dump=links
chrome.exe --headless --autobrowse=fetch --ab-url="https://example.com" --ab-dump=markdown
```

`--ab-dump`:
* `html` — outerHTML del documento renderizzato.
* `text` — testo visibile (`body.innerText`).
* `links` — un `{text, href}` JSON per riga.
* `markdown` — DOM→Markdown (heading, liste, link, code).

### `scrape` — molti URL, un eval/dump ciascuno (IMPLEMENTATO)

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

### `monitor` — watch pagina, NDJSON sui cambiamenti (IMPLEMENTATO)

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

### `warmup` — matura una sessione riusabile (IMPLEMENTATO)

Naviga davvero il motore per N minuti: digita query reali lettera-per-lettera,
apre il primo risultato di ognuna, fa pause naturali, opzionalmente visita URL
tuoi (`--ab-urls`), e i cookie si accumulano nel profilo (`--user-data-dir`).
È storia di visitatore-di-ritorno autentica, non fabbricata.

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

### `session-info` — riepilogo del cookie jar (IMPLEMENTATO)

Stampa un riepilogo leggibile dei cookie del profilo (`--user-data-dir`): totale,
domini, persistent/session/secure/expired e i top domini per numero di cookie.
Legge via `CookieManager` del profilo (niente navigazione, niente API).

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

### `serve` — server HTTP + WebSocket unificato (IN CORSO)

Un **unico** server (`net::HttpServer`, che gestisce HTTP e WS insieme) espone i
comandi autobrowse sulla rete. Poiché i comandi pilotano l'unico tab attivo, le
richieste sono **serializzate** (una alla volta, le altre in coda).

| Switch | Default | Descrizione |
|---|---|---|
| `--autobrowse=serve` | — | Avvia il server (resta vivo). |
| `--ab-port=N` | `8080` | Porta HTTP/WS (bind su 127.0.0.1). |

- `GET /health` → `ok`.
- `POST /<comando>` con body JSON → risultato JSON. WebSocket: una frame JSON di
  richiesta (`{"command":"search",...}`) → una frame JSON di risposta.

```bash
chrome.exe --user-data-dir="E:/tmp/ab" --autobrowse=serve --ab-port=8080 about:blank &
curl -s localhost:8080/search -d '{"query":"python asyncio","engine":"google","max_results":3}'
```

**Stato:** infrastruttura HTTP+WS pronta e `search` collegato e testato. I
restanti comandi (fetch/scrape/monitor/session-info/warmup) si agganciano allo
stesso dispatch aggiungendo la callback di completamento ai rispettivi runner
(come già fatto per search).

### Già nativi in Chromium (nessun codice nuovo)

Alcuni comandi/flag globali di Obscura esistono già in Chrome:

| Obscura | Chromium nativo |
|---|---|
| `serve` (server CDP) | `--remote-debugging-port=9222` (Puppeteer/Playwright si collegano già) |
| `--proxy socks5://…` | `--proxy-server="socks5://…"` |
| `--user-agent "…"` | `--user-agent="…"` |
| `--session DIR` | `--user-data-dir=DIR` |

### Roadmap (da implementare, stesso pattern del fetch runner)

| Obscura | Piano nativo | Stato |
|---|---|---|
| `octo-serve` (search via HTTP/WS) | **un unico** server HTTP+WS che espone **tutti** i comandi (search/fetch/scrape/monitor/session-info/warmup), non solo search; richiede il refactor dei core in servizi riusabili | ⏳ |
| `mcp` | server MCP che espone i tool browser | ⏳ |

> **Nota architettura.** Tre superfici distinte: **CLI** (`--autobrowse=<cmd>`, fatta), **CDP** (`--remote-debugging-port`, già nativa, separata), e **HTTP+WS** (un solo server per tutti i comandi, da fare). `--stealth` è trasversale a tutte.

---

## 4. Architettura interna

* `autobrowse_switches.h` — nomi degli switch (`--autobrowse*`, `--ab-*`).
* `autobrowse_main_extra_parts.{h,cc}` — `ChromeBrowserMainExtraParts`
  agganciato in `chrome_browser_main.cc`. In `PostBrowserStart()` seleziona il
  sub-comando e istanzia il runner giusto; in `PostEarlyInitialization()`
  silenzia i log di servizio così lo stdout resta pulito.
* `autobrowse_search_runner.{h,cc}` — runner di `search`.
* `autobrowse_fetch_runner.{h,cc}` — runner di `fetch`.

**Pattern di un runner** (`WebContentsObserver`):
1. `Start()` — trova il tab attivo (ritenta finché non esiste), naviga.
2. `DocumentOnLoadCompletedInPrimaryMainFrame()` — pagina caricata.
3. Esegue lo script driver in **isolated world** (`ExecuteJavaScriptInIsolatedWorld`)
   così le pagine non lo vedono. Search ripolla finché non arriva il JSON finale;
   fetch estrae una volta dopo il settle.
4. `Finish()` — stampa/scrive l'output; in **headless** chiama
   `chrome::AttemptExit()`, in **GUI** lascia la finestra aperta.
5. `OnTimeout()` — deadline di sicurezza con output di errore.

Perché **isolated world**: lo script gira in un mondo JS separato, invisibile
alla pagina, quindi l'automazione non è rilevabile via override di `document`/DOM.
