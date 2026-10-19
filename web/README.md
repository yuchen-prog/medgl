# MEDGL Web Workstation

React 19 / TypeScript / Vite front end for the independently hosted CUDA volume-rendering service. The application renders only server images; no client-side volume renderer or sample medical photograph is included.

## Run and Verify

```powershell
cd web
npm install
npm run lint
npm run typecheck
npm test
npm run build
```

The service serves `web/dist` at the same origin in production. The owner can start `npm run dev` for development; Vite proxies HTTP and WebSocket `/api` requests to `http://localhost:8080`. No persistent server is started by this implementation.

Browser tests use a temporary Vite process on port 5178, shut down automatically after the run. They mock the API and stream a plainly labeled test raster, not a medical image. Screenshots are written to `web/test-results`.

```powershell
$env:PLAYWRIGHT_BROWSERS_PATH = "$PWD/node_modules/.cache/ms-playwright"
npx playwright install chromium
npm run test:ui
```

## API Assumptions

- `GET /api/v1/series` returns `{series:[{id,description,fileCount,supported,reason}]}`. Only a supported entry with the exact ID `phantom` is auto-loaded. All other datasets require explicit selection and Load series.
- `POST /api/v1/sessions` accepts `{seriesId}` and returns HTTP 202 with `{sessionId,state}`. The front end polls GET at 350 ms intervals, with a 60 second loading deadline.
- A ready session supplies `sceneRevision`, `volume:{dimensions:[w,h,d],spacing:[x,y,z]}`, and `initial:{camera,style}`. `camera` contains `targetMm`, `rotationXyzw`, `orthoHeightMm`; `style` contains `presetId`, `windowWidth`, `windowLevel`, `opacityScale`, `backgroundRgb`. Extra initial fields are ignored.
- Camera quaternion convention: XYZW, camera-local axes to patient coordinates, local +X right / +Y up. Confirm this convention with the renderer owner. Right-drag panning uses orthographic millimeters per CSS pixel. Orthographic height is clamped to 5..5000 mm.
- Background RGB is normalized 0..1. The guaranteed default preset is `ct-bone`; selectable IDs `ct-soft-tissue` and `ct-lung` need confirmation from the owner. Their window defaults are defined in `src/App.tsx`. No speculative lighting or sampling fields are sent.
- The WebSocket path is `/api/v1/sessions/{id}/stream`, using `ws` / `wss` at the page origin. Requests are full absolute render snapshots with a globally increasing safe-integer `seq`, revision, `preview` / `final`, integer dimensions, camera and style. Resize accounts for device pixel ratio (max 2), selectable resolution, and a 2048-pixel longest edge.
- Frame packets are a 4-byte **little-endian** JSON byte count, UTF-8 metadata, then image bytes. Protocol version is **1**. Metadata requires matching session, revision, dimensions, safe positive sequence, `encoding:jpeg|png`, exact `payloadBytes`, and quality. ImageBitmap dimensions must match metadata. Sequences are checked both before and after decoding; every decoded bitmap is closed.
- Updates are coalesced to approximately 30 Hz; an idle or pointer-up final render is debounced 200 ms. Canvas backing dimensions change only on viewport/resolution resize, never on frame arrival. Render round trip measures request-to-decoded-presentation time, not GPU time or FPS.
- `GET /api/v1/sessions/{id}/frame?seq=...&revision=...` returns the cached latest final as `image/png`. Export is enabled only for the current final frame. HTTP errors and an empty/non-PNG response are surfaced.
- `DELETE /api/v1/sessions/{id}` returns 202. Close confirmation is polled for up to 15 seconds after DELETE; `closed`, HTTP 404, or 410 confirm closure. A timeout retains ownership and blocks a new POST until closure is subsequently confirmed. WS and scheduled work stop on unmount; DELETE continues for a POST that resolves late. Page exit additionally uses best-effort keepalive DELETE; a service-side session TTL is still recommended.
- Automatic stream recovery is limited to three attempts (0.5 / 1 / 2 seconds); manual reconnect is available. Connect timeout is 8 seconds and absent-frame timeout is 12 seconds. Loading and close polling are also bounded.

## Ownership

All frontend source, configuration, dependencies, tests and build output live under `web`. The C++ service and root build/documentation are untouched. IBM Plex is loaded optionally from Google Fonts; offline fallbacks use Segoe UI / Consolas.

The next integration step is to point this build at the actual service and confirm preset IDs, quaternion convention, normalized RGB and protocol version with the backend owner.