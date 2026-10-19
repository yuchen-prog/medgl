import { expect, test } from '@playwright/test';
import type { APIRequestContext, Page, Response, TestInfo } from '@playwright/test';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import WebSocket from 'ws';
import type { RawData } from 'ws';
import { parseCamera, parseCatalog, parseSession, record } from '../../src/protocol';
import type { FrameMeta, RenderRequest, Session } from '../../src/protocol';

test.describe.configure({ timeout: 90_000 });

const sessionsPath = '/api/v1/sessions';
const sequenceSelector = '.viewport-bottom-right .mono';
const pngSignature = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
const summary = {
  browser: { status: 'not-run', cycles: 0, sessions: 0, frames: 0, displayedTransitions: 0,
    lastSeq: 0, sliderChanges: 0, reconnects: 0, pageErrors: 0 },
  native: { status: 'not-run', sentRequests: 0, receivedFrames: 0, invalidRenders: 0,
    reconnects: 0, lastSeq: 0, closedSessions: 0 }
};

function isolatedOrigin(baseURL: string | undefined): string {
  if (!baseURL) throw new Error('Use playwright.real.config.ts for native stability tests');
  const url = new URL(baseURL);
  if (url.protocol !== 'http:' || !['127.0.0.1', 'localhost'].includes(url.hostname) ||
      url.port !== '8082' || url.username || url.password || url.pathname !== '/' || url.search || url.hash) {
    throw new Error('Stability tests require the isolated loopback backend on port 8082');
  }
  return url.origin;
}

function sessionPath(sessionId: string): string {
  return `${sessionsPath}/${encodeURIComponent(sessionId)}`;
}

async function saveSummary(info: TestInfo): Promise<void> {
  await mkdir(info.project.outputDir, { recursive: true });
  await writeFile(join(info.project.outputDir, 'stability-summary.json'),
    `${JSON.stringify(summary, null, 2)}\n`, 'utf8');
}

async function sessionState(request: APIRequestContext, sessionId: string): Promise<string> {
  const response = await request.get(sessionPath(sessionId), { timeout: 3000 });
  try {
    if (response.status() === 404) return 'absent';
    expect(response.status()).toBe(200);
    const session = parseSession(await response.json() as unknown);
    expect(session.sessionId === sessionId, 'GET must preserve session identity').toBe(true);
    return session.state;
  } finally {
    await response.dispose();
  }
}

async function closed(request: APIRequestContext, sessionId: string): Promise<void> {
  await expect.poll(async () => ['closed', 'absent'].includes(await sessionState(request, sessionId)),
    { timeout: 30_000, intervals: [50, 100, 250, 500], message: 'Old session must close within 30 seconds' }).toBe(true);
}

async function release(request: APIRequestContext, sessionId: string): Promise<void> {
  if (['closed', 'absent'].includes(await sessionState(request, sessionId))) return;
  const response = await request.delete(sessionPath(sessionId), { timeout: 5000 });
  try { expect([202, 404].includes(response.status())).toBe(true); }
  finally { await response.dispose(); }
  await closed(request, sessionId);
}

function decodePacket(bytes: Buffer): { meta: FrameMeta; image: Buffer } {
  if (bytes.length < 5) throw new Error('Truncated native frame');
  const length = bytes.readUInt32LE(0);
  if (length < 1 || length > 64 * 1024 || length > bytes.length - 4) {
    throw new Error('Invalid native frame header length');
  }
  const value = record(JSON.parse(new TextDecoder('utf-8', { fatal: true })
    .decode(bytes.subarray(4, 4 + length))) as unknown);
  const image = bytes.subarray(4 + length);
  const integer = (item: unknown, maximum = Number.MAX_SAFE_INTEGER) =>
    typeof item === 'number' && Number.isSafeInteger(item) && item > 0 && item <= maximum;
  if (value.protocolVersion !== 1 || typeof value.sessionId !== 'string' || !value.sessionId ||
      !integer(value.seq) || !integer(value.sceneRevision) || !integer(value.width, 8192) ||
      !integer(value.height, 8192) || !integer(value.payloadBytes, 64 * 1024 * 1024) ||
      value.payloadBytes !== image.length || !['preview', 'final'].includes(String(value.quality)) ||
      !['jpeg', 'png'].includes(String(value.encoding))) {
    throw new Error('Invalid native frame metadata');
  }
  if (value.camera !== undefined) parseCamera(value.camera);
  if (value.quality === 'final') {
    if (value.encoding !== 'png' || image.length < 33 || !image.subarray(0, 8).equals(pngSignature) ||
        image.toString('ascii', 12, 16) !== 'IHDR' || image.readUInt32BE(16) !== value.width ||
        image.readUInt32BE(20) !== value.height) throw new Error('Invalid final PNG dimensions');
  } else if (value.encoding !== 'jpeg' || image[0] !== 255 || image[1] !== 216) {
    throw new Error('Invalid preview JPEG');
  }
  return { meta: value as unknown as FrameMeta, image };
}

async function ready(page: Page): Promise<number> {
  await expect(page.getByRole('button', { name: 'Download final PNG', exact: true })).toBeEnabled({ timeout: 30_000 });
  await expect(page.locator('.render-indicator')).toHaveCount(0);
  await expect(page.locator('.viewport-bottom-right .viewport-label')).toHaveText('FINAL');
  await expect(page.locator('.header-status')).toContainText('Connected');
  await expect(page.getByRole('alert')).toHaveCount(0);
  const label = await page.locator(sequenceSelector).innerText();
  if (!/^#[1-9]\d*$/.test(label)) throw new Error('Missing displayed native frame sequence');
  return Number(label.slice(1));
}

test('native browser stability: five lifecycle cycles, rapid sliders and offline recovery',
  async ({ page, context, request, baseURL }, info) => {
    const origin = isolatedOrigin(baseURL);
    summary.browser.status = 'failed';
    const ids = new Set<string>();
    const frames = new Map<string, FrameMeta>();
    const pending = new Set<Promise<void>>();
    let currentId: string | undefined;
    let listenerFailure = false;
    let frameCount = 0;
    let pageErrors = 0;
    let collectFrames = true;
    let expectedNetworkErrors = false;
    let completed = false;
    const onResponse = (response: Response) => {
      if (response.request().method() !== 'POST' || new URL(response.url()).pathname !== sessionsPath ||
          response.status() !== 202) return;
      if (pending.size >= 16) { listenerFailure = true; return; }
      const capture = (async () => {
        try {
          const session = parseSession(await response.json() as unknown);
          if (ids.size >= 16 || ids.has(session.sessionId)) { listenerFailure = true; return; }
          ids.add(session.sessionId);
          currentId = session.sessionId;
        } catch { listenerFailure = true; }
      })();
      pending.add(capture);
      void capture.finally(() => pending.delete(capture));
    };
    const onPageError = (error: Error) => {
      if (!expectedNetworkErrors || !/net::ERR_|Failed to fetch|NetworkError|Network request failed/i.test(error.message)) pageErrors++;
    };
    page.on('response', onResponse);
    page.on('pageerror', onPageError);
    page.on('websocket', socket => {
      if (!new URL(socket.url()).pathname.endsWith('/stream')) return;
      socket.on('framereceived', event => {
        if (!collectFrames) return;
        try {
          if (typeof event.payload === 'string' || ++frameCount > 256) {
            throw new Error('Unexpected or excessive native browser frames');
          }
          const { meta } = decodePacket(event.payload);
          const previous = frames.get(meta.sessionId);
          if (meta.sceneRevision !== 1 || (previous && meta.seq <= previous.seq) ||
              (!previous && frames.size >= 16)) throw new Error('Native browser frame regression');
          frames.set(meta.sessionId, meta);
        } catch { listenerFailure = true; }
      });
    });
    const captured = async (expected: number): Promise<string> => {
      await expect.poll(() => ids.size, { timeout: 5000 }).toBe(expected);
      if (listenerFailure || !currentId) throw new Error('Failed to capture bounded session responses');
      return currentId;
    };
    const presented = async (sessionId: string, previousSeq = 0): Promise<number> => {
      const seq = await ready(page);
      expect(seq).toBeGreaterThan(previousSeq);
      await expect.poll(() => {
        const frame = frames.get(sessionId);
        return frame?.seq === seq && frame.quality === 'final';
      }, { timeout: 5000, message: 'Displayed frame must belong to the current native session' }).toBe(true);
      const dimensions = await page.locator('canvas').evaluate((canvas: HTMLCanvasElement) =>
        ({ width: canvas.width, height: canvas.height }));
      expect(frames.get(sessionId)?.width).toBe(dimensions.width);
      expect(frames.get(sessionId)?.height).toBe(dimensions.height);
      expect(listenerFailure, 'Native frame and session listeners must remain valid').toBe(false);
      summary.browser.lastSeq = seq;
      return seq;
    };
    try {
      const catalogResponse = await request.get('/api/v1/series', { timeout: 5000 });
      try {
        expect(catalogResponse.status()).toBe(200);
        const catalog = parseCatalog(await catalogResponse.json() as unknown);
        for (const id of ['phantom', 'series-1']) expect(catalog.some(item => item.id === id && item.supported)).toBe(true);
      } finally { await catalogResponse.dispose(); }
      await page.setViewportSize({ width: 1100, height: 760 });
      await page.goto('/');
      expect(new URL(page.url()).origin === origin, 'Browser requests must remain same-origin').toBe(true);
      const unloaded = await captured(1);
      await presented(unloaded);
      await page.goto('about:blank');
      await closed(request, unloaded);
      await page.goto('/');
      let active = await captured(2);
      let lastSeq = await presented(active);
      await page.evaluate(selector => {
        const target = document.querySelector(selector);
        if (!target) throw new Error('Missing sequence indicator');
        const state = { last: 0, transitions: 0, regressions: 0 };
        const observe = () => {
          const label = target.textContent ?? '';
          if (!/^#[1-9]\d*$/.test(label)) return;
          const seq = Number(label.slice(1));
          if (seq === state.last) return;
          if (seq <= state.last) state.regressions++;
          state.last = seq;
          state.transitions++;
        };
        observe();
        new MutationObserver(observe).observe(target, { childList: true, characterData: true, subtree: true });
        Object.defineProperty(globalThis, '__medglStability', { value: state, configurable: true });
      }, sequenceSelector);
      for (let cycle = 0; cycle < 5; cycle++) {
        for (const seriesId of ['series-1', 'phantom']) {
          const previous = active;
          const expectedSessions = ids.size + 1;
          await page.locator('#series').selectOption(seriesId);
          await page.getByRole('button', { name: 'Load series', exact: true }).click();
          active = await captured(expectedSessions);
          expect(active !== previous, 'Every load must allocate a distinct session').toBe(true);
          if (seriesId === 'series-1') {
            await page.getByRole('tab', { name: 'Info', exact: true }).click();
            await expect(page.locator('.geometry')).toContainText(/64\s*\u00d7\s*64\s*\u00d7\s*64/, { timeout: 30_000 });
            await page.getByRole('tab', { name: 'Display', exact: true }).click();
          }
          lastSeq = await presented(active, lastSeq);
          await closed(request, previous);
        }
        summary.browser.cycles++;
      }
      expect(ids.size).toBe(12);
      const slider = page.getByRole('slider', { name: 'Window level slider', exact: true });
      const initialLevel = Number(await slider.inputValue());
      await slider.focus();
      for (let interaction = 0; interaction < 25; interaction++) {
        await slider.press('ArrowRight');
        summary.browser.sliderChanges++;
      }
      await expect(slider).toHaveValue(String(initialLevel + 25));
      await expect.poll(async () => Number((await page.locator(sequenceSelector).innerText()).slice(1)),
        { timeout: 15_000 }).toBeGreaterThan(lastSeq);
      lastSeq = await presented(active, lastSeq);
      const [download] = await Promise.all([
        page.waitForEvent('download', { timeout: 10_000 }),
        page.getByRole('button', { name: 'Download final PNG', exact: true }).click()
      ]);
      expect(await download.failure()).toBeNull();
      const downloadPath = await download.path();
      if (!downloadPath) throw new Error('Missing native PNG export');
      const image = await readFile(downloadPath);
      expect(image.subarray(0, 8).equals(pngSignature)).toBe(true);
      const dimensions = await page.locator('canvas').evaluate((canvas: HTMLCanvasElement) =>
        ({ width: canvas.width, height: canvas.height }));
      expect(image.readUInt32BE(16)).toBe(dimensions.width);
      expect(image.readUInt32BE(20)).toBe(dimensions.height);
      const cdp = await context.newCDPSession(page);
      try {
        expectedNetworkErrors = true;
        await context.setOffline(true);
        const prototype = await cdp.send('Runtime.evaluate', { expression: 'WebSocket.prototype' });
        const prototypeId = prototype.result.objectId;
        if (!prototypeId) throw new Error('Missing native WebSocket prototype');
        const sockets = await cdp.send('Runtime.queryObjects', { prototypeObjectId: prototypeId });
        const socketsId = sockets.objects.objectId;
        if (!socketsId) throw new Error('Missing native WebSocket instances');
        try {
          const result = await cdp.send('Runtime.callFunctionOn', {
            objectId: socketsId,
            functionDeclaration: `function () {
              const live = this.filter(socket => socket.readyState === WebSocket.OPEN &&
                new URL(socket.url).pathname.endsWith('/stream'));
              for (const socket of live) socket.close(1000, 'Stability disconnect');
              return live.length;
            }`, returnByValue: true
          });
          expect([0, 1].includes(result.result.value as number),
            'Offline may already close the sole UI socket; additional connections are forbidden').toBe(true);
        } finally {
          await cdp.send('Runtime.releaseObject', { objectId: socketsId });
          await cdp.send('Runtime.releaseObject', { objectId: prototypeId });
        }
        await expect(page.locator('.header-status')).toContainText(/Reconnecting|Idle/, { timeout: 1500 });
      } finally {
        await context.setOffline(false);
        await cdp.detach();
      }
      await expect.poll(async () => (await page.locator('.header-status').innerText()).includes('Connected') ||
        await page.getByRole('button', { name: 'Reconnect stream', exact: true }).isVisible(),
      { timeout: 5000 }).toBe(true);
      const reconnect = page.getByRole('button', { name: 'Reconnect stream', exact: true });
      if (await reconnect.isVisible()) await reconnect.click();
      await expect.poll(async () => Number((await page.locator(sequenceSelector).innerText()).slice(1)),
        { timeout: 15_000 }).toBeGreaterThan(lastSeq);
      await presented(active, lastSeq);
      expectedNetworkErrors = false;
      expect(currentId === active, 'Reconnect must retain the current session').toBe(true);
      expect(ids.size).toBe(12);
      summary.browser.reconnects++;
      const display = await page.evaluate(() => (globalThis as unknown as {
        __medglStability: { last: number; transitions: number; regressions: number }
      }).__medglStability);
      expect(display.regressions, 'Displayed sequences must never regress, including reconnect').toBe(0);
      expect(display.transitions).toBeGreaterThanOrEqual(12);
      summary.browser.displayedTransitions = display.transitions;
      for (const sessionId of ids) if (sessionId !== active) await closed(request, sessionId);
      await page.goto('about:blank');
      await closed(request, active);
      expect(listenerFailure, 'Bounded native listeners must remain valid through unload').toBe(false);
      expect(pageErrors, 'Unexpected browser exceptions (excluding expected offline network errors)').toBe(0);
      completed = true;
    } finally {
      let cleanupSucceeded = false;
      try {
        try {
          await context.setOffline(false);
          collectFrames = false;
          await page.goto('about:blank', { timeout: 5000 });
        } finally {
          await Promise.all(pending);
          page.off('response', onResponse);
          page.off('pageerror', onPageError);
          await Promise.all([...ids].map(sessionId => release(request, sessionId)));
        }
        cleanupSucceeded = true;
      } finally {
        summary.browser.sessions = ids.size;
        summary.browser.frames = frameCount;
        summary.browser.pageErrors = pageErrors;
        summary.browser.status = completed && cleanupSucceeded ? 'passed' : 'failed';
        await saveSummary(info);
      }
    }
  });

type NativePacket = ReturnType<typeof decodePacket>;
type NativeCounters = typeof summary.native;

class NativeStream {
  private readonly socket: WebSocket;
  private packet: NativePacket | undefined;
  private failure: Error | undefined;
  private opened = false;
  private ended = false;
  private errorCode: string | undefined;
  private wake: (() => void) | undefined;

  constructor(origin: string, private readonly sessionId: string, private readonly counters: NativeCounters) {
    const url = new URL(`${sessionPath(sessionId)}/stream`, origin);
    url.protocol = 'ws:';
    this.socket = new WebSocket(url, { origin, handshakeTimeout: 5000, maxPayload: 65 * 1024 * 1024 });
    this.socket.on('open', () => { this.opened = true; this.wake?.(); });
    this.socket.on('close', () => { this.ended = true; this.wake?.(); });
    this.socket.on('error', () => {
      this.failure = new Error('Native WebSocket connection failed');
      this.wake?.();
    });
    this.socket.on('message', (data: RawData, binary: boolean) => {
      try {
        const bytes = Buffer.isBuffer(data) ? data : Array.isArray(data) ? Buffer.concat(data) : Buffer.from(data);
        if (binary) {
          const packet = decodePacket(bytes);
          if (++this.counters.receivedFrames > 64 || packet.meta.sessionId !== this.sessionId ||
              packet.meta.sceneRevision !== 1 || packet.meta.seq <= this.counters.lastSeq) {
            throw new Error('Native frame count, identity or monotonicity violation');
          }
          this.counters.lastSeq = packet.meta.seq;
          this.packet = packet;
        } else {
          if (bytes.length > 64 * 1024) throw new Error('Excessive native JSON error');
          const error = record(record(JSON.parse(bytes.toString('utf8')) as unknown).error);
          if (typeof error.code !== 'string' || error.code !== 'invalid_render' || ++this.counters.invalidRenders > 4) {
            throw new Error('Unexpected native protocol error');
          }
          this.errorCode = error.code;
        }
      } catch {
        this.failure = new Error('Native packet validation failed');
      }
      this.wake?.();
    });
  }

  private wait<Value>(predicate: () => Value | undefined, timeout: number, allowClosed = false): Promise<Value> {
    if (this.wake) return Promise.reject(new Error('Only one native protocol wait may be pending'));
    return new Promise<Value>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.wake = undefined;
        reject(new Error('Bounded native protocol wait timed out'));
      }, timeout);
      const check = () => {
        const value = predicate();
        if (this.failure || value !== undefined || (!allowClosed && this.ended)) {
          clearTimeout(timer);
          this.wake = undefined;
          if (this.failure) reject(this.failure);
          else if (value !== undefined) resolve(value);
          else reject(new Error('Native stream closed before its expected response'));
        }
      };
      this.wake = check;
      check();
    });
  }

  open(): Promise<true> {
    return this.wait(() => this.opened ? true : undefined, 5000);
  }

  send(render: RenderRequest): void {
    this.assertHealthy();
    if (this.socket.readyState !== WebSocket.OPEN) throw new Error('Native stream is not open');
    if (++this.counters.sentRequests > 110) throw new Error('Native request budget exceeded');
    this.socket.send(JSON.stringify(render));
  }

  frame(seq: number): Promise<NativePacket> {
    return this.wait(() => this.packet?.meta.seq === seq ? this.packet : undefined, 15_000);
  }

  invalid(after: number): Promise<true> {
    return this.wait(() => this.counters.invalidRenders > after && this.errorCode === 'invalid_render' ? true : undefined, 5000);
  }

  closed(): Promise<true> {
    return this.wait(() => this.ended ? true : undefined, 5000, true);
  }

  assertHealthy(): void {
    if (this.failure) throw this.failure;
  }

  async close(): Promise<void> {
    try {
      if (this.socket.readyState === WebSocket.OPEN) this.socket.close(1000, 'Stability release');
      else if (this.socket.readyState === WebSocket.CONNECTING) this.socket.terminate();
      await this.closed();
    } finally {
      this.socket.terminate();
    }
  }
}

async function create(request: APIRequestContext, seriesId: string): Promise<Session> {
  const response = await request.post(sessionsPath, { data: { seriesId }, timeout: 5000 });
  try {
    expect(response.status()).toBe(202);
    return parseSession(await response.json() as unknown);
  } finally { await response.dispose(); }
}

async function initialView(request: APIRequestContext, sessionId: string): Promise<Session> {
  let snapshot: Session | undefined;
  await expect.poll(async () => {
    const response = await request.get(sessionPath(sessionId), { timeout: 3000 });
    try {
      expect(response.status()).toBe(200);
      snapshot = parseSession(await response.json() as unknown);
      expect(snapshot.sessionId === sessionId, 'Ready snapshot must belong to the created session').toBe(true);
      return snapshot.state;
    } finally { await response.dispose(); }
  }, { timeout: 30_000, intervals: [50, 100, 250, 500] }).toBe('ready');
  if (!snapshot?.initial || snapshot.sceneRevision !== 1) throw new Error('Missing ready native initial view');
  return snapshot;
}

function render(snapshot: Session, seq: number, quality: 'preview' | 'final', size = 256): RenderRequest {
  if (!snapshot.initial || snapshot.sceneRevision !== 1) throw new Error('Missing native render settings');
  return { type: 'render', seq, quality, sceneRevision: snapshot.sceneRevision, width: size, height: size,
    camera: structuredClone(snapshot.initial.camera), style: structuredClone(snapshot.initial.style) };
}

async function conflict(request: APIRequestContext): Promise<void> {
  const response = await request.post(sessionsPath, { data: { seriesId: 'phantom' }, timeout: 5000 });
  try {
    expect(response.status()).toBe(409);
    const error = record(record(await response.json() as unknown).error);
    expect(error.code).toBe('session_conflict');
  } finally { await response.dispose(); }
}

test('native protocol stability: bounded latest-wins burst, rejection, reconnect and heavy-render deletion',
  async ({ page, request, baseURL }, info) => {
    const origin = isolatedOrigin(baseURL);
    summary.native.status = 'failed';
    const ids = new Set<string>();
    const streams = new Set<NativeStream>();
    let completed = false;
    try {
      await page.goto('about:blank');
      const created = await create(request, 'phantom');
      ids.add(created.sessionId);
      expect(created.state).toBe('loading');
      await conflict(request);
      const snapshot = await initialView(request, created.sessionId);
      let stream = new NativeStream(origin, created.sessionId, summary.native);
      streams.add(stream);
      await stream.open();
      const finalRequest = render(snapshot, 101, 'final');
      for (let seq = 1; seq <= 100; seq++) stream.send(render(snapshot, seq, 'preview'));
      stream.send(finalRequest);
      const packet = await stream.frame(101);
      expect(packet.meta.seq).toBe(101);
      expect(packet.meta.sceneRevision).toBe(1);
      expect(packet.meta.quality).toBe('final');
      expect(packet.meta.encoding).toBe('png');
      expect(packet.meta.width).toBe(finalRequest.width);
      expect(packet.meta.height).toBe(finalRequest.height);
      if (packet.meta.camera !== undefined) expect(packet.meta.camera).toEqual(finalRequest.camera);
      expect(summary.native.sentRequests).toBe(101);
      const cached = await request.get(`${sessionPath(created.sessionId)}/frame?seq=101&revision=1`, { timeout: 5000 });
      try {
        expect(cached.status()).toBe(200);
        expect(cached.headers()['content-type']).toContain('image/png');
        expect((await cached.body()).equals(packet.image)).toBe(true);
      } finally { await cached.dispose(); }
      let errorsBefore = summary.native.invalidRenders;
      stream.send(render(snapshot, 100, 'final'));
      await stream.invalid(errorsBefore);
      errorsBefore = summary.native.invalidRenders;
      stream.send({ ...render(snapshot, 102, 'final'), sceneRevision: 2 });
      await stream.invalid(errorsBefore);
      expect(summary.native.lastSeq).toBe(101);
      await stream.close();
      streams.delete(stream);
      expect(await sessionState(request, created.sessionId)).toBe('ready');
      stream = new NativeStream(origin, created.sessionId, summary.native);
      streams.add(stream);
      await stream.open();
      stream.send(render(snapshot, 102, 'final'));
      expect((await stream.frame(102)).meta.seq).toBe(102);
      summary.native.reconnects++;
      errorsBefore = summary.native.invalidRenders;
      stream.send(render(snapshot, 101, 'final'));
      await stream.invalid(errorsBefore);
      errorsBefore = summary.native.invalidRenders;
      stream.send({ ...render(snapshot, 103, 'final'), sceneRevision: 2 });
      await stream.invalid(errorsBefore);
      stream.send(render(snapshot, 103, 'final', 1024));
      const deletion = await request.delete(sessionPath(created.sessionId), { timeout: 5000 });
      try { expect(deletion.status()).toBe(202); }
      finally { await deletion.dispose(); }
      await Promise.all([closed(request, created.sessionId), stream.closed()]);
      stream.assertHealthy();
      await stream.close();
      streams.delete(stream);
      summary.native.closedSessions++;
      const expired = await request.get(`${sessionPath(created.sessionId)}/frame?seq=103&revision=1`, { timeout: 5000 });
      try { expect([404, 410].includes(expired.status())).toBe(true); }
      finally { await expired.dispose(); }
      const replacement = await create(request, 'phantom');
      ids.add(replacement.sessionId);
      expect(ids.size, 'DELETE must free the active slot for a distinct new session').toBe(2);
      await initialView(request, replacement.sessionId);
      await conflict(request);
      await release(request, replacement.sessionId);
      summary.native.closedSessions++;
      for (const sessionId of ids) await closed(request, sessionId);
      expect(summary.native.invalidRenders).toBe(4);
      expect(summary.native.sentRequests).toBe(107);
      expect(summary.native.receivedFrames).toBeGreaterThanOrEqual(2);
      expect(summary.native.receivedFrames).toBeLessThanOrEqual(64);
      completed = true;
    } finally {
      let cleanupSucceeded = false;
      try {
        try {
          await Promise.all([...streams].map(stream => stream.close()));
        } finally {
          await Promise.all([...ids].map(sessionId => release(request, sessionId)));
        }
        cleanupSucceeded = true;
      } finally {
        summary.native.status = completed && cleanupSucceeded ? 'passed' : 'failed';
        await saveSummary(info);
      }
    }
  });
