import { expect } from '@playwright/test';
import type { Page } from '@playwright/test';
import { test } from './fixtures';
import type { FrameMeta, Session } from '../../src/protocol';
import { PRESET_STORAGE_KEY } from '../../src/transfer';

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

test('editable preset curves change GPU output, restore exactly, save locally and fit touch screens', async ({ page, request }) => {
  const frames = new Map<number, FrameMeta>();
  const errors: string[] = [];
  page.on('pageerror', error => errors.push(error.message));
  page.on('websocket', socket => socket.on('framereceived', event => {
    if (typeof event.payload === 'string') return;
    const length = event.payload.readUInt32LE(0);
    const meta = JSON.parse(event.payload.subarray(4, length + 4).toString('utf8')) as FrameMeta;
    frames.set(meta.seq, meta);
  }));
  const frame = async () => {
    const seq = Number((await page.locator('.viewport-bottom-right .mono').innerText()).slice(1));
    return frames.get(seq)!;
  };
  await page.setViewportSize({ width: 1440, height: 960 });
  await page.goto('/');
  await finalFrame(page);
  await changed(page, () => page.getByRole('combobox', { name: 'Render size', exact: true }).selectOption('50%'));
  await changed(page, () => page.getByRole('button', { name: 'Edit transfer function', exact: true }).click());
  const sessionId = (await frame()).sessionId;
  const response = await request.get(`/api/v1/sessions/${sessionId}`);
  const session = await response.json() as Session;
  await response.dispose();
  expect(session.transferFunctionSupported).toBe(true);
  expect(session.histogram!.counts.reduce((sum, count) => sum + count, 0)).toBe(session.histogram!.voxelCount);
  expect(session.histogram!.voxelCount).toBe(session.volume!.dimensions.reduce((product, size) => product * size, 1));
  expect(await page.getByTestId('histogram-bars').locator('rect').evaluateAll(bars => bars.some(bar => Number(bar.getAttribute('height')) > 0))).toBe(true);
  const presetSelect = page.getByRole('combobox', { name: 'Transfer preset', exact: true });
  const catalogResponse = await request.get('/api/v1/presets');
  const catalog = await catalogResponse.json() as { presets: { id: string; transferFunction: unknown[] }[] };
  await catalogResponse.dispose();
  for (const preset of catalog.presets) {
    await changed(page, () => presetSelect.selectOption(preset.id));
    await expect(page.locator('.curve-point')).toHaveCount(preset.transferFunction.length);
    expect((await frame()).transferFunction).toEqual(preset.transferFunction);
  }
  await changed(page, () => presetSelect.selectOption('ct-bone'));
  const original = await pixels(page);
  const initialCurve = (await frame()).transferFunction!;
  const point = page.getByRole('button', { name: 'Curve point 3', exact: true });
  await point.scrollIntoViewIfNeeded();
  const bounds = (await point.boundingBox())!;
  await changed(page, async () => {
    await page.mouse.move(bounds.x + bounds.width / 2, bounds.y + bounds.height / 2);
    await page.mouse.down();
    await page.mouse.move(bounds.x + 35, bounds.y - 20, { steps: 12 });
    await page.mouse.up();
  });
  expect((await frame()).transferFunction).not.toEqual(initialCurve);
  expect(await pixels(page)).not.toEqual(original);
  await changed(page, () => page.getByRole('spinbutton', { name: 'Point opacity', exact: true }).fill('10'));
  await changed(page, () => page.getByLabel('Point color', { exact: true }).fill('#00ff00'));
  const colored = (await frame()).transferFunction![2]!.rgba;
  expect(colored.slice(0, 3)).toEqual([0, 1, 0]);
  expect(colored[3]).toBeCloseTo(0.1, 6);
  await changed(page, () => page.getByRole('button', { name: 'Add curve point', exact: true }).click());
  await expect(page.locator('.curve-point')).toHaveCount(initialCurve.length + 1);
  await changed(page, () => page.getByRole('button', { name: 'Delete curve point', exact: true }).click());
  await expect(page.locator('.curve-point')).toHaveCount(initialCurve.length);
  await page.getByRole('button', { name: 'Curve point 3', exact: true }).focus();
  await changed(page, () => page.keyboard.press('ArrowUp'));
  await page.getByRole('button', { name: 'Save custom preset', exact: true }).click();
  await page.getByRole('textbox', { name: 'Custom preset name', exact: true }).fill('Green bone');
  await page.getByRole('button', { name: 'Confirm save preset', exact: true }).click();
  const localId = await presetSelect.inputValue();
  expect(localId).toMatch(/^local-/);
  const savedCurve = (await frame()).transferFunction;
  const stored = await page.evaluate(key => localStorage.getItem(key), PRESET_STORAGE_KEY);
  expect(stored).toContain('Green bone');
  expect(stored).not.toMatch(/histogram|sessionId|voxels|targetMm/);
  await page.getByRole('button', { name: 'Curve point 3', exact: true }).focus();
  await changed(page, () => page.keyboard.press('ArrowUp'));
  await changed(page, () => page.getByRole('button', { name: 'Reset preset', exact: true }).click());
  expect((await frame()).transferFunction).toEqual(savedCurve);
  await page.screenshot({ path: 'test-results-real/transfer-desktop.png' });
  await changed(page, () => page.getByRole('button', { name: 'Restore built-in curve', exact: true }).click());
  expect((await frame()).transferFunction).toEqual(initialCurve);
  expect(await pixels(page)).toEqual(original);
  await page.goto('about:blank');
  await request.delete(`/api/v1/sessions/${sessionId}`);
  await expect.poll(async () => {
    const status = await request.get(`/api/v1/sessions/${sessionId}`);
    try { return status.status() === 410 || status.status() === 404 || (await status.json() as Session).state === 'closed'; }
    finally { await status.dispose(); }
  }).toBe(true);
  await page.goto('/');
  await finalFrame(page);
  await changed(page, () => page.getByRole('button', { name: 'Edit transfer function', exact: true }).click());
  await changed(page, () => presetSelect.selectOption(localId));
  expect((await frame()).transferFunction).toEqual(savedCurve);
  const touch = await page.context().newCDPSession(page);
  await touch.send('Emulation.setTouchEmulationEnabled', { enabled: true });
  try {
    for (const width of [390, 320]) {
      await changed(page, () => page.setViewportSize({ width, height: 844 }));
      const target = page.getByRole('button', { name: 'Curve point 3', exact: true });
      await target.scrollIntoViewIfNeeded();
      const rect = (await target.boundingBox())!;
      const before = (await frame()).transferFunction;
      await changed(page, async () => {
        const start = { x: rect.x + rect.width / 2, y: rect.y + rect.height / 2, id: 1 };
        await touch.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [start] });
        await touch.send('Input.dispatchTouchEvent', { type: 'touchMove', touchPoints: [{ ...start, y: start.y - 15 }] });
        await touch.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
      });
      expect((await frame()).transferFunction).not.toEqual(before);
      expect(await page.getByTestId('transfer-editor').evaluate(editor => {
        const boundary = editor.getBoundingClientRect();
        return [...editor.querySelectorAll('input, button, select')].every(element => {
          const rect = element.getBoundingClientRect();
          return rect.left >= boundary.left && rect.right <= boundary.right;
        });
      })).toBe(true);
      await page.screenshot({ path: `test-results-real/transfer-mobile-${width}.png` });
    }
  } finally { await touch.detach(); }
  await page.setViewportSize({ width: 1440, height: 960 });
  await finalFrame(page);
  page.once('dialog', dialog => dialog.accept());
  await page.getByRole('button', { name: 'Delete custom preset', exact: true }).click();
  await expect(presetSelect.locator(`option[value="${localId}"]`)).toHaveCount(0);
  expect(errors).toEqual([]);
});