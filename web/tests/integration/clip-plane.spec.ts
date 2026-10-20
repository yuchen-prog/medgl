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
  return page.getByLabel('Server-rendered volume image', { exact: true }).evaluate((canvas: HTMLCanvasElement) =>
    [...canvas.getContext('2d')!.getImageData(0, 0, canvas.width, canvas.height).data]);
}

async function glassPixels(page: Page) {
  return page.getByLabel('Glass clipping plane', { exact: true }).evaluate((canvas: HTMLCanvasElement) => {
    const context = canvas.getContext('webgl2')!;
    const buffer = new Uint8Array(canvas.width * canvas.height * 4);
    context.readPixels(0, 0, canvas.width, canvas.height, context.RGBA, context.UNSIGNED_BYTE, buffer);
    let visible = 0;
    let transparent = 0;
    for (let offset = 0; offset < buffer.length; offset += 4) {
      if (buffer[offset + 3]! > 0) visible++;
      if (buffer[offset + 3]! === 0) transparent++;
    }
    return { visible, transparent, width: canvas.width, height: canvas.height };
  });
}

async function handlePoint(page: Page, color: 'red' | 'blue') {
  const canvas = page.getByLabel('Glass clipping plane', { exact: true });
  const point = await canvas.evaluate((element: HTMLCanvasElement, targetColor) => {
    const context = element.getContext('webgl2')!;
    const buffer = new Uint8Array(element.width * element.height * 4);
    context.readPixels(0, 0, element.width, element.height, context.RGBA, context.UNSIGNED_BYTE, buffer);
    const bounds = element.getBoundingClientRect();
    for (let row = element.height - 1; row >= 0; row--) for (let column = 0; column < element.width; column++) {
      const offset = (row * element.width + column) * 4;
      const red = buffer[offset]!;
      const green = buffer[offset + 1]!;
      const blue = buffer[offset + 2]!;
      const match = targetColor === 'red' ? red > 170 && green < 140 && blue < 140 : blue > 170 && red < 140 && green < 140;
      if (match && buffer[offset + 3]! > 200) return {
        x: bounds.left + (column + 0.5) * bounds.width / element.width,
        y: bounds.top + (element.height - row - 0.5) * bounds.height / element.height
      };
    }
    return null;
  }, color);
  expect(point, `${color} transform handle must be visible`).not.toBeNull();
  return point!;
}

async function dragHandle(page: Page, color: 'red' | 'blue') {
  const point = await handlePoint(page, color);
  await page.mouse.move(point!.x, point!.y);
  await page.mouse.down();
  await expect(page.getByTestId('glass-overlay')).toHaveAttribute('data-drag-action', color === 'red' ? 'rotate' : 'translate');
  await page.mouse.move(point!.x + 75, point!.y + 40, { steps: 12 });
  await page.mouse.up();
}

test('glass plane rotates, advances, clips on GPU, restores, exports without tools and fits mobile', async ({ page }) => {
  const frames = new Map<number, FrameMeta>();
  const errors: string[] = [];
  page.on('pageerror', error => errors.push(error.message));
  page.on('websocket', socket => socket.on('framereceived', event => {
    if (typeof event.payload === 'string') return;
    const length = event.payload.readUInt32LE(0);
    const frame = JSON.parse(event.payload.subarray(4, 4 + length).toString('utf8')) as FrameMeta;
    frames.set(frame.seq, frame);
  }));
  const displayedFrame = async () => {
    const seq = Number((await page.locator('.viewport-bottom-right .mono').innerText()).slice(1));
    return frames.get(seq)!;
  };
  await page.setViewportSize({ width: 1440, height: 960 });
  await page.goto('/');
  await finalFrame(page);
  await expect(page.getByRole('button', { name: 'Anterior view', exact: true })).toHaveAttribute('aria-pressed', 'true');
  await changed(page, () => page.getByRole('button', { name: 'Inferior view', exact: true }).click());
  await changed(page, () => page.getByRole('combobox', { name: 'Render size' }).selectOption('50%'));
  const original = await pixels(page);
  await page.getByRole('tab', { name: 'Crop', exact: true }).click();
  const enable = page.getByRole('checkbox', { name: 'Enable clipping plane', exact: true });
  await expect(enable).toBeEnabled();
  await changed(page, () => enable.check());
  const overlay = page.getByTestId('glass-overlay');
  await expect(overlay).toHaveAttribute('data-editing', 'true');
  expect((await glassPixels(page)).visible).toBeGreaterThan(1000);
  expect(await pixels(page)).not.toEqual(original);
  const cancelPoint = await handlePoint(page, 'red');
  await page.mouse.move(cancelPoint.x, cancelPoint.y);
  await page.mouse.down();
  await enable.focus();
  await page.keyboard.press('Space');
  await page.mouse.up();
  await finalFrame(page);
  await expect(overlay).toHaveCount(0);
  expect(errors).toEqual([]);
  await changed(page, () => enable.check());
  await page.getByText('Precision settings', { exact: true }).click();
  const beforeDrag = (await displayedFrame()).clipPlane!.normal;
  await changed(page, () => dragHandle(page, 'red'));
  expect((await displayedFrame()).clipPlane!.normal).not.toEqual(beforeDrag);
  const camera = (await displayedFrame()).camera;
  await changed(page, () => page.getByRole('spinbutton', { name: 'Tilt (LPS)', exact: true }).fill('55'));
  await changed(page, () => page.getByRole('spinbutton', { name: 'Azimuth (LPS)', exact: true }).fill('25'));
  const beforeMove = (await displayedFrame()).clipPlane!.offsetMm;
  await changed(page, () => dragHandle(page, 'blue'));
  expect((await displayedFrame()).clipPlane!.offsetMm).not.toBeCloseTo(beforeMove, 2);
  expect((await displayedFrame()).camera).toEqual(camera);
  await changed(page, () => page.getByRole('spinbutton', { name: 'Plane position', exact: true }).fill('10'));
  const oblique = await pixels(page);
  await changed(page, () => page.getByRole('button', { name: 'Flip retained side', exact: true }).click());
  expect(await pixels(page)).not.toEqual(oblique);
  expect((await displayedFrame()).clipPlane!.keepPositive).toBe(false);
  expect(JSON.parse((await overlay.getAttribute('data-plane'))!)).toEqual((await displayedFrame()).clipPlane);
  await expect(overlay).toHaveAttribute('data-seq', String((await displayedFrame()).seq));
  await page.getByRole('button', { name: 'Rotate volume', exact: true }).click();
  await expect(overlay).toHaveAttribute('data-editing', 'true');
  await page.screenshot({ path: 'test-results-real/clip-plane-desktop.png' });
  const [download] = await Promise.all([page.waitForEvent('download'),
    page.getByRole('button', { name: 'Download final PNG', exact: true }).click()]);
  const bytes = await readFile((await download.path())!);
  const mismatches = await page.getByLabel('Server-rendered volume image', { exact: true }).evaluate(async (canvas: HTMLCanvasElement, png) => {
    const bitmap = await createImageBitmap(new Blob([new Uint8Array(png)], { type: 'image/png' }));
    const exported = new OffscreenCanvas(bitmap.width, bitmap.height);
    const context = exported.getContext('2d')!;
    context.drawImage(bitmap, 0, 0);
    bitmap.close();
    const saved = context.getImageData(0, 0, exported.width, exported.height).data;
    const shown = canvas.getContext('2d')!.getImageData(0, 0, canvas.width, canvas.height).data;
    return saved.length === shown.length ? saved.reduce((count, value, index) => count + Number(value !== shown[index]), 0) : -1;
  }, [...bytes]);
  expect(mismatches).toBe(0);
  await changed(page, () => enable.uncheck());
  expect(await pixels(page)).toEqual(original);
  await expect(overlay).toHaveCount(0);
  await changed(page, () => enable.check());
  await changed(page, () => page.getByRole('checkbox', { name: 'Enable crop', exact: true }).check());
  await changed(page, () => page.getByRole('spinbutton', { name: 'Columns (X) from', exact: true }).fill('50'));
  expect((await displayedFrame()).cropBox?.enabled).toBe(true);
  expect((await displayedFrame()).clipPlane?.enabled).toBe(true);
  await changed(page, () => page.getByRole('button', { name: 'Reset crop', exact: true }).click());
  await changed(page, () => page.getByRole('button', { name: 'Reset clipping plane', exact: true }).click());
  expect(await pixels(page)).toEqual(original);
  for (const width of [390, 320]) {
    await changed(page, () => page.setViewportSize({ width, height: 844 }));
    await page.getByRole('button', { name: 'Open settings', exact: true }).click();
    await changed(page, () => enable.check());
    await changed(page, () => page.getByRole('spinbutton', { name: 'Tilt (LPS)', exact: true }).fill('55'));
    expect(await page.getByTestId('plane-controls').evaluate(panel => {
      const bounds = panel.getBoundingClientRect();
      return [...panel.querySelectorAll('input, button')].every(element => {
        const rect = element.getBoundingClientRect();
        return rect.left >= bounds.left && rect.right <= bounds.right;
      });
    })).toBe(true);
    await page.keyboard.press('Escape');
    await finalFrame(page);
    expect((await glassPixels(page)).visible).toBeGreaterThan(500);
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
    await page.screenshot({ path: `test-results-real/clip-plane-mobile-${width}.png` });
    await page.getByRole('button', { name: 'Open settings', exact: true }).click();
    await changed(page, () => page.getByRole('button', { name: 'Reset clipping plane', exact: true }).click());
    await page.keyboard.press('Escape');
  }
  expect(errors).toEqual([]);
});

test('glass handles and empty-space camera gestures keep independent ownership', async ({ page }) => {
  const frames = new Map<number, FrameMeta>();
  const errors: string[] = [];
  page.on('pageerror', error => errors.push(error.message));
  page.on('websocket', socket => socket.on('framereceived', event => {
    if (typeof event.payload === 'string') return;
    const length = event.payload.readUInt32LE(0);
    const frame = JSON.parse(event.payload.subarray(4, 4 + length).toString('utf8')) as FrameMeta;
    frames.set(frame.seq, frame);
  }));
  const displayedFrame = async () => {
    const seq = Number((await page.locator('.viewport-bottom-right .mono').innerText()).slice(1));
    return frames.get(seq)!;
  };
  const blankDrag = async (button: 'left' | 'right' = 'left') => {
    const bounds = (await page.getByTestId('viewport').boundingBox())!;
    const start = { x: bounds.x + 35, y: bounds.y + bounds.height * 0.7 };
    await page.mouse.move(start.x, start.y);
    await page.mouse.down({ button });
    await page.mouse.move(start.x + 95, start.y - 50, { steps: 12 });
    await page.mouse.up({ button });
  };
  await page.setViewportSize({ width: 1440, height: 960 });
  await page.goto('/');
  await finalFrame(page);
  await expect(page.getByRole('button', { name: 'Anterior view', exact: true })).toHaveAttribute('aria-pressed', 'true');
  await changed(page, () => page.getByRole('button', { name: 'Inferior view', exact: true }).click());
  await changed(page, () => page.getByRole('combobox', { name: 'Render size' }).selectOption('50%'));
  await page.getByRole('tab', { name: 'Crop', exact: true }).click();
  await changed(page, () => page.getByRole('checkbox', { name: 'Enable clipping plane', exact: true }).check());
  await page.getByText('Precision settings', { exact: true }).click();
  const overlay = page.getByTestId('glass-overlay');
  await expect(page.getByRole('button', { name: 'Rotate clipping plane', exact: true })).toHaveCount(0);
  await expect(page.getByRole('button', { name: 'Move clipping plane', exact: true })).toHaveCount(0);
  const initial = await displayedFrame();
  await changed(page, () => blankDrag());
  const rotated = await displayedFrame();
  expect(rotated.camera!.rotationXyzw).not.toEqual(initial.camera!.rotationXyzw);
  expect(rotated.clipPlane).toEqual(initial.clipPlane);
  await expect(overlay).toHaveAttribute('data-editing', 'true');
  await changed(page, () => page.getByRole('button', { name: 'Inferior view', exact: true }).click());
  const beforeCrossing = await displayedFrame();
  await changed(page, async () => {
    const point = await handlePoint(page, 'red');
    const bounds = (await page.getByTestId('viewport').boundingBox())!;
    await page.mouse.move(bounds.x + 35, bounds.y + bounds.height * 0.7);
    await page.mouse.down();
    await page.mouse.move(point.x, point.y, { steps: 12 });
    await page.mouse.move(point.x + 50, point.y + 30, { steps: 6 });
    await page.mouse.up();
  });
  const crossed = await displayedFrame();
  expect(crossed.camera!.rotationXyzw).not.toEqual(beforeCrossing.camera!.rotationXyzw);
  expect(crossed.clipPlane).toEqual(beforeCrossing.clipPlane);
  await changed(page, () => page.getByRole('button', { name: 'Inferior view', exact: true }).click());
  const beforeHandle = await displayedFrame();
  await changed(page, () => dragHandle(page, 'red'));
  const afterHandle = await displayedFrame();
  expect(afterHandle.camera).toEqual(beforeHandle.camera);
  expect(afterHandle.clipPlane!.normal).not.toEqual(beforeHandle.clipPlane!.normal);
  await changed(page, () => page.getByRole('spinbutton', { name: 'Tilt (LPS)', exact: true }).fill('55'));
  await changed(page, () => page.getByRole('spinbutton', { name: 'Azimuth (LPS)', exact: true }).fill('25'));
  const beforeMove = await displayedFrame();
  await changed(page, () => blankDrag());
  const rotatedAgain = await displayedFrame();
  expect(rotatedAgain.camera!.rotationXyzw).not.toEqual(beforeMove.camera!.rotationXyzw);
  expect(rotatedAgain.clipPlane).toEqual(beforeMove.clipPlane);
  await changed(page, () => page.getByRole('button', { name: 'Inferior view', exact: true }).click());
  const beforeAdvance = await displayedFrame();
  await changed(page, async () => {
    const point = await handlePoint(page, 'blue');
    await page.mouse.move(point.x, point.y);
    await page.mouse.down();
    await expect(overlay).toHaveAttribute('data-dragging', 'true');
    await expect(overlay).toHaveAttribute('data-drag-action', 'translate');
    await page.mouse.wheel(0, -120);
    await page.mouse.move(point.x + 75, point.y + 40, { steps: 12 });
    await page.mouse.up();
  });
  await expect(overlay).toHaveAttribute('data-dragging', 'false');
  let advanced = await displayedFrame();
  expect(advanced.camera).toEqual(beforeAdvance.camera);
  expect(advanced.clipPlane!.offsetMm).not.toBeCloseTo(beforeAdvance.clipPlane!.offsetMm, 2);
  advanced.clipPlane!.normal.forEach((value, axis) => expect(value).toBeCloseTo(beforeAdvance.clipPlane!.normal[axis]!, 6));
  await page.getByRole('button', { name: 'Pan volume', exact: true }).click();
  const beforeSecondRotation = advanced;
  await changed(page, () => dragHandle(page, 'red'));
  advanced = await displayedFrame();
  expect(advanced.camera).toEqual(beforeSecondRotation.camera);
  expect(advanced.clipPlane!.normal).not.toEqual(beforeSecondRotation.clipPlane!.normal);
  await changed(page, () => blankDrag('right'));
  const panned = await displayedFrame();
  expect(panned.camera!.targetMm).not.toEqual(advanced.camera!.targetMm);
  expect(panned.camera!.rotationXyzw).toEqual(advanced.camera!.rotationXyzw);
  expect(panned.clipPlane).toEqual(advanced.clipPlane);
  await changed(page, async () => {
    const bounds = (await page.getByTestId('viewport').boundingBox())!;
    await page.mouse.move(bounds.x + 35, bounds.y + bounds.height * 0.7);
    await page.mouse.wheel(0, -120);
  });
  const zoomed = await displayedFrame();
  expect(zoomed.camera!.orthoHeightMm).toBeLessThan(panned.camera!.orthoHeightMm);
  expect(zoomed.clipPlane).toEqual(panned.clipPlane);
  expect(JSON.parse((await overlay.getAttribute('data-plane'))!)).toEqual(zoomed.clipPlane);
  await expect(overlay).toHaveAttribute('data-seq', String(zoomed.seq));
  await page.screenshot({ path: 'test-results-real/clip-plane-camera-gestures.png' });
  await page.getByRole('button', { name: 'Rotate volume', exact: true }).click();
  const touch = await page.context().newCDPSession(page);
  await touch.send('Emulation.setTouchEmulationEnabled', { enabled: true });
  try {
    for (const width of [390, 320]) {
      await changed(page, () => page.setViewportSize({ width, height: 844 }));
      await changed(page, () => page.getByRole('button', { name: 'Reset camera', exact: true }).click());
      const point = await handlePoint(page, 'blue');
      await touch.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x: point.x, y: point.y, id: 1 }] });
      await expect(overlay).toHaveAttribute('data-dragging', 'true');
      await expect(overlay).toHaveAttribute('data-drag-action', 'translate');
      await touch.send('Input.dispatchTouchEvent', { type: 'touchCancel', touchPoints: [] });
      await expect(overlay).toHaveAttribute('data-dragging', 'false');
      const beforeTouch = await displayedFrame();
      await changed(page, async () => {
        const bounds = (await page.getByTestId('viewport').boundingBox())!;
        const start = { x: bounds.x + 25, y: bounds.y + bounds.height * 0.75 };
        await touch.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ ...start, id: 2 }] });
        for (let step = 1; step <= 12; step++) await touch.send('Input.dispatchTouchEvent', {
          type: 'touchMove', touchPoints: [{ x: start.x + step * 5, y: start.y - step * 3, id: 2 }]
        });
        await touch.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
      });
      const afterTouch = await displayedFrame();
      expect(afterTouch.camera!.rotationXyzw).not.toEqual(beforeTouch.camera!.rotationXyzw);
      expect(afterTouch.clipPlane).toEqual(beforeTouch.clipPlane);
      expect((await glassPixels(page)).visible).toBeGreaterThan(300);
      await page.screenshot({ path: `test-results-real/clip-plane-camera-mobile-${width}.png` });
    }
  } finally { await touch.detach(); }
  expect(errors).toEqual([]);
});