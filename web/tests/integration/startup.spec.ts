import { expect } from '@playwright/test';
import { test } from './fixtures';
import type { Session } from '../../src/protocol';

test('an injected CT load failure closes its real session before a synthetic GPU scene starts', async ({ page, request }) => {
  const inputs: string[] = [];
  const errors: string[] = [];
  let failedSession: string | null = null;
  let injected = false;
  let releasedBeforeFallback = false;
  page.on('pageerror', error => errors.push(error.message));
  await page.route('**/api/v1/sessions', async route => {
    if (route.request().method() !== 'POST') return route.continue();
    const seriesId = (route.request().postDataJSON() as { seriesId: string }).seriesId;
    inputs.push(seriesId);
    if (seriesId === 'phantom' && failedSession) {
      const previous = await request.get(`/api/v1/sessions/${failedSession}`);
      try {
        releasedBeforeFallback = [404, 410].includes(previous.status()) ||
          (await previous.json() as Session).state === 'closed';
      } finally { await previous.dispose(); }
    }
    const response = await route.fetch();
    if (seriesId === 'series-1') failedSession = (await response.json() as Session).sessionId;
    await route.fulfill({ response });
  });
  await page.route('**/api/v1/sessions/*', async route => {
    const sessionId = new URL(route.request().url()).pathname.split('/').at(-1);
    if (route.request().method() === 'GET' && sessionId === failedSession && !injected) {
      injected = true;
      return route.fulfill({ json: { sessionId, state: 'failed' } });
    }
    return route.continue();
  });
  await page.setViewportSize({ width: 1440, height: 960 });
  await page.goto('/');
  await expect(page.getByRole('button', { name: 'Download final PNG', exact: true })).toBeEnabled({ timeout: 30000 });
  await expect(page.locator('.render-indicator')).toHaveCount(0);
  expect(inputs).toEqual(['series-1', 'phantom']);
  expect(injected).toBe(true);
  expect(releasedBeforeFallback).toBe(true);
  await expect(page.getByRole('combobox', { name: 'Series', exact: true })).toHaveValue('phantom');
  await expect(page.locator('.viewport-series')).toHaveText('Synthetic phantom');
  await expect(page.getByTestId('load-warning')).toContainText('Session failed');
  await expect(page.getByTestId('load-warning')).toContainText('Showing Synthetic phantom');
  const visiblePixels = await page.getByLabel('Server-rendered volume image', { exact: true }).evaluate((canvas: HTMLCanvasElement) => {
    const pixels = canvas.getContext('2d')!.getImageData(0, 0, canvas.width, canvas.height).data;
    let visible = 0;
    for (let index = 0; index < pixels.length; index += 4)
      if (pixels[index]! + pixels[index + 1]! + pixels[index + 2]! > 30) visible++;
    return visible;
  });
  expect(visiblePixels).toBeGreaterThan(1000);
  await page.screenshot({ path: 'test-results-real/startup-ct-fallback.png' });
  expect(errors).toEqual([]);
});