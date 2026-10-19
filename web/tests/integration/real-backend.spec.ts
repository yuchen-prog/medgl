import { expect } from '@playwright/test';
import { test } from './fixtures';
import type { Page } from '@playwright/test';
import { readFile } from 'node:fs/promises';
import type { AppearancePreset } from '../../src/protocol';

async function ready(page: Page) {
  await expect(page.getByRole('button', { name: 'Download final PNG' })).toBeEnabled({ timeout: 30000 });
  await expect(page.locator('.render-indicator')).toHaveCount(0);
}

async function pixelSummary(page: Page) {
  return page.locator('canvas').evaluate((canvas: HTMLCanvasElement) => {
    const pixels = canvas.getContext('2d')!.getImageData(0, 0, canvas.width, canvas.height).data;
    let checksum = 0;
    let nonzero = 0;
    let minimum = 765;
    let maximum = 0;
    for (let offset = 0; offset < pixels.length; offset += 4) {
      const value = pixels[offset]! + pixels[offset + 1]! + pixels[offset + 2]!;
      if (value > 0) nonzero++;
      minimum = Math.min(minimum, value);
      maximum = Math.max(maximum, value);
      checksum = (checksum + value * (1 + (offset % 97))) % 2147483647;
    }
    return { width: canvas.width, height: canvas.height, nonzero, minimum, maximum, checksum };
  });
}

test('real DICOM, CUDA frames, interaction, presets, PNG export and responsive canvas', async ({ page }) => {
  const errors: string[] = [];
  const external: string[] = [];
  await page.route('**/*', route => {
    const url = new URL(route.request().url());
    if (url.protocol.startsWith('http') && !['127.0.0.1', 'localhost'].includes(url.hostname)) {
      external.push(url.hostname);
      return route.abort();
    }
    return route.continue();
  });
  page.on('pageerror', error => errors.push(error.message));
  await page.setViewportSize({ width: 1440, height: 960 });
  await page.goto('/');
  await ready(page);
  expect(await page.evaluate(async () => {
    await document.fonts.ready;
    return document.fonts.check('12px "IBM Plex Sans Variable"') && document.fonts.check('12px "IBM Plex Mono"');
  })).toBe(true);
  await expect(page.getByTestId('frame-timings')).not.toContainText('--');
  await expect(page.getByTestId('send-to-canvas')).toHaveText(/^\d+ ms$/);
  const first = await pixelSummary(page);
  expect(first.nonzero).toBeGreaterThan(1000);
  expect(first.maximum - first.minimum).toBeGreaterThan(100);

  await page.getByLabel('Series', { exact: true }).selectOption('series-1');
  await page.getByRole('button', { name: 'Load series', exact: true }).click();
  await expect(page.locator('.geometry')).toContainText('64 × 64 × 64', { timeout: 30000 });
  await ready(page);
  const before = await pixelSummary(page);
  const viewbar = page.getByRole('group', { name: 'Standard views', exact: true });
  await expect(viewbar.getByRole('button')).toHaveCount(6);
  await expect(page.getByRole('combobox', { name: 'Standard view' })).toHaveCount(0);
  const views = [
    { id: 'anterior', top: 'S', left: 'L', right: 'R', bottom: 'I' },
    { id: 'posterior', top: 'S', left: 'R', right: 'L', bottom: 'I' },
    { id: 'left', top: 'S', left: 'P', right: 'A', bottom: 'I' },
    { id: 'right', top: 'S', left: 'A', right: 'P', bottom: 'I' },
    { id: 'superior', top: 'P', left: 'L', right: 'R', bottom: 'A' },
    { id: 'inferior', top: 'P', left: 'R', right: 'L', bottom: 'A' }
  ];
  for (const view of views) {
    const previousSequence = await page.locator('.viewport-bottom-right .mono').innerText();
    const viewButton = page.getByRole('button', { name: `${view.id[0]!.toUpperCase()}${view.id.slice(1)} view`, exact: true });
    await viewButton.click();
    await expect(page.locator('.viewport-bottom-right .mono')).not.toHaveText(previousSequence);
    await ready(page);
    await expect(viewButton).toHaveAttribute('aria-pressed', 'true');
    await expect(viewbar.locator('button[aria-pressed=true]')).toHaveCount(1);
    for (const edge of ['top', 'left', 'right', 'bottom'] as const)
      await expect(page.getByTestId(`orientation-${edge}`)).toHaveText(view[edge]);
    await expect(page.getByRole('combobox', { name: 'Transfer preset' })).toHaveValue('ct-bone');
    expect((await pixelSummary(page)).maximum).toBeGreaterThan(50);
  }
  await page.getByRole('button', { name: 'Reset camera' }).click();
  await ready(page);
  const viewport = page.getByTestId('viewport');
  const bounds = (await viewport.boundingBox())!;
  await page.mouse.move(bounds.x + bounds.width * .45, bounds.y + bounds.height * .45);
  await page.mouse.down();
  await page.mouse.move(bounds.x + bounds.width * .57, bounds.y + bounds.height * .52, { steps: 8 });
  await page.mouse.up();
  await expect.poll(async () => (await pixelSummary(page)).checksum).not.toBe(before.checksum);
  await ready(page);
  const catalogResponse = await page.request.get('/api/v1/presets');
  expect(catalogResponse.ok()).toBe(true);
  const catalog = await catalogResponse.json() as { presets: AppearancePreset[] };
  expect(catalog.presets).toHaveLength(8);
  const checksums = new Set<number>();
  for (const preset of catalog.presets) {
    const previousSequence = await page.locator('.viewport-bottom-right .mono').innerText();
    await page.getByRole('combobox', { name: 'Transfer preset' }).selectOption(preset.id);
    await expect(page.locator('.viewport-bottom-right .mono')).not.toHaveText(previousSequence);
    await ready(page);
    await expect(page.getByRole('spinbutton', { name: 'Window width', exact: true })).toHaveValue(String(preset.windowWidth));
    await expect(page.getByRole('spinbutton', { name: 'Window level', exact: true })).toHaveValue(String(preset.windowLevel));
    await expect(page.getByRole('spinbutton', { name: 'Opacity scale', exact: true })).toHaveValue(String(preset.opacityScale));
    await expect(page.getByRole('checkbox', { name: 'Gradient lighting' })).toBeChecked();
    await expect(page.getByRole('img', { name: `${preset.name} color table` })).toBeVisible();
    checksums.add((await pixelSummary(page)).checksum);
    await expect(page.getByRole('alert')).toHaveCount(0);
  }
  expect(checksums.size).toBe(8);
  await page.getByRole('combobox', { name: 'Transfer preset' }).selectOption('ct-soft-tissue');
  await expect(page.locator('.viewport-bottom-left')).toContainText('ct-soft-tissue');
  await ready(page);
  await expect(page.getByRole('alert')).toHaveCount(0);
  await page.screenshot({ path: 'test-results-real/real-cuda-desktop.png' });

  const [download] = await Promise.all([
    page.waitForEvent('download'),
    page.getByRole('button', { name: 'Download final PNG' }).click()
  ]);
  expect(await download.failure()).toBeNull();
  const bytes = await readFile((await download.path())!);
  expect([...bytes.subarray(0, 8)]).toEqual([137, 80, 78, 71, 13, 10, 26, 10]);
  const exported = await pixelSummary(page);
  expect(bytes.readUInt32BE(16)).toBe(exported.width);
  expect(bytes.readUInt32BE(20)).toBe(exported.height);

  for (const width of [390, 320]) {
    const previousSequence = await page.locator('.viewport-bottom-right .mono').innerText();
    await page.setViewportSize({ width, height: 844 });
    await expect.poll(() => page.locator('canvas').evaluate((canvas: HTMLCanvasElement) => canvas.width)).toBeLessThanOrEqual(width * 2);
    await expect(page.locator('.viewport-bottom-right .mono')).not.toHaveText(previousSequence);
    await ready(page);
    await expect.poll(async () => {
      const pixels = await pixelSummary(page);
      return pixels.maximum - pixels.minimum;
    }).toBeGreaterThan(100);
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
    await page.getByRole('button', { name: 'Open settings' }).click();
    await expect(page.getByLabel('Series', { exact: true })).toBeVisible();
    await page.getByRole('combobox', { name: 'Transfer preset' }).selectOption('ct-lung');
    await page.keyboard.press('Escape');
    await ready(page);
  }
  await page.screenshot({ path: 'test-results-real/real-cuda-mobile.png' });
  expect(errors).toEqual([]);
  expect(external).toEqual([]);
  const seriesResponse = await page.request.get('/api/v1/series');
  expect(seriesResponse.ok()).toBe(true);
  await page.goto('about:blank');
});