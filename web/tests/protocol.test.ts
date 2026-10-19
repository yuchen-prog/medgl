import { describe, expect, it } from 'vitest';
import { defaultClipPlane, fullCropBox, parseCatalog, parseClipPlane, parseCropBox, parseFramePacket, parsePresetCatalog, parseSession, parseStandardViews } from '../src/protocol';

const metadata = {
  protocolVersion: 1, sessionId: 'phantom-session', seq: 7, sceneRevision: 1,
  width: 640, height: 480, encoding: 'png', payloadBytes: 3, quality: 'final'
};

function packet(overrides: Record<string, unknown> = {}): ArrayBuffer {
  const json = new TextEncoder().encode(JSON.stringify({ ...metadata, ...overrides }));
  const buffer = new ArrayBuffer(4 + json.length + 3);
  new DataView(buffer).setUint32(0, json.length, true);
  new Uint8Array(buffer, 4, json.length).set(json);
  new Uint8Array(buffer, 4 + json.length).set([1, 2, 3]);
  return buffer;
}

describe('binary frame protocol', () => {
  it('parses actual curve snapshots and rejects invalid points without requiring newer fields', () => {
    const curve = [{ position: 0, rgba: [0, 0, 0, 0] }, { position: 1, rgba: [1, 1, 1, 0.2] }];
    expect(parseFramePacket(packet({ transferFunction: curve })).meta.transferFunction).toEqual(curve);
    expect(parseFramePacket(packet()).meta.transferFunction).toBeUndefined();
    expect(() => parseFramePacket(packet({ transferFunction: [curve[1], curve[0]] }))).toThrow();
  });
  it('validates plane snapshots and accepts older frames', () => {
    const plane = { enabled: true, normal: [0.6, 0.8, 0], offsetMm: 12, keepPositive: false };
    expect(parseFramePacket(packet({ clipPlane: plane })).meta.clipPlane).toEqual(plane);
    expect(parseFramePacket(packet()).meta.clipPlane).toBeUndefined();
    for (const change of [{ enabled: 1 }, { keepPositive: 'yes' }, { normal: [0, 0, 0] },
      { normal: [1, 1, 1] }, { normal: [0, 1] }, { normal: [NaN, 0, 1] }, { offsetMm: Infinity }, { offsetMm: 1000001 }])
      expect(() => parseClipPlane({ ...plane, ...change })).toThrow();
    const parsed = parseClipPlane(plane);
    parsed.normal[0] = 0;
    expect(plane.normal[0]).toBe(0.6);
    defaultClipPlane().normal[0] = 1;
    expect(defaultClipPlane().normal).toEqual([0, 0, 1]);
  });
  it('validates optional crop snapshots and preserves old frames', () => {
    const crop = { enabled: true, minimum: [0.1, 0.2, 0.3], maximum: [0.8, 0.9, 1] };
    expect(parseCropBox(crop)).toEqual(crop);
    expect(parseFramePacket(packet({ cropBox: crop })).meta.cropBox).toEqual(crop);
    expect(parseFramePacket(packet()).meta.cropBox).toBeUndefined();
    for (const change of [{ enabled: 1 }, { minimum: [-0.1, 0, 0] }, { maximum: [1, 1, 1.1] },
      { minimum: [0, 0] }, { minimum: [0, 0.9, 0], maximum: [1, 0.5, 1] },
      { minimum: [0, 0, 1] }, { maximum: [NaN, 1, 1] }])
      expect(() => parseFramePacket(packet({ cropBox: { ...crop, ...change } }))).toThrow();
    const first = fullCropBox();
    first.minimum[0] = 0.4;
    expect(fullCropBox().minimum).toEqual([0, 0, 0]);
  });
  it('accepts finite nonnegative server intervals and rejects invalid metrics', () => {
    const intervals = { prepareMs: 1, kernelMs: 2, kernelWallMs: 3, readbackMs: 4, totalMs: 8, encodeMs: 5, queueMs: 0 };
    expect(parseFramePacket(packet({ timings: intervals })).meta.timings).toEqual(intervals);
    expect(parseFramePacket(packet()).meta.timings).toBeUndefined();
    for (const value of [-1, Infinity, '2', 3600001])
      expect(() => parseFramePacket(packet({ timings: { ...intervals, kernelMs: value } }))).toThrow();
    expect(() => parseFramePacket(packet({ timings: { kernelMs: 2 } }))).toThrow();
  });
  it('uses the camera snapshot attached to the actual frame', () => {
    const camera = { targetMm: [10, 20, 30], rotationXyzw: [0, 0, 0, 1], orthoHeightMm: 250 };
    expect(parseFramePacket(packet({ camera })).meta.camera).toEqual(camera);
    expect(() => parseFramePacket(packet({ camera: { ...camera, rotationXyzw: [0, 0, 0, 0] } }))).toThrow();
  });
  it('reads a little-endian UTF-8 header and image payload', async () => {
    const frame = parseFramePacket(packet({ sessionId: 'phantom-\u6d4b\u8bd5' }));
    expect(frame.meta.sessionId).toBe('phantom-\u6d4b\u8bd5');
    expect(frame.image.type).toBe('image/png');
    expect([...new Uint8Array(await frame.image.arrayBuffer())]).toEqual([1, 2, 3]);
  });

  it.each([0, 1, 4])('rejects a truncated %i-byte packet', size => {
    expect(() => parseFramePacket(new ArrayBuffer(size))).toThrow();
  });

  it.each([0, 65537, 0xffffffff])('rejects metadata length %i', length => {
    const buffer = packet();
    new DataView(buffer).setUint32(0, length, true);
    expect(() => parseFramePacket(buffer)).toThrow('length');
  });

  it.each([
    { payloadBytes: 2 }, { payloadBytes: 0 }, { seq: Number.MAX_SAFE_INTEGER + 1 },
    { seq: 0 }, { seq: 1.5 }, { sceneRevision: 0 }, { protocolVersion: 2 },
    { encoding: 'webp' }, { quality: 'unknown' }, { width: 8193 }, { height: -1 }
  ])('rejects invalid metadata %j', overrides => {
    expect(() => parseFramePacket(packet(overrides))).toThrow();
  });

  it('rejects invalid UTF-8 and malformed JSON', () => {
    const buffer = packet();
    new Uint8Array(buffer)[4] = 0xff;
    expect(() => parseFramePacket(buffer)).toThrow();
    new Uint8Array(buffer)[4] = 0;
    expect(() => parseFramePacket(buffer)).toThrow();
  });
});

describe('HTTP contracts', () => {
  it('validates optional load stages on a ready session', () => {
    const ready = { sessionId: 'one', state: 'ready', sceneRevision: 1,
      volume: { dimensions: [2, 3, 4], spacing: [1, 1, 1] }, initial: {
        camera: { targetMm: [0, 0, 0], rotationXyzw: [0, 0, 0, 1], orthoHeightMm: 20 },
        style: { presetId: 'ct-bone', windowWidth: 1000, windowLevel: 400, opacityScale: 1, backgroundRgb: [0, 0, 0] }
      }, loadTimings: { queueMs: 1, dataLoadMs: 2, validationMs: 3, uploadMs: 4, workerMs: 10 } };
    expect(parseSession(ready).loadTimings).toEqual(ready.loadTimings);
    const histogram = { minimum: -1000, maximum: 1000, counts: [24, ...Array<number>(255).fill(0)], voxelCount: 24 };
    const curve = [{ position: 0, rgba: [0, 0, 0, 0] }, { position: 1, rgba: [1, 1, 1, 0.2] }];
    expect(parseSession({ ...ready, histogram, transferFunctionSupported: true,
      initial: { ...ready.initial, style: { ...ready.initial.style, transferFunction: curve } } })).toMatchObject({
        histogram, transferFunctionSupported: true, initial: { style: { transferFunction: curve } }
      });
    expect(parseSession(ready).transferFunctionSupported).toBe(false);
    expect(() => parseSession({ ...ready, transferFunctionSupported: 'true' })).toThrow();
    expect(() => parseSession({ ...ready, histogram: { ...histogram, voxelCount: 25 } })).toThrow();
    expect(parseSession({ ...ready, clipPlaneSupported: true,
      initial: { ...ready.initial, clipPlane: defaultClipPlane() } }).initial?.clipPlane).toEqual(defaultClipPlane());
    expect(parseSession(ready).clipPlaneSupported).toBe(false);
    expect(() => parseSession({ ...ready, clipPlaneSupported: 'yes' })).toThrow();
    expect(() => parseSession({ ...ready, initial: { ...ready.initial, clipPlane: {} } })).toThrow();
    expect(() => parseSession({ ...ready, loadTimings: { ...ready.loadTimings, uploadMs: -1 } })).toThrow();
  });
  it('validates exactly six unique standard views with unit rotations', () => {
    const views = ['anterior', 'posterior', 'left', 'right', 'superior', 'inferior']
      .map(id => ({ id, name: id, rotationXyzw: [0, 0, 0, 1] }));
    expect(parseStandardViews(views)).toEqual(views);
    expect(() => parseStandardViews(views.slice(1))).toThrow();
    expect(() => parseStandardViews([views[0], views[0], ...views.slice(2)])).toThrow();
    expect(() => parseStandardViews([{ ...views[0], rotationXyzw: [0, 0, 0, 2] }, ...views.slice(1)])).toThrow();
  });
  it('validates presets and rejects duplicates, invalid colors or defaults', () => {
    const preset = { id: 'ct-heart', name: 'CT / Heart (contrast)', windowWidth: 800,
      windowLevel: 200, opacityScale: 1, lighting: true, colors: [[0, 0, 0], [1, 0.3, 0.2]] };
    expect(parsePresetCatalog({ presets: [preset] })).toEqual([preset]);
    const curve = [{ position: 0, rgba: [0, 0, 0, 0] }, { position: 1, rgba: [1, 1, 1, 0.2] }];
    expect(parsePresetCatalog({ presets: [{ ...preset, transferFunction: curve }] })[0]?.transferFunction).toEqual(curve);
    expect(() => parsePresetCatalog({ presets: [{ ...preset, transferFunction: [] }] })).toThrow();
    for (const override of [{ windowWidth: 0 }, { windowLevel: NaN }, { opacityScale: -1 },
      { lighting: 'true' }, { colors: [[0, 0, 0], [2, 0, 0]] }, { colors: [[0, 0, 0]] }]) {
      expect(() => parsePresetCatalog({ presets: [{ ...preset, ...override }] })).toThrow();
    }
    expect(() => parsePresetCatalog({ presets: [preset, preset] })).toThrow();
    expect(() => parsePresetCatalog({ presets: [] })).toThrow();
  });
  it('accepts minimal loading and closing responses', () => {
    expect(parseSession({ sessionId: 'one', state: 'loading' }).state).toBe('loading');
    expect(parseSession({ sessionId: 'one', state: 'closing' }).state).toBe('closing');
  });

  it('requires camera, style and volume on a ready session', () => {
    expect(() => parseSession({ sessionId: 'one', state: 'ready' })).toThrow();
  });

  it('preserves unsupported series reasons', () => {
    expect(parseCatalog({ series: [{ id: 'bad', description: 'Unsupported', fileCount: 2,
      supported: false, reason: 'Nonuniform spacing' }] })[0]?.reason).toBe('Nonuniform spacing');
  });

  it('preserves compatibility warnings without turning them into errors', () => {
    const series = { id: 'series-1', description: 'CT series', fileCount: 999,
      supported: true, reason: '', warning: 'Instance UID compatibility enabled' };
    expect(parseCatalog({ series: [series] })[0]).toEqual(series);
    expect(() => parseCatalog({ series: [{ ...series, warning: 12 }] })).toThrow();
  });
});