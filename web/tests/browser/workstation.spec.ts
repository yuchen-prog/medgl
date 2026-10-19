import { expect, test } from '@playwright/test';
import type { Page } from '@playwright/test';
import type { RenderRequest } from '../../src/protocol';
import { THEME_STORAGE_KEY } from '../../src/theme';

const initial = {
  camera: { targetMm: [80, 90, 100], rotationXyzw: [0, 0, 0, 1], orthoHeightMm: 240 },
  style: { presetId: 'ct-bone', windowWidth: 1800, windowLevel: 400, opacityScale: 1, backgroundRgb: [0.055, 0.065, 0.065] }
};

const half = Math.SQRT1_2;
const standardViews = [
  { id: 'anterior', name: 'Anterior', rotationXyzw: [0, half, half, 0] },
  { id: 'posterior', name: 'Posterior', rotationXyzw: [half, 0, 0, half] },
  { id: 'left', name: 'Left', rotationXyzw: [0.5, -0.5, -0.5, 0.5] },
  { id: 'right', name: 'Right', rotationXyzw: [0.5, 0.5, 0.5, 0.5] },
  { id: 'superior', name: 'Superior', rotationXyzw: [0, 1, 0, 0] },
  { id: 'inferior', name: 'Inferior', rotationXyzw: [0, 0, 0, 1] }
];

async function image(page: Page, width: number, height: number): Promise<Buffer> {
  const encoded = await page.evaluate(({ width, height }) => {
    const canvas = document.createElement('canvas');
    canvas.width = width;
    canvas.height = height;
    const context = canvas.getContext('2d')!;
    context.fillStyle = '#0e1111';
    context.fillRect(0, 0, width, height);
    context.fillStyle = '#39aaa4';
    context.fillRect(width * .3, height * .25, width * .4, height * .5);
    context.fillStyle = '#ffffff';
    context.font = '18px sans-serif';
    context.textAlign = 'center';
    context.fillText('TEST FRAME', width / 2, height / 2);
    return canvas.toDataURL('image/png').split(',')[1]!;
  }, { width, height });
  return Buffer.from(encoded, 'base64');
}

async function mockService(page: Page, options: { noPhantom?: boolean; noFrames?: boolean; noCt?: boolean; failCt?: boolean } = {}) {
  const renders: RenderRequest[] = [];
  const created: string[] = [];
  const events: string[] = [];
  let nextSession = 0;
  let activeSession = '';
  let closePolls = 0;
  let cached: Buffer = Buffer.alloc(0);
  let exportQuery = '';
  const sessions = new Map<string, string>();
  const inputs = new Map<string, string>();

  await page.route('**/api/v1/**', async route => {
    const request = route.request();
    const url = new URL(request.url());
    const parts = url.pathname.split('/');
    const id = parts[4] ?? '';
    if (url.pathname.endsWith('/presets')) {
      await route.fulfill({ json: { presets: [
        { id: 'ct-bone', name: 'CT / Bone', windowWidth: 1800, windowLevel: 400 },
        { id: 'ct-soft-tissue', name: 'CT / Soft tissue', windowWidth: 400, windowLevel: 40 },
        { id: 'ct-heart', name: 'CT / Heart (contrast)', windowWidth: 800, windowLevel: 200 },
        { id: 'ct-skin', name: 'CT / Skin', windowWidth: 800, windowLevel: -200 },
        { id: 'ct-lung', name: 'CT / Lung', windowWidth: 1500, windowLevel: -600 }
      ].map(preset => ({ ...preset, opacityScale: 1, lighting: true, colors: [[0, 0, 0], [1, 0.3, 0.2]] })) } });
    } else if (url.pathname.endsWith('/series')) {
      await route.fulfill({ json: { series: [
        ...options.noPhantom ? [] : [{ id: 'phantom', description: 'Synthetic phantom', fileCount: 0, supported: true, reason: '' }],
        ...options.noCt ? [] : [{ id: 'patient-series', description: 'CT series', fileCount: 256, supported: true, reason: '' }],
        { id: 'unsupported', description: 'Nonuniform series', fileCount: 12, supported: false, reason: 'Nonuniform slice spacing' }
      ] } });
    } else if (request.method() === 'POST') {
      const body = request.postDataJSON() as { seriesId: string };
      activeSession = `session-${++nextSession}`;
      created.push(body.seriesId);
      events.push(`create:${activeSession}`);
      sessions.set(activeSession, 'loading');
      inputs.set(activeSession, body.seriesId);
      await route.fulfill({ status: 202, json: { sessionId: activeSession, state: 'loading' } });
    } else if (request.method() === 'DELETE') {
      events.push(`delete:${id}`);
      sessions.set(id, 'closing');
      closePolls = 0;
      await route.fulfill({ status: 202, json: { sessionId: id, state: 'closing' } });
    } else if (url.pathname.endsWith('/frame')) {
      exportQuery = url.search;
      await route.fulfill({ status: 200, contentType: 'image/png', body: cached });
    } else {
      let state = sessions.get(id);
      if (state === 'closing') {
        closePolls++;
        if (closePolls >= 3) { state = 'closed'; sessions.set(id, state); events.push(`closed:${id}`); }
      } else if (state === 'loading') {
        state = options.failCt && inputs.get(id) === 'patient-series' ? 'failed' : 'ready';
        sessions.set(id, state);
      }
      await route.fulfill({ json: { sessionId: id, state, ...(state === 'ready' ? {
        sceneRevision: 1, volume: { dimensions: [160, 180, 200], spacing: [1, 1, 1] }, initial, standardViews
      } : {}) } });
    }
  });
  await page.routeWebSocket('**/api/v1/sessions/*/stream', socket => {
    const sessionId = new URL(socket.url()).pathname.split('/')[4]!;
    socket.onMessage(async message => {
      const request = JSON.parse(message.toString()) as RenderRequest;
      renders.push(request);
      if (options.noFrames || page.isClosed()) return;
      const payload = await image(page, request.width, request.height);
      if (request.quality === 'final') cached = payload;
      const meta = Buffer.from(JSON.stringify({ protocolVersion: 1, sessionId, seq: request.seq,
        sceneRevision: request.sceneRevision, width: request.width, height: request.height,
        camera: request.camera,
        encoding: 'png', payloadBytes: payload.length, quality: request.quality }));
      const header = Buffer.alloc(4);
      header.writeUInt32LE(meta.length);
      socket.send(Buffer.concat([header, meta, payload]));
    });
  });
  return { renders, created, events, exportQuery: () => exportQuery };
}

async function expectReady(page: Page) {
  await expect(page.getByRole('button', { name: 'Download final PNG' })).toBeEnabled();
  await expect.poll(() => page.locator('canvas').evaluate((canvas: HTMLCanvasElement) => {
    const context = canvas.getContext('2d')!;
    const pixel = context.getImageData(Math.round(canvas.width * .4), Math.round(canvas.height * .4), 1, 1).data;
    return (pixel[1] ?? 0) > 100 && pixel[3] === 255;
  })).toBe(true);
  await expect(page.getByRole('button', { name: 'Download final PNG' })).toBeEnabled();
}

test('CT autoload, rotate, pan, zoom, styles, reset and final PNG export', async ({ page }) => {
  await page.setViewportSize({ width: 1440, height: 960 });
  const errors: string[] = [];
  page.on('pageerror', error => errors.push(error.message));
  const service = await mockService(page);
  await page.goto('/');
  await expectReady(page);
  expect(service.created).toEqual(['patient-series']);
  await expect(page.getByRole('combobox', { name: 'Series', exact: true })).toHaveValue('patient-series');
  await expect(page.locator('.viewport-series')).toHaveText('CT series');
  expect(service.renders[0]!.width).toBeGreaterThan(1);
  expect(service.renders[0]!.height).toBeGreaterThan(1);
  const viewbar = page.getByRole('group', { name: 'Standard views', exact: true });
  await expect(viewbar.getByRole('button')).toHaveCount(6);
  await expect(page.getByRole('complementary', { name: 'Volume settings' }).getByRole('button', { name: / view$/ })).toHaveCount(0);
  await expect(page.getByRole('combobox', { name: 'Standard view' })).toHaveCount(0);
  await page.getByRole('button', { name: 'Anterior view', exact: true }).click();
  await expect.poll(() => service.renders.at(-1)?.camera.rotationXyzw).toEqual([0, half, half, 0]);
  await expectReady(page);
  await expect(page.getByRole('button', { name: 'Anterior view', exact: true })).toHaveAttribute('aria-pressed', 'true');
  await expect(viewbar.locator('button[aria-pressed=true]')).toHaveCount(1);
  await expect(page.getByTestId('orientation-top')).toHaveText('S');
  await expect(page.getByTestId('orientation-left')).toHaveText('L');
  await expect(page.getByTestId('orientation-right')).toHaveText('R');
  await expect(page.getByTestId('orientation-bottom')).toHaveText('I');
  await page.getByRole('button', { name: 'Reset camera' }).click();
  await expectReady(page);
  await expect(page.getByTestId('orientation-top')).toHaveText('P');
  await page.getByText('Lighting and background', { exact: true }).click();
  for (const preset of [
    { id: 'ct-heart', width: 800, level: 200 },
    { id: 'ct-skin', width: 800, level: -200 },
    { id: 'ct-bone', width: 1800, level: 400 }
  ]) {
    await page.getByRole('spinbutton', { name: 'Opacity scale', exact: true }).fill('2.5');
    await page.getByRole('spinbutton', { name: 'Opacity scale', exact: true }).press('Enter');
    await page.getByRole('checkbox', { name: 'Gradient lighting' }).uncheck();
    await page.getByRole('combobox', { name: 'Transfer preset' }).selectOption(preset.id);
    await expect.poll(() => service.renders.at(-1)?.style.presetId).toBe(preset.id);
    await expectReady(page);
    expect(service.renders.at(-1)?.style).toMatchObject({ presetId: preset.id,
      windowWidth: preset.width, windowLevel: preset.level, opacityScale: 1, lighting: true });
    await expect(page.getByRole('img', { name: new RegExp('color table$') })).toBeVisible();
    await page.getByRole('spinbutton', { name: 'Window width', exact: true }).fill('2300');
    await page.getByRole('spinbutton', { name: 'Window width', exact: true }).press('Enter');
    await page.getByRole('button', { name: 'Reset preset', exact: true }).click();
    await expect.poll(() => service.renders.at(-1)?.style.windowWidth).toBe(preset.width);
    await expectReady(page);
    expect(service.renders.at(-1)?.style.presetId).toBe(preset.id);
  }
  const viewport = page.getByTestId('viewport');
  const bounds = (await viewport.boundingBox())!;
  const startX = bounds.x + bounds.width * .45;
  const startY = bounds.y + bounds.height * .45;
  await page.mouse.move(startX, startY);
  await page.mouse.down();
  await page.mouse.move(startX + 100, startY + 50, { steps: 10 });
  await page.mouse.up();
  await expect.poll(() => service.renders.at(-1)?.quality).toBe('final');
  await expectReady(page);
  expect(service.renders.at(-1)?.camera.rotationXyzw).not.toEqual(initial.camera.rotationXyzw);
  await expect(viewbar.locator('button[aria-pressed=true]')).toHaveCount(0);
  await page.mouse.down({ button: 'right' });
  await page.mouse.move(startX + 140, startY + 65, { steps: 4 });
  await page.mouse.up({ button: 'right' });
  await expectReady(page);
  expect(service.renders.at(-1)?.camera.targetMm).not.toEqual(initial.camera.targetMm);
  await page.mouse.wheel(0, -180);
  await expectReady(page);
  expect(service.renders.at(-1)!.camera.orthoHeightMm).toBeLessThan(240);
  await page.getByRole('spinbutton', { name: 'Window width', exact: true }).fill('2200');
  await page.getByRole('spinbutton', { name: 'Window width', exact: true }).press('Enter');
  await expectReady(page);
  expect(service.renders.at(-1)?.style.windowWidth).toBe(2200);
  await page.getByRole('button', { name: 'White background' }).click();
  await expectReady(page);
  expect(service.renders.at(-1)?.style.backgroundRgb).toEqual([1, 1, 1]);
  await page.getByRole('button', { name: 'Charcoal background' }).click();
  await viewport.dblclick({ position: { x: bounds.width * .5, y: bounds.height * .5 } });
  await expectReady(page);
  expect(service.renders.at(-1)?.camera).toEqual(initial.camera);
  const downloadEvent = page.waitForEvent('download');
  await page.getByRole('button', { name: 'Download final PNG' }).click();
  const download = await downloadEvent;
  expect(download.suggestedFilename()).toMatch(/^medgl-frame-\d+\.png$/);
  expect(service.exportQuery()).toMatch(/^\?seq=\d+&revision=1$/);
  const seqs = service.renders.map(render => render.seq);
  expect(seqs.every((seq, index) => Number.isSafeInteger(seq) && (index === 0 || seq > seqs[index - 1]!))).toBe(true);
  expect(errors).toEqual([]);
  await page.screenshot({ path: 'test-results/workstation-desktop.png' });
});

test('switch waits for closed confirmation before the next POST', async ({ page }) => {
  const service = await mockService(page);
  await page.goto('/');
  await expectReady(page);
  await page.getByLabel('Series', { exact: true }).selectOption('phantom');
  await page.getByRole('button', { name: 'Load series', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Closing session' })).toBeDisabled();
  expect(service.created).toEqual(['patient-series']);
  await expectReady(page);
  expect(service.events.indexOf('closed:session-1')).toBeLessThan(service.events.indexOf('create:session-2'));
  expect(service.created).toEqual(['patient-series', 'phantom']);
});

test('loads CT without a synthetic entry and still displays unsupported reasons', async ({ page }) => {
  const service = await mockService(page, { noPhantom: true });
  await page.goto('/');
  await expectReady(page);
  const viewButtons = page.getByRole('group', { name: 'Standard views', exact: true }).getByRole('button');
  await expect(viewButtons).toHaveCount(6);
  for (const button of await viewButtons.all()) await expect(button).toBeEnabled();
  expect(service.created).toEqual(['patient-series']);
  await page.getByLabel('Series', { exact: true }).selectOption('unsupported');
  await expect(page.getByText('Nonuniform slice spacing')).toBeVisible();
  await expect(page.getByRole('button', { name: 'Load series', exact: true })).toBeDisabled();
});

test('failed CT autoload releases its session before synthetic fallback and keeps a visible notice', async ({ page }) => {
  const service = await mockService(page, { failCt: true });
  await page.goto('/');
  await expectReady(page);
  expect(service.created).toEqual(['patient-series', 'phantom']);
  expect(service.events.indexOf('closed:session-1')).toBeLessThan(service.events.indexOf('create:session-2'));
  await expect(page.getByRole('combobox', { name: 'Series', exact: true })).toHaveValue('phantom');
  await expect(page.locator('.viewport-series')).toHaveText('Synthetic phantom');
  await expect(page.getByTestId('load-warning')).toContainText('Session failed');
  await expect(page.getByTestId('load-warning')).toContainText('Showing Synthetic phantom');
  await page.getByRole('button', { name: 'Refresh series', exact: true }).click();
  await expectReady(page);
  expect(service.created).toHaveLength(2);
  await expect(page.getByTestId('load-warning')).toBeVisible();
  await page.getByRole('combobox', { name: 'Series', exact: true }).selectOption('patient-series');
  await page.getByRole('button', { name: 'Load series', exact: true }).click();
  await expect(page.getByRole('alert')).toContainText('Session failed');
  expect(service.created).toEqual(['patient-series', 'phantom', 'patient-series']);
  await expect(page.getByTestId('load-warning')).toHaveCount(0);
});

test('loads synthetic directly when no supported CT exists', async ({ page }) => {
  const service = await mockService(page, { noCt: true });
  await page.goto('/');
  await expectReady(page);
  expect(service.created).toEqual(['phantom']);
  await expect(page.getByRole('combobox', { name: 'Series', exact: true })).toHaveValue('phantom');
  await expect(page.locator('.synthetic-label')).toBeVisible();
});

test('narrow drawer and canvas stay in bounds at 320px and 390px', async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await mockService(page);
  await page.goto('/');
  await expectReady(page);
  for (const width of [320, 390]) {
    await page.setViewportSize({ width, height: 844 });
    await expectReady(page);
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
    const download = (await page.getByRole('button', { name: 'Download final PNG' }).boundingBox())!;
    expect(download.x + download.width).toBeLessThanOrEqual(width);
    const viewbar = page.getByRole('group', { name: 'Standard views', exact: true });
    await expect(viewbar.getByRole('button')).toHaveCount(6);
    expect(await viewbar.evaluate(group => {
      const boundary = group.getBoundingClientRect();
      const buttons = [...group.querySelectorAll('button')].map(button => button.getBoundingClientRect());
      return buttons.every((rect, index) => rect.left >= boundary.left && rect.right <= boundary.right &&
        (index === 0 || rect.left >= buttons[index - 1]!.right));
    })).toBe(true);
    for (const view of standardViews) await expect(page.getByRole('button', { name: `${view.name} view`, exact: true })).toBeVisible();
    await page.getByRole('button', { name: 'Open settings' }).click();
    await expect(page.getByLabel('Series', { exact: true })).toBeVisible();
    const panel = (await page.getByRole('complementary', { name: 'Volume settings' }).boundingBox())!;
    expect(panel.width).toBeLessThan(width);
    await page.getByRole('combobox', { name: 'Transfer preset' }).selectOption('ct-lung');
    await expect.poll(() => page.getByRole('spinbutton', { name: 'Window level', exact: true }).inputValue()).toBe('-600');
    await page.keyboard.press('Escape');
    await expect(page.getByRole('button', { name: 'Open settings' })).toBeFocused();
    await page.getByRole('button', { name: 'Left view', exact: true }).click();
    await expect(page.getByRole('button', { name: 'Left view', exact: true })).toHaveAttribute('aria-pressed', 'true');
    await expect(page.getByTestId('orientation-top')).toHaveText('S');
  }
  await expectReady(page);
  await page.screenshot({ path: 'test-results/workstation-mobile.png' });
});

test('a missing frame times out without a permanent pending spinner', async ({ page }) => {
  await page.clock.install();
  await mockService(page, { noFrames: true });
  await page.goto('/');
  await expect(page.getByText('Awaiting render', { exact: true })).toBeVisible();
  await page.clock.fastForward(13000);
  await expect(page.getByRole('alert')).toContainText('No image received within 12 seconds', { timeout: 15000 });
  await expect(page.getByText('Image unavailable', { exact: true })).toBeVisible();
  await expect(page.getByRole('button', { name: 'Reconnect stream' })).toBeVisible();
  const alert = (await page.getByRole('alert').boundingBox())!;
  await expect.poll(async () => (await page.locator('.viewport-top-left').boundingBox())!.y).toBeGreaterThan(alert.y + alert.height);
  await page.screenshot({ path: 'test-results/workstation-timeout.png' });
});

test('icon tabs keep common controls visible and support keyboard navigation without changing view state', async ({ page }) => {
  await page.setViewportSize({ width: 1440, height: 960 });
  const service = await mockService(page);
  await page.goto('/');
  await expectReady(page);
  const tabs = page.getByRole('tablist', { name: 'Settings categories' });
  await expect(tabs.getByRole('tab')).toHaveCount(3);
  for (const name of ['Display', 'Crop', 'Info']) await expect(tabs.getByRole('tab', { name, exact: true }).locator('svg')).toHaveCount(1);
  await expect(page.getByRole('tab', { name: 'Display', exact: true })).toHaveAttribute('aria-selected', 'true');
  await expect(page.getByTestId('frame-timings')).toBeHidden();
  for (const name of ['Window width', 'Window level', 'Opacity scale']) {
    const bounds = (await page.getByRole('spinbutton', { name, exact: true }).boundingBox())!;
    expect(bounds.y + bounds.height).toBeLessThan(960);
  }
  await expect(page.getByRole('region', { name: 'Transfer function editor', exact: true })).toHaveCount(0);
  await expect(page.getByRole('button', { name: 'Edit transfer function', exact: true })).toBeDisabled();
  const initialView = service.renders.at(-1);
  await page.getByRole('tab', { name: 'Display', exact: true }).focus();
  await page.keyboard.press('ArrowRight');
  await expect(page.getByRole('tab', { name: 'Crop', exact: true })).toBeFocused();
  await expect(page.getByTestId('frame-timings')).toBeHidden();
  await expect(page.getByRole('checkbox', { name: 'Enable clipping plane', exact: true })).toBeVisible();
  await expect(page.getByRole('spinbutton', { name: 'Plane position', exact: true })).toHaveCount(0);
  await expect(page.getByRole('spinbutton', { name: 'Columns (X) from', exact: true })).toHaveCount(0);
  await page.keyboard.press('End');
  await expect(page.getByRole('tab', { name: 'Info', exact: true })).toBeFocused();
  await expect(page.locator('.geometry')).toBeVisible();
  await expect(page.getByTestId('frame-timings')).toBeVisible();
  await page.keyboard.press('Home');
  await expect(page.getByRole('tab', { name: 'Display', exact: true })).toBeFocused();
  expect(service.renders.at(-1)?.camera).toEqual(initialView?.camera);
  expect(service.renders.at(-1)?.style).toEqual(initialView?.style);
  await page.screenshot({ path: 'test-results/settings-tabs-desktop.png' });
});

test('theme preference defaults to light, persists, and recovers from invalid stored values', async ({ page }) => {
  const service = await mockService(page);
  await page.emulateMedia({ colorScheme: 'dark' });
  await page.goto('/');
  await expectReady(page);
  const toggle = page.getByRole('switch', { name: 'Dark theme', exact: true });
  await expect(toggle).not.toBeChecked();
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'light');
  const renders = service.renders.length;
  await toggle.check();
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
  expect(service.renders).toHaveLength(renders);
  expect(await page.evaluate(key => localStorage.getItem(key), THEME_STORAGE_KEY)).toBe('dark');
  await page.reload();
  await expectReady(page);
  await expect(toggle).toBeChecked();
  expect(await page.locator('html').evaluate(element => getComputedStyle(element).colorScheme)).toBe('dark');
  await toggle.focus();
  await page.keyboard.press('Space');
  await expect(toggle).not.toBeChecked();
  await page.reload();
  await expectReady(page);
  await expect(toggle).not.toBeChecked();
  await page.evaluate(key => localStorage.setItem(key, 'invalid'), THEME_STORAGE_KEY);
  await page.reload();
  await expectReady(page);
  await expect(toggle).not.toBeChecked();
});

test('theme still switches when browser storage is blocked and reports that it cannot be saved', async ({ page }) => {
  await page.addInitScript(() => Object.defineProperty(window, 'localStorage', {
    configurable: true, get() { throw new DOMException('Storage blocked', 'SecurityError'); }
  }));
  const errors: string[] = [];
  page.on('pageerror', error => errors.push(error.message));
  await mockService(page);
  await page.goto('/');
  await expectReady(page);
  const toggle = page.getByRole('switch', { name: 'Dark theme', exact: true });
  await toggle.check();
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
  await expect(page.locator('.theme-storage-notice')).toContainText('Browser storage is unavailable');
  await toggle.uncheck();
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'light');
  expect(errors).toEqual([]);
});