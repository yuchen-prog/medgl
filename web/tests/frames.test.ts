import { describe, expect, it, vi } from 'vitest';
import { FramePresenter } from '../src/frames';
import type { FrameTarget } from '../src/frames';

function packet(seq: number, overrides: Record<string, unknown> = {}): ArrayBuffer {
  const meta = new TextEncoder().encode(JSON.stringify({ protocolVersion: 1, sessionId: 'one', seq,
    sceneRevision: 1, width: 640, height: 480, encoding: 'png', payloadBytes: 1, quality: 'final', ...overrides }));
  const buffer = new ArrayBuffer(4 + meta.length + 1);
  new DataView(buffer).setUint32(0, meta.length, true);
  new Uint8Array(buffer, 4).set(meta);
  return buffer;
}

function setup() {
  let target: FrameTarget | null = { sessionId: 'one', revision: 1, width: 640, height: 480, issuedSeq: 10 };
  const drawImage = vi.fn();
  const canvas = { width: 640, height: 480, getContext: () => ({ drawImage }) } as unknown as HTMLCanvasElement;
  const presented = vi.fn();
  const pending: ((bitmap: ImageBitmap) => void)[] = [];
  const decode = vi.fn(() => new Promise<ImageBitmap>(resolve => pending.push(resolve)));
  const presenter = new FramePresenter(() => target, () => canvas, presented, decode);
  const bitmap = () => ({ width: 640, height: 480, close: vi.fn() }) as unknown as ImageBitmap;
  return { presenter, drawImage, decode, pending, bitmap, presented, canvas,
    setTarget: (next: FrameTarget | null) => { target = next; } };
}

describe('latest decoded frame', () => {
  it('times asynchronous decode and Canvas submission without replacing server intervals', async () => {
    const context = setup();
    const clock = vi.spyOn(performance, 'now').mockReturnValueOnce(10).mockReturnValueOnce(11)
      .mockReturnValueOnce(16).mockReturnValueOnce(17).mockReturnValueOnce(18);
    try {
      const task = context.presenter.accept(packet(1, { timings: {
        prepareMs: 1, kernelMs: 2, kernelWallMs: 3, readbackMs: 4, totalMs: 8, encodeMs: 5, queueMs: 0
      } }));
      context.pending[0]?.(context.bitmap());
      await task;
      expect(context.presented).toHaveBeenCalledWith(expect.objectContaining({
        timings: expect.objectContaining({ kernelMs: 2 }),
        presentationTimings: { decodeMs: 5, drawMs: 1, receiveToDrawMs: 8 }
      }));
    } finally { clock.mockRestore(); }
  });
  it('publishes orientation only from the most recent displayed image', async () => {
    const context = setup();
    const olderCamera = { targetMm: [0, 0, 0], rotationXyzw: [0, 0, 0, 1], orthoHeightMm: 200 };
    const newerCamera = { ...olderCamera, rotationXyzw: [0, Math.SQRT1_2, Math.SQRT1_2, 0] };
    const older = context.presenter.accept(packet(2, { camera: olderCamera }));
    const newer = context.presenter.accept(packet(3, { camera: newerCamera }));
    context.pending[1]?.(context.bitmap());
    await newer;
    context.pending[0]?.(context.bitmap());
    await older;
    expect(context.presented).toHaveBeenCalledTimes(1);
    expect(context.presented).toHaveBeenCalledWith(expect.objectContaining({ seq: 3, camera: newerCamera }));
  });
  it('checks sequence after asynchronous decoding and closes stale bitmaps', async () => {
    const context = setup();
    const older = context.presenter.accept(packet(2));
    const newer = context.presenter.accept(packet(3));
    const olderBitmap = context.bitmap();
    const newerBitmap = context.bitmap();
    context.pending[1]?.(newerBitmap);
    await newer;
    context.pending[0]?.(olderBitmap);
    await older;
    expect(context.drawImage).toHaveBeenCalledTimes(1);
    expect(context.presenter.latestDecodedSeq).toBe(3);
    expect(olderBitmap.close).toHaveBeenCalledOnce();
    expect(newerBitmap.close).toHaveBeenCalledOnce();
    await context.presenter.accept(packet(1));
    expect(context.decode).toHaveBeenCalledTimes(2);
    expect(context.canvas.width).toBe(640);
  });

  it.each([{ sessionId: 'other' }, { sceneRevision: 2 }, { width: 600 }, { seq: 11 }])(
    'discards nonmatching frames before decoding %j', async overrides => {
      const context = setup();
      await context.presenter.accept(packet(1, overrides));
      expect(context.decode).not.toHaveBeenCalled();
    });

  it('closes bitmaps after session disposal', async () => {
    const context = setup();
    const task = context.presenter.accept(packet(1));
    context.presenter.dispose();
    const bitmap = context.bitmap();
    context.pending[0]?.(bitmap);
    await task;
    expect(context.drawImage).not.toHaveBeenCalled();
    expect(bitmap.close).toHaveBeenCalledOnce();
  });

  it('rejects inconsistent decoded dimensions without leaking a bitmap', async () => {
    const context = setup();
    const task = context.presenter.accept(packet(1));
    const bitmap = { ...context.bitmap(), width: 100 } as ImageBitmap;
    context.pending[0]?.(bitmap);
    await expect(task).rejects.toThrow('dimensions');
    expect(bitmap.close).toHaveBeenCalledOnce();
  });
});