import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import * as api from '../src/api';
import { WorkstationClient } from '../src/workstation';
import { defaultClipPlane, fullCropBox } from '../src/protocol';
import { PRESET_STORAGE_KEY } from '../src/transfer';
import type { RenderRequest, Session } from '../src/protocol';

vi.mock('../src/api', () => ({
  catalog: vi.fn(), appearancePresets: vi.fn(), createSession: vi.fn(), closeSession: vi.fn(), waitUntilReady: vi.fn(),
  streamUrl: () => 'ws://localhost/stream', sessionPath: (id: string) => `/api/v1/sessions/${id}`
}));

class FakeSocket {
  static OPEN = 1;
  static instances: FakeSocket[] = [];
  readyState = 0;
  bufferedAmount = 0;
  binaryType = '';
  onopen: (() => void) | null = null;
  onclose: (() => void) | null = null;
  onerror: (() => void) | null = null;
  onmessage: (() => void) | null = null;
  send = vi.fn();
  constructor() { FakeSocket.instances.push(this); }
  open() { this.readyState = 1; this.onopen?.(); }
  close() { this.readyState = 3; this.onclose?.(); }
}

const phantom = { id: 'phantom', description: 'Synthetic phantom', fileCount: 0, supported: true, reason: '' };
const patient = { id: 'patient', description: 'Explicit selection', fileCount: 123, supported: true, reason: '' };
const heartPreset = { id: 'ct-heart', name: 'CT / Heart (contrast)', windowWidth: 800,
  windowLevel: 200, opacityScale: 1, lighting: true, colors: [[0, 0, 0], [1, 0.3, 0.2]] as [number, number, number][] };
const ready: Session = {
  sessionId: 'one', state: 'ready', sceneRevision: 1,
  volume: { dimensions: [100, 100, 100], spacing: [1, 1, 1] },
  standardViews: [{ id: 'anterior', name: 'Anterior', rotationXyzw: [0, Math.SQRT1_2, Math.SQRT1_2, 0] }],
  initial: { camera: { targetMm: [0, 0, 0], rotationXyzw: [0, 0, 0, 1], orthoHeightMm: 200 },
    style: { presetId: 'ct-bone', windowWidth: 1800, windowLevel: 400, opacityScale: 1, backgroundRgb: [0, 0, 0] } }
};

async function flush() { for (let index = 0; index < 12; index++) await Promise.resolve(); }
const clients: WorkstationClient[] = [];
function client() {
  const instance = new WorkstationClient({ current: 0 });
  instance.resize(640, 480);
  clients.push(instance);
  return instance;
}

beforeEach(() => {
  vi.useFakeTimers();
  vi.stubGlobal('WebSocket', FakeSocket);
  vi.stubGlobal('createImageBitmap', vi.fn());
  FakeSocket.instances = [];
  vi.mocked(api.catalog).mockResolvedValue([phantom, patient]);
  vi.mocked(api.appearancePresets).mockResolvedValue([heartPreset]);
  vi.mocked(api.createSession).mockResolvedValue(ready);
  vi.mocked(api.closeSession).mockResolvedValue();
  vi.mocked(api.waitUntilReady).mockResolvedValue(ready);
});

afterEach(async () => {
  for (const instance of clients.splice(0)) instance.stop();
  await flush();
  vi.useRealTimers();
  vi.unstubAllGlobals();
  vi.resetAllMocks();
});

describe('session ownership and render scheduling', () => {
  it('does not send edited transfer curves to an older service', async () => {
    const instance = client();
    await instance.start(); await flush();
    instance.setTransferFunction([{ position: 0, rgba: [0, 0, 0, 0] }, { position: 1, rgba: [1, 1, 1, 1] }]);
    const socket = FakeSocket.instances[0]!;
    socket.open();
    expect(instance.getSnapshot().style?.transferFunction).toBeUndefined();
    expect(JSON.parse(socket.send.mock.calls[0]![0] as string).style).not.toHaveProperty('transferFunction');
  });
  it('preserves edited curves on view/crop/reconnect, restores presets, saves locally and resets on a new series', async () => {
    const points = [{ position: 0, rgba: [0, 0, 0, 0] as [number, number, number, number] },
      { position: 1, rgba: [1, 1, 1, 0.2] as [number, number, number, number] }];
    const storage = new Map<string, string>();
    vi.stubGlobal('localStorage', { getItem: (key: string) => storage.get(key) ?? null,
      setItem: (key: string, value: string) => storage.set(key, value) });
    vi.mocked(api.createSession).mockResolvedValue({ ...ready, transferFunctionSupported: true,
      initial: { ...ready.initial!, style: { ...ready.initial!.style, transferFunction: points } } });
    vi.mocked(api.appearancePresets).mockResolvedValue([{ ...heartPreset, transferFunction: points },
      { ...heartPreset, id: 'ct-bone', name: 'CT / Bone', transferFunction: points }]);
    const instance = client();
    await instance.start(); await flush();
    const socket = FakeSocket.instances[0]!;
    socket.open();
    instance.setTransferFunction([{ ...points[0]! }, { position: 1, rgba: [1, 0, 0, 0.4] }]);
    instance.setStandardView('anterior');
    await vi.advanceTimersByTimeAsync(200);
    const edited = structuredClone(instance.getSnapshot().style!.transferFunction);
    expect(JSON.parse(socket.send.mock.calls.at(-1)![0] as string).style.transferFunction).toEqual(edited);
    instance.reconnect(); FakeSocket.instances.at(-1)!.open();
    expect(instance.getSnapshot().style!.transferFunction).toEqual(edited);
    instance.saveCustomPreset('My bone');
    const saved = instance.getSnapshot().customPresets[0]!;
    expect(storage.get(PRESET_STORAGE_KEY)).not.toMatch(/sessionId|histogram|voxels|targetMm/);
    instance.applyPreset('ct-heart');
    expect(instance.getSnapshot().style!.transferFunction).toEqual(points);
    instance.applyPreset(saved.id);
    expect(instance.getSnapshot().style!.transferFunction).toEqual(edited);
    expect(instance.getSnapshot().style!.presetId).toBe('ct-bone');
    instance.setStyle({ ...instance.getSnapshot().style!, windowWidth: 2200, windowLevel: 550, lighting: false });
    instance.resetTransferFunction();
    expect(instance.getSnapshot().style).toMatchObject({ windowWidth: 2200, windowLevel: 550, lighting: false, transferFunction: points });
    await instance.loadSeries('patient');
    expect(instance.getSnapshot().customPresetId).toBeNull();
    expect(instance.getSnapshot().style!.transferFunction).toEqual(points);
    instance.applyPreset(saved.id);
    instance.deleteCustomPreset();
    expect(instance.getSnapshot().customPresets).toHaveLength(0);
    vi.stubGlobal('localStorage', { getItem: () => '{broken', setItem: () => { throw new Error('denied'); } });
    await instance.start();
    expect(instance.getSnapshot().storageError).toContain('could not be read');
    instance.saveCustomPreset('Retry');
    expect(instance.getSnapshot().storageError).toContain('could not be saved');
  });
  it('copies absolute planes, preserves them on view/preset/reconnect, resets for a new volume', async () => {
    vi.mocked(api.createSession).mockResolvedValue({ ...ready, clipPlaneSupported: true });
    const instance = client();
    await instance.start();
    await flush();
    const socket = FakeSocket.instances[0]!;
    socket.open();
    const plane = { ...defaultClipPlane(), enabled: true, normal: [0.6, 0.8, 0] as [number, number, number], offsetMm: 15 };
    instance.setClipPlane(plane);
    plane.normal[0] = 0;
    instance.applyPreset('ct-heart');
    instance.setStandardView('anterior');
    await vi.advanceTimersByTimeAsync(200);
    const expected = { ...plane, normal: [0.6, 0.8, 0] };
    expect(JSON.parse(socket.send.mock.calls.at(-1)![0] as string).clipPlane).toEqual(expected);
    instance.reconnect();
    FakeSocket.instances.at(-1)!.open();
    expect(JSON.parse(FakeSocket.instances.at(-1)!.send.mock.calls[0]![0] as string).clipPlane).toEqual(expected);
    instance.resetClipPlane();
    expect(instance.getSnapshot().clipPlane).toEqual(defaultClipPlane());
    instance.setClipPlane({ ...defaultClipPlane(), enabled: true });
    await instance.loadSeries('patient');
    expect(instance.getSnapshot().clipPlane).toEqual(defaultClipPlane());
  });
  it('omits plane state for older services', async () => {
    const instance = client();
    await instance.start();
    await flush();
    instance.setClipPlane({ ...defaultClipPlane(), enabled: true });
    const socket = FakeSocket.instances[0]!;
    socket.open();
    expect(instance.getSnapshot().clipPlane).toEqual(defaultClipPlane());
    expect(JSON.parse(socket.send.mock.calls[0]![0] as string)).not.toHaveProperty('clipPlane');
  });
  it('sends crop snapshots, preserves them across appearance/views, and resets on new series', async () => {
    vi.mocked(api.createSession).mockResolvedValue({ ...ready, cropBoxSupported: true });
    const instance = client();
    await instance.start();
    await flush();
    const socket = FakeSocket.instances[0]!;
    socket.open();
    const crop = { enabled: true, minimum: [0.2, 0, 0] as [number, number, number],
      maximum: [1, 1, 0.7] as [number, number, number] };
    instance.setCropBox(crop);
    crop.minimum[0] = 0.5;
    instance.applyPreset('ct-heart');
    instance.setStandardView('anterior');
    await vi.advanceTimersByTimeAsync(200);
    const request = JSON.parse(socket.send.mock.calls.at(-1)![0] as string) as RenderRequest;
    expect(request.cropBox).toEqual({ ...crop, minimum: [0.2, 0, 0] });
    instance.reconnect();
    FakeSocket.instances.at(-1)!.open();
    expect(JSON.parse(FakeSocket.instances.at(-1)!.send.mock.calls[0]![0] as string).cropBox).toEqual(request.cropBox);
    instance.resetCropBox();
    expect(instance.getSnapshot().cropBox).toEqual(fullCropBox());
    instance.setCropBox(crop);
    await instance.loadSeries('patient');
    expect(instance.getSnapshot().cropBox).toEqual(fullCropBox());
  });
  it('does not send or apply cropping to a service without crop support', async () => {
    const instance = client();
    await instance.start();
    await flush();
    instance.setCropBox({ enabled: true, minimum: [0.2, 0, 0], maximum: [1, 1, 1] });
    const socket = FakeSocket.instances[0]!;
    socket.open();
    expect(instance.getSnapshot().cropBox?.enabled).toBe(false);
    expect(JSON.parse(socket.send.mock.calls[0]![0] as string)).not.toHaveProperty('cropBox');
  });
  it('resets standard-view framing without changing appearance', async () => {
    const instance = client();
    await instance.start();
    await flush();
    const style = instance.getSnapshot().style;
    instance.setCamera({ targetMm: [300, 400, 500], rotationXyzw: [0, 0, 1, 0], orthoHeightMm: 700 });
    instance.setStandardView('anterior');
    expect(instance.getSnapshot().camera).toEqual({ ...ready.initial!.camera,
      rotationXyzw: [0, Math.SQRT1_2, Math.SQRT1_2, 0] });
    expect(instance.getSnapshot().style).toBe(style);
    const applied = instance.getSnapshot().camera;
    instance.setStandardView('unknown');
    expect(instance.getSnapshot().camera).toBe(applied);
  });
  it('switches the entire preset without changing camera or background', async () => {
    const instance = client();
    await instance.start();
    await flush();
    const camera = structuredClone(instance.getSnapshot().camera);
    const background = [0.1, 0.2, 0.3] as [number, number, number];
    instance.setStyle({ ...ready.initial!.style, opacityScale: 2.5, lighting: false, backgroundRgb: background });
    instance.applyPreset('ct-heart');
    expect(instance.getSnapshot().style).toEqual({ presetId: 'ct-heart', windowWidth: 800,
      windowLevel: 200, opacityScale: 1, lighting: true, backgroundRgb: background });
    expect(instance.getSnapshot().camera).toEqual(camera);
    const applied = instance.getSnapshot().style;
    instance.applyPreset('unknown');
    expect(instance.getSnapshot().style).toBe(applied);
  });

  it('keeps series loading available if the preset catalog fails', async () => {
    vi.mocked(api.appearancePresets).mockRejectedValueOnce(new Error('Preset service unavailable'));
    const instance = client();
    await instance.start();
    await flush();
    expect(instance.getSnapshot().phase).toBe('ready');
    expect(instance.getSnapshot().presetError).toBe('Preset service unavailable');
    await instance.refreshPresets();
    expect(instance.getSnapshot().presetError).toBeNull();
    expect(instance.getSnapshot().presets).toEqual([heartPreset]);
  });
  it('autoloads only the synthetic phantom', async () => {
    const instance = client();
    await instance.start();
    await flush();
    expect(api.createSession).toHaveBeenCalledWith('phantom');
    instance.stop();
    await flush();
    vi.mocked(api.createSession).mockClear();
    vi.mocked(api.catalog).mockResolvedValue([patient]);
    await client().start();
    await flush();
    expect(api.createSession).not.toHaveBeenCalled();
  });

  it('cannot create a second session before closed is confirmed', async () => {
    const instance = client();
    await instance.start();
    await flush();
    let confirm: (() => void) | undefined;
    vi.mocked(api.closeSession).mockImplementationOnce(() => new Promise(resolve => { confirm = resolve; }));
    const switching = instance.loadSeries('patient');
    await flush();
    expect(instance.getSnapshot().phase).toBe('closing');
    expect(api.createSession).toHaveBeenCalledTimes(1);
    confirm?.();
    await switching;
    expect(api.createSession).toHaveBeenNthCalledWith(2, 'patient');
  });

  it('keeps ownership locked after a close timeout and retries closure first', async () => {
    const instance = client();
    await instance.start();
    await flush();
    vi.mocked(api.closeSession).mockRejectedValueOnce(new Error('Close timeout'));
    await instance.loadSeries('patient');
    expect(instance.getSnapshot().phase).toBe('error');
    expect(api.createSession).toHaveBeenCalledTimes(1);
    await instance.loadSeries('patient');
    expect(api.closeSession).toHaveBeenNthCalledWith(2, 'one', false);
    expect(api.createSession).toHaveBeenCalledTimes(2);
  });

  it('deletes a session whose POST finishes after unmount', async () => {
    let finishPost: ((session: Session) => void) | undefined;
    vi.mocked(api.createSession).mockImplementationOnce(() => new Promise(resolve => { finishPost = resolve; }));
    const instance = client();
    await instance.start();
    await flush();
    instance.stop();
    finishPost?.(ready);
    await flush();
    expect(api.closeSession).toHaveBeenCalledWith('one', true);
    expect(FakeSocket.instances).toHaveLength(0);
  });

  it('sends full absolute updates at most every 34ms and a debounced final', async () => {
    const instance = client();
    await instance.start();
    await flush();
    const socket = FakeSocket.instances[0]!;
    socket.open();
    socket.send.mockClear();
    for (let index = 0; index < 10; index++) {
      instance.setCamera({ ...ready.initial!.camera, orthoHeightMm: 200 + index });
    }
    await vi.advanceTimersByTimeAsync(34);
    expect(socket.send).toHaveBeenCalledTimes(1);
    const preview = JSON.parse(socket.send.mock.calls[0]![0] as string) as RenderRequest;
    expect(preview).toMatchObject({ type: 'render', quality: 'preview', width: 640, height: 480,
      camera: { orthoHeightMm: 209 }, style: ready.initial!.style, sceneRevision: 1 });
    instance.finishInteraction();
    await vi.advanceTimersByTimeAsync(200);
    const final = JSON.parse(socket.send.mock.calls[1]![0] as string) as RenderRequest;
    expect(final.quality).toBe('final');
    expect(final.seq).toBeGreaterThan(preview.seq);
    instance.stop();
    await vi.advanceTimersByTimeAsync(1000);
    expect(socket.send).toHaveBeenCalledTimes(2);
  });

  it('bounds automatic reconnects and allows an explicit retry', async () => {
    const instance = client();
    await instance.start();
    await flush();
    for (let index = 0; index < 3; index++) {
      FakeSocket.instances.at(-1)!.close();
      await vi.advanceTimersByTimeAsync(500 * 2 ** index);
    }
    FakeSocket.instances.at(-1)!.close();
    await vi.advanceTimersByTimeAsync(30000);
    expect(FakeSocket.instances).toHaveLength(4);
    expect(instance.getSnapshot().connection).toBe('offline');
    expect(instance.getSnapshot().rendering).toBe(false);
    instance.reconnect();
    expect(FakeSocket.instances).toHaveLength(5);
  });

  it('leaves the pending state when no frame arrives', async () => {
    const instance = client();
    await instance.start();
    await flush();
    FakeSocket.instances[0]!.open();
    expect(instance.getSnapshot().rendering).toBe(true);
    await vi.advanceTimersByTimeAsync(12000);
    expect(instance.getSnapshot().rendering).toBe(false);
    expect(instance.getSnapshot().error).toContain('12 seconds');
  });
});