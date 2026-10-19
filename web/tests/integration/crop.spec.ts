import { expect } from '@playwright/test';
import type { Page } from '@playwright/test';
import { readFile } from 'node:fs/promises';
import { test } from './fixtures';
import type { FrameMeta } from '../../src/protocol';

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
  return page.locator('canvas').evaluate((canvas: HTMLCanvasElement) =>
    [...canvas.getContext('2d')!.getImageData(0, 0, canvas.width, canvas.height).data]);
}

test('GPU crop controls preserve state, restore exact image, export and fit narrow screens', async ({ page }) => {
  const frames = new Map<number, FrameMeta>();
  page.on('websocket', socket => socket.on('framereceived', event => {
    if (typeof event.payload === 'string') return;
    const length = event.payload.readUInt32LE(0);
    const frame = JSON.parse(event.payload.subarray(4, 4 + length).toString('utf8')) as FrameMeta;
    frames.set(frame.seq, frame);
  }));
  await page.setViewportSize({ width: 1440, height: 960 });
  await page.goto('/');
  await finalFrame(page);
  await page.getByRole('combobox', { name: 'Series', exact: true }).selectOption('series-1');
  await page.getByRole('button', { name: 'Load series', exact: true }).click();
  await page.getByRole('tab', { name: 'Info', exact: true }).click();
  await expect(page.locator('.geometry')).toContainText('64 × 64 × 64');
  await finalFrame(page);
  await changed(page, () => page.getByRole('combobox', { name: 'Render size' }).selectOption('50%'));
  const original = await pixels(page);
  await page.getByRole('tab', { name: 'Crop', exact: true }).click();
  const enabled = page.getByRole('checkbox', { name: 'Enable crop', exact: true });
  const from = page.getByRole('spinbutton', { name: 'Columns (X) from', exact: true });
  await expect(page.getByTestId('crop-controls').getByLabel('Columns (X) from', { exact: true })).toBeDisabled();
  await changed(page, () => enabled.check());
  await expect(page.getByTestId('crop-status')).toBeVisible();
  expect(await pixels(page)).toEqual(original);
  await changed(page, () => from.fill('50'));
  expect(await pixels(page)).not.toEqual(original);
  const seq = Number((await page.locator('.viewport-bottom-right .mono').innerText()).slice(1));
  expect(frames.get(seq)?.cropBox).toEqual({ enabled: true, minimum: [0.5, 0, 0], maximum: [1, 1, 1] });
  await page.screenshot({ path: 'test-results-real/crop-desktop.png' });
  const [download] = await Promise.all([page.waitForEvent('download'),
    page.getByRole('button', { name: 'Download final PNG', exact: true }).click()]);
  expect(await download.failure()).toBeNull();
  const bytes = await readFile((await download.path())!);
  expect([...bytes.subarray(0, 8)]).toEqual([137, 80, 78, 71, 13, 10, 26, 10]);
  const canvas = await page.locator('canvas').evaluate((element: HTMLCanvasElement) => [element.width, element.height]);
  expect([bytes.readUInt32BE(16), bytes.readUInt32BE(20)]).toEqual(canvas);
  await changed(page, () => enabled.uncheck());
  await expect(page.getByTestId('crop-status')).toHaveCount(0);
  expect(await pixels(page)).toEqual(original);
  await changed(page, () => enabled.check());
  await page.getByRole('tab', { name: 'Display', exact: true }).click();
  await changed(page, () => page.getByRole('combobox', { name: 'Transfer preset', exact: true }).selectOption('ct-soft-tissue'));
  await changed(page, () => page.getByRole('button', { name: 'Anterior view', exact: true }).click());
  await page.getByRole('tab', { name: 'Crop', exact: true }).click();
  await expect(from).toHaveValue('50');
  await changed(page, () => page.getByRole('button', { name: 'Reset crop', exact: true }).click());
  await expect(enabled).not.toBeChecked();
  await expect(page.getByTestId('crop-controls').getByLabel('Columns (X) from', { exact: true })).toHaveValue('0');
  for (const width of [390, 320]) {
    await changed(page, () => page.setViewportSize({ width, height: 844 }));
    await page.getByRole('button', { name: 'Open settings' }).click();
    await enabled.check();
    await from.fill('99');
    await expect(page.getByRole('spinbutton', { name: 'Columns (X) to', exact: true })).toHaveAttribute('min', '100');
    expect(await page.getByTestId('crop-controls').evaluate(panel => {
      const boundary = panel.getBoundingClientRect();
      return [...panel.querySelectorAll('input')].every(input => {
        const rect = input.getBoundingClientRect();
        return rect.left >= boundary.left && rect.right <= boundary.right;
      });
    })).toBe(true);
    await page.getByRole('button', { name: 'Reset crop', exact: true }).click();
    await page.screenshot({ path: `test-results-real/crop-mobile-${width}.png` });
    await page.keyboard.press('Escape');
    await finalFrame(page);
  }
  await page.goto('about:blank');
});