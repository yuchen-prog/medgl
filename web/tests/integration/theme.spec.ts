import { expect } from '@playwright/test';
import type { Page } from '@playwright/test';
import { readFile } from 'node:fs/promises';
import { test } from './fixtures';
import type { RenderRequest, Session } from '../../src/protocol';
import { THEME_STORAGE_KEY } from '../../src/theme';

async function finalFrame(page: Page) {
  await expect(page.getByRole('button', { name: 'Download final PNG', exact: true })).toBeEnabled({ timeout: 30000 });
  await expect(page.locator('.render-indicator')).toHaveCount(0);
}

async function changed(page: Page, action: () => Promise<unknown>) {
  const before = await page.locator('.viewport-bottom-right .mono').innerText();
  await action();
  await expect(page.locator('.viewport-bottom-right .mono')).not.toHaveText(before);
  await finalFrame(page);
}

async function pixels(page: Page) {
  return page.getByLabel('Server-rendered volume image', { exact: true }).evaluate((canvas: HTMLCanvasElement) =>
    [...canvas.getContext('2d')!.getImageData(0, 0, canvas.width, canvas.height).data]);
}

test('dark theme preserves GPU images, export and scene state, persists and fits desktop/mobile', async ({ page, request }) => {
  const renders: RenderRequest[] = [];
  const sessions: string[] = [];
  const errors: string[] = [];
  page.on('pageerror', error => errors.push(error.message));
  page.on('response', response => {
    if (response.request().method() === 'POST' && response.status() === 202 &&
        new URL(response.url()).pathname === '/api/v1/sessions')
      void response.json().then((session: Session) => sessions.push(session.sessionId));
  });
  page.on('websocket', socket => socket.on('framesent', event => {
    renders.push(JSON.parse(event.payload.toString()) as RenderRequest);
  }));
  await page.setViewportSize({ width: 1440, height: 960 });
  await page.goto('/');
  await finalFrame(page);
  const toggle = page.getByRole('switch', { name: 'Dark theme', exact: true });
  await expect(toggle).not.toBeChecked();
  await changed(page, () => page.getByRole('combobox', { name: 'Render size', exact: true }).selectOption('50%'));
  await page.getByText('Lighting and background', { exact: true }).click();
  await changed(page, () => page.getByRole('button', { name: 'White background', exact: true }).click());
  await page.getByRole('tab', { name: 'Crop', exact: true }).click();
  await changed(page, () => page.getByRole('checkbox', { name: 'Enable clipping plane', exact: true }).check());
  await page.getByText('Precision settings', { exact: true }).click();
  await changed(page, () => page.getByRole('spinbutton', { name: 'Tilt (LPS)', exact: true }).fill('55'));
  await changed(page, () => page.getByRole('checkbox', { name: 'Enable crop', exact: true }).check());
  await changed(page, () => page.getByRole('spinbutton', { name: 'Columns (X) from', exact: true }).fill('20'));
  await page.getByRole('tab', { name: 'Display', exact: true }).click();
  await changed(page, () => page.getByRole('button', { name: 'Edit transfer function', exact: true }).click());
  await page.getByRole('button', { name: 'Curve point 3', exact: true }).click();
  await changed(page, () => page.getByRole('spinbutton', { name: 'Point opacity', exact: true }).fill('5'));
  await changed(page, () => page.getByRole('spinbutton', { name: 'Point opacity', exact: true }).blur());
  const original = await pixels(page);
  const originalRender = structuredClone(renders.at(-1));
  const renderCount = renders.length;
  const sessionCount = sessions.length;
  const sequence = await page.locator('.viewport-bottom-right .mono').innerText();
  const viewport = await page.getByTestId('viewport').boundingBox();
  await toggle.check();
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
  await expect(page.locator('meta[name="theme-color"]')).toHaveAttribute('content', '#161a1a');
  expect(await pixels(page)).toEqual(original);
  expect(renders).toHaveLength(renderCount);
  expect(renders.at(-1)).toEqual(originalRender);
  expect(sessions).toHaveLength(sessionCount);
  await expect(page.locator('.viewport-bottom-right .mono')).toHaveText(sequence);
  expect(await page.getByTestId('viewport').boundingBox()).toEqual(viewport);
  const backgrounds = await page.locator('.app-header, .sidebar, .viewer-toolbar, .standard-viewbar, .transfer-dock').evaluateAll(elements =>
    elements.map(element => getComputedStyle(element).backgroundColor));
  expect(backgrounds.every(color => color === 'rgb(32, 38, 38)')).toBe(true);
  expect(await page.locator('html').evaluate(element => getComputedStyle(element).colorScheme)).toBe('dark');
  expect(await page.evaluate(key => localStorage.getItem(key), THEME_STORAGE_KEY)).toBe('dark');
  await expect(page.getByRole('button', { name: 'White background', exact: true })).toHaveAttribute('aria-pressed', 'true');
  await page.mouse.move(1400, 20);
  await page.screenshot({ path: 'test-results-real/theme-dark-white-background.png', animations: 'disabled' });
  await page.getByRole('tab', { name: 'Crop', exact: true }).click();
  await page.screenshot({ path: 'test-results-real/theme-dark-crop.png', animations: 'disabled' });
  await page.getByRole('tab', { name: 'Info', exact: true }).click();
  await page.screenshot({ path: 'test-results-real/theme-dark-info.png', animations: 'disabled' });
  await page.getByRole('tab', { name: 'Display', exact: true }).click();
  const [download] = await Promise.all([page.waitForEvent('download'),
    page.getByRole('button', { name: 'Download final PNG', exact: true }).click()]);
  const png = await readFile((await download.path())!);
  const difference = await page.getByLabel('Server-rendered volume image', { exact: true }).evaluate(async (canvas: HTMLCanvasElement, bytes) => {
    const bitmap = await createImageBitmap(new Blob([new Uint8Array(bytes)], { type: 'image/png' }));
    const exported = new OffscreenCanvas(bitmap.width, bitmap.height);
    const context = exported.getContext('2d')!;
    context.drawImage(bitmap, 0, 0);
    bitmap.close();
    const saved = context.getImageData(0, 0, exported.width, exported.height).data;
    const shown = canvas.getContext('2d')!.getImageData(0, 0, canvas.width, canvas.height).data;
    return saved.length === shown.length ? saved.reduce((total, value, index) => total + Number(value !== shown[index]), 0) : -1;
  }, [...png]);
  expect(difference).toBe(0);
  await toggle.uncheck();
  expect(await pixels(page)).toEqual(original);
  expect(renders).toHaveLength(renderCount);
  await page.screenshot({ path: 'test-results-real/theme-light-desktop.png', animations: 'disabled' });
  await toggle.check();
  const previousSession = sessions.at(-1)!;
  await page.goto('about:blank');
  await request.delete(`/api/v1/sessions/${previousSession}`);
  await expect.poll(async () => {
    const response = await request.get(`/api/v1/sessions/${previousSession}`);
    try { return [404, 410].includes(response.status()) || (await response.json() as Session).state === 'closed'; }
    finally { await response.dispose(); }
  }).toBe(true);
  await page.goto('/');
  await finalFrame(page);
  await expect(toggle).toBeChecked();
  await changed(page, () => page.getByRole('button', { name: 'Edit transfer function', exact: true }).click());
  await page.mouse.move(1400, 20);
  await page.screenshot({ path: 'test-results-real/theme-dark-desktop.png', animations: 'disabled' });
  await changed(page, () => page.getByRole('button', { name: 'Close transfer editor', exact: true }).click());
  for (const width of [390, 320]) {
    await changed(page, () => page.setViewportSize({ width, height: 844 }));
    const toggleBounds = (await toggle.boundingBox())!;
    expect(toggleBounds.x + toggleBounds.width).toBeLessThanOrEqual(width);
    expect(toggleBounds.x).toBeGreaterThanOrEqual(0);
    await page.getByRole('button', { name: 'Open settings', exact: true }).click();
    await page.screenshot({ path: `test-results-real/theme-dark-drawer-${width}.png`, animations: 'disabled' });
    const drawer = page.getByRole('complementary', { name: 'Volume settings', exact: true });
    expect(await drawer.evaluate(element => getComputedStyle(element).backgroundColor)).toBe('rgb(32, 38, 38)');
    await changed(page, () => page.getByRole('button', { name: 'Edit transfer function', exact: true }).click());
    await expect(page.getByRole('region', { name: 'Transfer function editor', exact: true })).toBeVisible();
    await page.mouse.move(width - 5, 60);
    await page.screenshot({ path: `test-results-real/theme-dark-mobile-${width}.png`, animations: 'disabled' });
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
    const before = await pixels(page);
    const count = renders.length;
    await toggle.uncheck();
    expect(await pixels(page)).toEqual(before);
    expect(renders).toHaveLength(count);
    await toggle.check();
    await changed(page, () => page.getByRole('button', { name: 'Close transfer editor', exact: true }).click());
  }
  expect(errors).toEqual([]);
});