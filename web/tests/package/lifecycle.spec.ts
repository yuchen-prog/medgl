import { expect, test } from '@playwright/test';
import { mkdir, writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';

test('extracted runtime completes repeated load, CUDA frame, export and close', async ({ page }) => {
  const cycles = Number(process.env.MEDGL_PACKAGE_CYCLES ?? '100');
  expect(Number.isInteger(cycles) && cycles >= 1 && cycles <= 100).toBe(true);
  await page.goto('/api/v1/health');
  const result = await page.evaluate(async count => {
    const pause = () => new Promise(resolve => setTimeout(resolve, 10));
    const getState = async (path: string) => {
      const response = await fetch(path);
      if (!response.ok) throw new Error('Session status failed');
      return await response.json() as { state: string; sceneRevision?: number;
        initial?: { camera: unknown; style: unknown } };
    };
    const waitState = async (path: string, state: string) => {
      const deadline = performance.now() + 30000;
      while (performance.now() < deadline) {
        const status = await getState(path);
        if (status.state === state) return status;
        if (status.state === 'failed') throw new Error('Package volume load failed');
        await pause();
      }
      throw new Error('Package lifecycle timeout');
    };
    const release = async (path: string) => {
      const deletion = await fetch(path, { method: 'DELETE' });
      if (deletion.status !== 202) throw new Error('Package delete failed');
      await waitState(path, 'closed');
    };
    const report: { cycle: number; input: string; elapsedMs: number; encodedBytes: number; pixels: number }[] = [];
    for (let cycle = 0; cycle < count; cycle++) {
      const start = performance.now();
      const seriesId = cycle % 2 === 0 ? 'series-1' : 'phantom';
      const created = await fetch('/api/v1/sessions', { method: 'POST',
        headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ seriesId }) });
      if (created.status !== 202) throw new Error('Package single-session slot not released');
      const session = await created.json() as { sessionId: string };
      const path = `/api/v1/sessions/${encodeURIComponent(session.sessionId)}`;
      let socket: WebSocket | undefined;
      try {
        const status = await waitState(path, 'ready');
        if (!status.initial || status.sceneRevision !== 1) throw new Error('Missing initial volume state');
        const request = { type: 'render', seq: 1, sceneRevision: 1, quality: 'final', width: 256, height: 256,
          camera: status.initial.camera, style: status.initial.style };
        const packet = await new Promise<ArrayBuffer>((resolve, reject) => {
          socket = new WebSocket(`ws://${location.host}${path}/stream`);
          socket.binaryType = 'arraybuffer';
          const timeout = setTimeout(() => reject(new Error('Package frame timeout')), 15000);
          socket.onopen = () => socket!.send(JSON.stringify(request));
          socket.onerror = () => { clearTimeout(timeout); reject(new Error('Package stream failed')); };
          socket.onmessage = event => {
            clearTimeout(timeout);
            if (!(event.data instanceof ArrayBuffer)) reject(new Error('Unexpected package stream reply'));
            else resolve(event.data);
          };
        });
        const length = new DataView(packet).getUint32(0, true);
        const meta = JSON.parse(new TextDecoder().decode(packet.slice(4, 4 + length))) as {
          seq: number; sessionId: string; width: number; height: number; encoding: string; pngCompression: string };
        if (meta.sessionId !== session.sessionId || meta.seq !== 1 || meta.width !== 256 || meta.height !== 256
          || meta.encoding !== 'png' || meta.pngCompression !== 'fast') throw new Error('Package frame contract mismatch');
        const image = packet.slice(4 + length);
        const bitmap = await createImageBitmap(new Blob([image], { type: 'image/png' }));
        const canvas = document.createElement('canvas');
        canvas.width = 256; canvas.height = 256;
        const context = canvas.getContext('2d')!;
        context.drawImage(bitmap, 0, 0);
        bitmap.close();
        const pixels = context.getImageData(0, 0, 256, 256).data;
        let nonblack = 0;
        for (let offset = 0; offset < pixels.length; offset += 4)
          if (pixels[offset]! + pixels[offset + 1]! + pixels[offset + 2]! > 0) nonblack++;
        if (nonblack < 100) throw new Error('Package CUDA image is blank');
        const exported = await fetch(`${path}/frame?seq=1&revision=1`);
        const bytes = new Uint8Array(await exported.arrayBuffer());
        const expected = new Uint8Array(image);
        if (!exported.ok || bytes.length !== expected.length || !bytes.every((value, index) => value === expected[index]))
          throw new Error('Package cached PNG export mismatch');
        report.push({ cycle, input: seriesId === 'phantom' ? 'phantom' : 'synthetic-ct',
          elapsedMs: performance.now() - start, encodedBytes: image.byteLength, pixels: nonblack });
      } finally {
        socket?.close();
        await release(path);
      }
    }
    return report;
  }, cycles);
  expect(result).toHaveLength(cycles);
  const output = fileURLToPath(new URL('../../../out/validation', import.meta.url));
  await mkdir(output, { recursive: true });
  await writeFile(`${output}/package-lifecycle.json`, JSON.stringify({ passed: true, cycles,
    data: 'synthetic-only', pngCompression: 'fast', samples: result,
    limitations: ['Not a multi-hour test', 'No process GPU memory attribution', 'No fault injection or external clinical comparison'] }, null, 2));
  await page.goto('about:blank');
});