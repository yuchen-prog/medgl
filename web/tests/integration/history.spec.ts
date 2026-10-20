import { expect } from '@playwright/test';
import type { Page } from '@playwright/test';
import { test } from './fixtures';
import type { RenderRequest } from '../../src/protocol';

async function finalFrame(page: Page) {
  await expect(page.getByRole('button', { name: 'Download final PNG', exact: true })).toBeEnabled({ timeout: 30000 });
  await expect(page.locator('.render-indicator')).toHaveCount(0);
}

async function changed(page: Page, action: () => Promise<unknown>) {
  const sequence = await page.locator('.viewport-bottom-right .mono').innerText();
  await action();
  await expect(page.locator('.viewport-bottom-right .mono')).not.toHaveText(sequence);
  await finalFrame(page);
}

test('history restores GPU pixels and absolute scene state across camera, presets, controls and clipping', async ({ page }) => {
  const requests = new Map<number, RenderRequest>();
  const errors: string[] = [];
  page.on('pageerror', error => errors.push(error.message));
  page.on('websocket', socket => socket.on('framesent', event => {
    const request = JSON.parse(event.payload.toString()) as RenderRequest;
    requests.set(request.seq, request);
  }));
  const snapshot = async () => {
    const sequence = Number((await page.locator('.viewport-bottom-right .mono').innerText()).slice(1));
    const { camera, style, cropBox, clipPlane } = requests.get(sequence)!;
    const pixels = await page.getByLabel('Server-rendered volume image', { exact: true }).evaluate(async (canvas: HTMLCanvasElement) => {
      const data = canvas.getContext('2d')!.getImageData(0, 0, canvas.width, canvas.height).data;
      const hash = await crypto.subtle.digest('SHA-256', data);
      return [...new Uint8Array(hash)].map(value => value.toString(16).padStart(2, '0')).join('');
    });
    return { camera, style, cropBox, clipPlane, pixels };
  };
  const verifyStep = async (action: () => Promise<unknown>) => {
    const before = await snapshot();
    await changed(page, action);
    const after = await snapshot();
    expect(after).not.toEqual(before);
    await changed(page, () => page.getByRole('button', { name: 'Undo', exact: true }).click());
    expect(await snapshot()).toEqual(before);
    await changed(page, () => page.getByRole('button', { name: 'Redo', exact: true }).click());
    expect(await snapshot()).toEqual(after);
  };
  await page.setViewportSize({ width: 1440, height: 960 });
  await page.goto('/');
  await finalFrame(page);
  await expect(page.getByRole('button', { name: 'Undo', exact: true })).toBeDisabled();
  await changed(page, () => page.getByRole('combobox', { name: 'Render size', exact: true }).selectOption('50%'));
  await expect(page.getByRole('button', { name: 'Undo', exact: true })).toBeDisabled();
  await verifyStep(async () => {
    const bounds = (await page.getByTestId('viewport').boundingBox())!;
    await page.mouse.move(bounds.x + bounds.width * 0.4, bounds.y + bounds.height * 0.4);
    await page.mouse.down();
    await page.mouse.move(bounds.x + bounds.width * 0.6, bounds.y + bounds.height * 0.55, { steps: 12 });
    await page.mouse.up();
  });
  await verifyStep(() => page.getByRole('button', { name: 'Left view', exact: true }).click());
  await verifyStep(() => page.getByRole('combobox', { name: 'Transfer preset', exact: true }).selectOption('ct-soft-tissue'));
  await verifyStep(async () => {
    const width = page.getByRole('spinbutton', { name: 'Window width', exact: true });
    await width.fill('800');
    await width.fill('1250');
    await width.press('Enter');
  });
  await page.getByText('Lighting and background', { exact: true }).click();
  await verifyStep(() => page.getByRole('button', { name: 'White background', exact: true }).click());
  await verifyStep(() => page.getByRole('checkbox', { name: 'Gradient lighting', exact: true }).uncheck());
  await page.getByRole('tab', { name: 'Crop', exact: true }).click();
  await verifyStep(() => page.getByRole('checkbox', { name: 'Enable crop', exact: true }).check());
  await verifyStep(async () => {
    const minimum = page.getByRole('spinbutton', { name: 'Columns (X) from', exact: true });
    await minimum.fill('10');
    await minimum.fill('30');
    await minimum.blur();
  });
  await verifyStep(() => page.getByRole('checkbox', { name: 'Enable clipping plane', exact: true }).check());
  await page.getByText('Precision settings', { exact: true }).click();
  await verifyStep(async () => {
    const tilt = page.getByRole('spinbutton', { name: 'Tilt (LPS)', exact: true });
    await tilt.fill('55');
    await tilt.press('Enter');
  });
  await verifyStep(() => page.getByRole('button', { name: 'Flip retained side', exact: true }).click());
  const beforeTheme = await snapshot();
  await page.getByRole('switch', { name: 'Dark theme', exact: true }).check();
  expect(await snapshot()).toEqual(beforeTheme);
  for (const width of [390, 320]) {
    await changed(page, () => page.setViewportSize({ width, height: 844 }));
    await changed(page, () => page.getByRole('button', { name: 'Undo', exact: true }).click());
    await changed(page, () => page.getByRole('button', { name: 'Redo', exact: true }).click());
    await page.screenshot({ path: `test-results-real/history-mobile-${width}.png`, animations: 'disabled' });
  }
  expect(errors).toEqual([]);
});