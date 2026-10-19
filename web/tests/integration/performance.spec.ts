import { expect } from '@playwright/test';
import { test } from './fixtures';
import { mkdir, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import type { FrameMeta } from '../../src/protocol';

function statistics(values: number[]) {
  const sorted = [...values].sort((first, second) => first - second);
  const middle = Math.floor(sorted.length / 2);
  return { count: sorted.length,
    median: sorted.length % 2 ? sorted[middle]! : (sorted[middle - 1]! + sorted[middle]!) / 2,
    p95: sorted[Math.ceil(sorted.length * .95) - 1]!,
    minimum: sorted[0]!, maximum: sorted.at(-1)!,
    mean: sorted.reduce((sum, value) => sum + value, 0) / sorted.length };
}

test('browser send-to-canvas samples with separate GPU, codec and decode stages', async ({ page }, info) => {
  test.setTimeout(90000);
  const frames = new Map<number, FrameMeta>();
  const modes = new Map<number, unknown>();
  let packetError = false;
  page.on('websocket', socket => socket.on('framereceived', event => {
    if (typeof event.payload === 'string') return;
    try {
      const length = event.payload.readUInt32LE(0);
      const metadata = JSON.parse(event.payload.subarray(4, 4 + length).toString('utf8')) as FrameMeta;
      frames.set(metadata.seq, metadata);
      modes.set(metadata.seq, (metadata as FrameMeta & { pngCompression?: unknown }).pngCompression);
      if (frames.size > 64) {
        const oldest = frames.keys().next().value!;
        frames.delete(oldest);
        modes.delete(oldest);
      }
    } catch { packetError = true; }
  }));
  await page.setViewportSize({ width: 1100, height: 760 });
  const expectedMode = process.env.MEDGL_PNG_COMPRESSION ?? 'default';
  const healthResponse = await page.request.get('/api/v1/health');
  expect(healthResponse.ok()).toBe(true);
  const health = await healthResponse.json() as { pngCompression: string };
  expect(health.pngCompression).toBe(expectedMode);
  await page.goto('/');
  const download = page.getByRole('button', { name: 'Download final PNG', exact: true });
  await expect(download).toBeEnabled({ timeout: 30000 });
  const series = process.env.MEDGL_PERF_SERIES;
  if (series) {
    await page.getByRole('combobox', { name: 'Series', exact: true }).selectOption(series);
    await page.getByRole('button', { name: 'Load series', exact: true }).click();
    await expect(page.locator('.geometry')).not.toContainText('96 × 96 × 96');
    await expect(download).toBeEnabled({ timeout: 30000 });
  }
  const resolution = process.env.MEDGL_PERF_RESOLUTION ?? '50%';
  await page.getByRole('combobox', { name: 'Render size' }).selectOption(resolution);
  if (resolution === '50%')
    await expect.poll(() => page.locator('canvas').evaluate((canvas: HTMLCanvasElement) => canvas.width)).toBeLessThan(600);
  await expect(download).toBeEnabled();
  const samples: Record<string, number | string>[] = [];
  for (let index = 0; index < 14; ++index) {
    const view = ['anterior', 'left', 'inferior'][index % 3]!;
    const before = await page.locator('.viewport-bottom-right .mono').innerText();
    await page.getByRole('button', { name: `${view[0]!.toUpperCase()}${view.slice(1)} view`, exact: true }).click();
    await expect(page.locator('.viewport-bottom-right .mono')).not.toHaveText(before);
    await expect(download).toBeEnabled();
    await expect(page.locator('.viewport-bottom-right .viewport-label')).toHaveText('FINAL');
    const sequence = Number((await page.locator('.viewport-bottom-right .mono').innerText()).slice(1));
    const meta = frames.get(sequence);
    expect(meta?.quality).toBe('final');
    expect(modes.get(sequence)).toBe(expectedMode);
    if (!meta?.timings) throw new Error('Missing server stage timings');
    const presentation = await page.getByTestId('frame-timings').evaluate(panel =>
      Object.fromEntries([...panel.querySelectorAll('div')].map(row =>
        [row.querySelector('dt')!.textContent!, Number.parseFloat(row.querySelector('dd')!.textContent!)])));
    const sendToCanvasMs = Number.parseFloat(await page.getByTestId('send-to-canvas').innerText());
    const sample = { view, width: meta.width, height: meta.height, seq: sequence,
      payloadBytes: meta.payloadBytes, sendToCanvasMs,
      ...meta.timings, decodeMs: presentation.Decode!, canvasSubmitMs: presentation['Canvas submit']! };
    for (const [name, value] of Object.entries(sample))
      if (name !== 'view') expect(Number.isFinite(value) && Number(value) >= 0).toBe(true);
    if (index >= 2) samples.push(sample);
  }
  expect(packetError).toBe(false);
  expect(samples).toHaveLength(12);
  const stats: Record<string, ReturnType<typeof statistics>> = {};
  for (const metric of ['sendToCanvasMs', 'prepareMs', 'kernelMs', 'kernelWallMs', 'readbackMs',
    'totalMs', 'encodeMs', 'queueMs', 'decodeMs', 'canvasSubmitMs', 'payloadBytes'])
    stats[metric] = statistics(samples.map(sample => Number(sample[metric])));
  await mkdir(info.project.outputDir, { recursive: true });
  await writeFile(join(info.project.outputDir, 'browser-performance.json'), JSON.stringify({
    version: 1, input: series ? 'dicom-read-only' : 'synthetic-phantom-96-cubed',
    pngCompression: expectedMode, resolution, preset: 'ct-bone', quality: 'final',
    stepMm: .5, lighting: true, warmupSamples: 2, samples, stats,
    limitations: ['Loopback headless Chromium; no remote network or physical display scanout measurement',
      'Browser send-to-canvas is rounded to milliseconds; decode/Canvas submission rounded to two decimals',
      'Server clocks are independent; queueMs is submit-to-worker-start, not submit-to-kernel-start',
      'Samples rotate among three views; aggregate is a mixed-view workload, not one fixed camera',
      'No clinical validation or FPS guarantee']
  }, null, 2) + '\n');
  await page.goto('about:blank');
});