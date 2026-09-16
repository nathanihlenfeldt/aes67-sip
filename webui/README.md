# aes67-sip web UI

Operator console for the **aes67-sip** gateway: a single-page Vite + React 18 app
(plain JavaScript) that talks to the gateway REST API documented in
[`../docs/api.md`](../docs/api.md).

## Stack

* Vite 5 + `@vitejs/plugin-react`
* React 18 (`react`, `react-dom`), `react-router-dom` v6
* No CSS framework, no Redux, no axios — hand written CSS in `src/styles.css`,
  `fetch` via the small wrapper in `src/api.js`

## Scripts

```sh
npm install       # install dependencies
npm start         # dev server on http://localhost:5173 (proxies /api)
npm run build     # production bundle into dist/
npm run serve     # preview the production build locally
npm test          # logic tests (node's built-in runner, no extra dependency)
```

### Dev proxy

`vite.config.js` proxies everything under `/api` to a gateway running on
`http://127.0.0.1:8081`, so `npm start` works against a locally running
`aes67-sip` (or a port-forwarded rack unit):

```js
server: {
  proxy: {
    '/api': { target: 'http://127.0.0.1:8081', changeOrigin: true },
  },
}
```

Start the gateway with the default HTTP port (`-p 8081`) or change the proxy
target to match `-p`.

## Production deployment

```
npm run build
```

produces `webui/dist/`. Point the gateway's `webui_dir` setting at that
directory; the gateway then serves the console from `/` and the app talks to
its own origin, so no proxy is used in production. Asset paths are relative
(`base: './'`), so the bundle also works when mounted under a sub-path.

## Architecture notes

* **`src/api.js`** — one function per REST endpoint (`getVersion`, `getStatus`,
  `setLineConfig`, `lineCall`, `browseRemoteSources`, `selfTest`, …). Every
  function checks `res.ok`, throws an `Error` carrying the `text/plain` error
  body and returns the parsed JSON body.
* **`src/hooks/useStatus.js`** — polls `GET /api/status` every 500 ms and is
  paused while `document.visibilityState === 'hidden'` (it fetches immediately
  on becoming visible again). Errors are returned as a message and the last good
  status is kept, so tables never blank out.
* **`src/StatusContext.jsx`** — single shared poll for all pages.
* **`src/commissioning.js`** — the party-lines page's logic: configuration document ⇄
  drafts ⇄ the patch posted to `POST /api/config`, plus the locating of the appliance's
  refusals against the entry that caused them. Kept out of the component so
  `test/commissioning.test.js` can exercise it under `npm test`; it imports `./format.js`
  with the extension so node can load it directly.
* **`src/pages/`** — Dashboard, Lines, Party lines, AES67, Diagnostics, Settings.
* **`src/components/`** — `StatusPill`, `LevelMeter` (dBFS −60…0 with peak
  hold, `null` levels render as −inf), `Card`, `ConfirmButton`, `Modal`.

### Data assumptions

`levels_dbfs` values may be `null` (digital silence / −inf); they are rendered
as `-inf`. Durations are shown as `mm:ss` (or `hh:mm:ss`) and dBFS/dB values to
one decimal place.

Status sub-objects are treated as optional everywhere (`status.call`,
`status.error`, `status.ptp`, `status.aes67`, `status.levels`): a missing field
renders as `n/a` instead of throwing.
