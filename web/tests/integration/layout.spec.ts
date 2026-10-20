import { expect } from '@playwright/test';
import type { Page } from '@playwright/test';
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

async function expectViewShortcuts(page: Page) {
  const shortcuts = page.getByTestId('viewport').getByRole('group', { name: 'Standard views', exact: true });
  await expect(shortcuts.getByRole('button')).toHaveText(['A', 'P', 'L', 'R', 'S', 'I']);
  expect(await shortcuts.evaluate(group => {
    const viewport = group.parentElement!;
    const boundary = viewport.getBoundingClientRect();
    const bar = group.getBoundingClientRect();
    const labels = [...viewport.querySelectorAll('.viewport-top-left, .viewport-top-right, .viewport-bottom-left, .viewport-bottom-right, .orientation-overlay > span')];
    return bar.left >= boundary.left && bar.right <= boundary.right && bar.top >= boundary.top && bar.bottom <= boundary.bottom &&
      labels.every(label => {
        const rect = label.getBoundingClientRect();
        return bar.right <= rect.left || bar.left >= rect.right || bar.bottom <= rect.top || bar.top >= rect.bottom;
      });
  })).toBe(true);
}

test('icon tabs and curve dock preserve scene state and keep the viewport usable on desktop and mobile', async ({ page }) => {
  const frames = new Map<number, FrameMeta>();
  const errors: string[] = [];
  page.on('pageerror', error => errors.push(error.message));
  page.on('websocket', socket => socket.on('framereceived', event => {
    if (typeof event.payload === 'string') return;
    const size = event.payload.readUInt32LE(0);
    const frame = JSON.parse(event.payload.subarray(4, size + 4).toString('utf8')) as FrameMeta;
    frames.set(frame.seq, frame);
  }));
  const frame = async () => {
    const seq = Number((await page.locator('.viewport-bottom-right .mono').innerText()).slice(1));
    return frames.get(seq)!;
  };
  const imagePixels = () => page.getByLabel('Server-rendered volume image', { exact: true }).evaluate((canvas: HTMLCanvasElement) => {
    const pixels = canvas.getContext('2d')!.getImageData(0, 0, canvas.width, canvas.height).data;
    let visible = 0;
    for (let index = 0; index < pixels.length; index += 4) if (pixels[index]! + pixels[index + 1]! + pixels[index + 2]! > 30) visible++;
    return visible;
  });
  await page.setViewportSize({ width: 1440, height: 960 });
  await page.goto('/');
  await finalFrame(page);
  await expectViewShortcuts(page);
  await changed(page, () => page.getByRole('combobox', { name: 'Render size' }).selectOption('50%'));
  const tabs = page.getByRole('tablist', { name: 'Settings categories' });
  await expect(tabs.getByRole('tab')).toHaveCount(3);
  for (const name of ['Display', 'Crop', 'Info']) await expect(tabs.getByRole('tab', { name, exact: true }).locator('svg')).toHaveCount(1);
  await expect(page.getByTestId('frame-timings')).toBeHidden();
  await expect(page.locator('#transfer-dock')).toBeHidden();
  const widthControl = (await page.getByRole('spinbutton', { name: 'Window width', exact: true }).boundingBox())!;
  expect(widthControl.y).toBeLessThan(600);
  await changed(page, () => page.getByRole('button', { name: 'Left view', exact: true }).click());
  await tabs.getByRole('tab', { name: 'Crop', exact: true }).click();
  await expect(page.getByText('Precision settings', { exact: true })).toBeHidden();
  await expect(page.getByRole('spinbutton', { name: 'Columns (X) from', exact: true })).toHaveCount(0);
  await changed(page, () => page.getByRole('checkbox', { name: 'Enable clipping plane', exact: true }).check());
  await expect(page.getByRole('spinbutton', { name: 'Plane position', exact: true })).toHaveCount(0);
  await page.getByText('Precision settings', { exact: true }).click();
  await changed(page, () => page.getByRole('spinbutton', { name: 'Tilt (LPS)', exact: true }).fill('55'));
  await changed(page, () => page.getByRole('checkbox', { name: 'Enable crop', exact: true }).check());
  await changed(page, () => page.getByRole('spinbutton', { name: 'Columns (X) from', exact: true }).fill('20'));
  await tabs.getByRole('tab', { name: 'Info', exact: true }).click();
  await expect(page.getByTestId('frame-timings')).toBeVisible();
  await expect(tabs.getByRole('img', { name: 'Clipping active', exact: true })).toBeVisible();
  await tabs.getByRole('tab', { name: 'Display', exact: true }).click();
  await changed(page, () => page.getByRole('spinbutton', { name: 'Window level', exact: true }).fill('450'));
  const beforeDock = await frame();
  const beforeBounds = (await page.getByTestId('viewport').boundingBox())!;
  await changed(page, () => page.getByRole('button', { name: 'Edit transfer function', exact: true }).click());
  const dock = page.getByRole('region', { name: 'Transfer function editor', exact: true });
  await expect(dock).toBeVisible();
  const openBounds = (await page.getByTestId('viewport').boundingBox())!;
  const dockBounds = (await dock.boundingBox())!;
  expect(openBounds.height).toBeGreaterThan(250);
  expect(openBounds.height).toBeLessThan(beforeBounds.height);
  expect(openBounds.y + openBounds.height).toBeLessThanOrEqual(dockBounds.y + 1);
  await expectViewShortcuts(page);
  expect((await frame()).camera).toEqual(beforeDock.camera);
  expect((await frame()).cropBox).toEqual(beforeDock.cropBox);
  expect((await frame()).clipPlane).toEqual(beforeDock.clipPlane);
  const chart = (await page.getByTestId('curve-chart').boundingBox())!;
  expect(chart.width).toBeGreaterThan(500);
  await page.getByRole('button', { name: 'Curve point 3', exact: true }).click();
  await changed(page, () => page.getByRole('spinbutton', { name: 'Point opacity', exact: true }).fill('8'));
  await page.getByRole('spinbutton', { name: 'Point opacity', exact: true }).press('Escape');
  await expect(dock).toBeVisible();
  await page.getByRole('combobox', { name: 'Opacity axis range', exact: true }).press('Escape');
  await expect(dock).toBeVisible();
  const edited = (await frame()).transferFunction;
  await tabs.getByRole('tab', { name: 'Crop', exact: true }).click();
  await expect(page.getByRole('spinbutton', { name: 'Tilt (LPS)', exact: true })).toHaveValue('55');
  await expect(page.getByRole('spinbutton', { name: 'Columns (X) from', exact: true })).toHaveValue('20');
  await expect(dock).toBeVisible();
  await changed(page, () => page.getByRole('button', { name: 'Close transfer editor', exact: true }).click());
  await expect(dock).toBeHidden();
  await expect(page.getByTestId('viewport')).toBeFocused();
  expect((await frame()).transferFunction).toEqual(edited);
  expect((await page.getByTestId('viewport').boundingBox())!.height).toBeCloseTo(beforeBounds.height, 0);
  await page.screenshot({ path: 'test-results-real/layout-crop-desktop.png' });
  await tabs.getByRole('tab', { name: 'Display', exact: true }).click();
  await changed(page, () => page.getByRole('button', { name: 'Edit transfer function', exact: true }).click());
  expect((await frame()).transferFunction).toEqual(edited);
  expect(await imagePixels()).toBeGreaterThan(300);
  await page.screenshot({ path: 'test-results-real/layout-display-desktop.png' });
  await changed(page, () => page.keyboard.press('Escape'));
  await expect(page.getByRole('button', { name: 'Edit transfer function', exact: true })).toBeFocused();
  for (const width of [390, 320]) {
    await changed(page, () => page.setViewportSize({ width, height: 844 }));
    await expectViewShortcuts(page);
    await page.getByRole('button', { name: 'Open settings', exact: true }).click();
    await page.getByRole('complementary', { name: 'Volume settings', exact: true }).getByRole('button', { name: 'Close settings', exact: true }).click();
    await expect(page.getByRole('button', { name: 'Open settings', exact: true })).toBeFocused();
    await page.getByRole('button', { name: 'Open settings', exact: true }).click();
    await tabs.getByRole('tab', { name: 'Info', exact: true }).click();
    await page.screenshot({ path: `test-results-real/layout-info-mobile-${width}.png` });
    await tabs.getByRole('tab', { name: 'Display', exact: true }).click();
    await changed(page, () => page.getByRole('button', { name: 'Edit transfer function', exact: true }).click());
    await expect(page.getByRole('button', { name: 'Open settings', exact: true })).toBeVisible();
    await expect(dock).toBeVisible();
    const viewport = (await page.getByTestId('viewport').boundingBox())!;
    const bottom = (await dock.boundingBox())!;
    expect(viewport.height).toBeGreaterThan(200);
    expect(viewport.y + viewport.height).toBeLessThanOrEqual(bottom.y + 1);
    await expectViewShortcuts(page);
    expect(bottom.x).toBeGreaterThanOrEqual(0);
    expect(bottom.x + bottom.width).toBeLessThanOrEqual(width);
    expect((await frame()).transferFunction).toEqual(edited);
    expect((await frame()).clipPlane).toEqual(beforeDock.clipPlane);
    expect(await imagePixels()).toBeGreaterThan(200);
    await page.screenshot({ path: `test-results-real/layout-dock-mobile-${width}.png` });
    await changed(page, () => page.getByRole('button', { name: 'Close transfer editor', exact: true }).click());
    await expect(page.getByRole('button', { name: 'Open settings', exact: true })).toBeFocused();
  }
  expect(errors).toEqual([]);
});