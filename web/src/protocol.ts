export type Vec3 = [number, number, number];
export type Quaternion = [number, number, number, number];
export type Quality = 'preview' | 'final';
export type SessionState = 'loading' | 'ready' | 'failed' | 'closing' | 'closed';

export interface Camera {
  targetMm: Vec3;
  rotationXyzw: Quaternion;
  orthoHeightMm: number;
}

export interface RenderStyle {
  presetId: string;
  windowWidth: number;
  windowLevel: number;
  opacityScale: number;
  lighting?: boolean;
  backgroundRgb: Vec3;
}

export interface CropBox {
  enabled: boolean;
  minimum: Vec3;
  maximum: Vec3;
}

export function fullCropBox(): CropBox {
  return { enabled: false, minimum: [0, 0, 0], maximum: [1, 1, 1] };
}

export interface ClipPlane {
  enabled: boolean;
  normal: Vec3;
  offsetMm: number;
  keepPositive: boolean;
}

export function defaultClipPlane(): ClipPlane {
  return { enabled: false, normal: [0, 0, 1], offsetMm: 0, keepPositive: true };
}

export interface AppearancePreset {
  id: string;
  name: string;
  windowWidth: number;
  windowLevel: number;
  opacityScale: number;
  lighting: boolean;
  colors: Vec3[];
}

export interface Series {
  id: string;
  description: string;
  fileCount: number;
  supported: boolean;
  reason: string;
  warning?: string;
}

export interface Session {
  sessionId: string;
  state: SessionState;
  sceneRevision?: number;
  volume?: { dimensions: Vec3; spacing: Vec3 };
  initial?: { camera: Camera; style: RenderStyle; cropBox?: CropBox; clipPlane?: ClipPlane };
  cropBoxSupported?: boolean;
  clipPlaneSupported?: boolean;
  standardViews?: StandardView[];
  loadTimings?: LoadTimings;
}

export interface LoadTimings {
  queueMs: number;
  dataLoadMs: number;
  validationMs: number;
  uploadMs: number;
  workerMs: number;
}

export interface RenderTimings {
  prepareMs: number;
  kernelMs: number;
  kernelWallMs: number;
  readbackMs: number;
  totalMs: number;
  encodeMs: number;
  queueMs: number;
}

export interface PresentationTimings {
  decodeMs: number;
  drawMs: number;
  receiveToDrawMs: number;
}

export interface StandardView {
  id: string;
  name: string;
  rotationXyzw: Quaternion;
}

export interface RenderRequest {
  type: 'render';
  seq: number;
  sceneRevision: number;
  quality: Quality;
  width: number;
  height: number;
  camera: Camera;
  style: RenderStyle;
  cropBox?: CropBox;
  clipPlane?: ClipPlane;
}

export interface FrameMeta {
  protocolVersion: 1;
  sessionId: string;
  seq: number;
  sceneRevision: number;
  width: number;
  height: number;
  encoding: 'jpeg' | 'png';
  payloadBytes: number;
  quality: Quality;
  camera?: Camera;
  cropBox?: CropBox;
  clipPlane?: ClipPlane;
  timings?: RenderTimings;
  presentationTimings?: PresentationTimings;
}

export function record(value: unknown): Record<string, unknown> {
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error('Invalid API object');
  }
  return value as Record<string, unknown>;
}

function finite(value: unknown): value is number {
  return typeof value === 'number' && Number.isFinite(value);
}

function tuple(value: unknown, length: number): value is number[] {
  return Array.isArray(value) && value.length === length && value.every(finite);
}

function positiveInteger(value: unknown, maximum = Number.MAX_SAFE_INTEGER): value is number {
  return finite(value) && Number.isSafeInteger(value) && value > 0 && value <= maximum;
}

function timings(value: unknown, keys: string[]): Record<string, number> {
  const values = record(value);
  const result: Record<string, number> = {};
  for (const key of keys) {
    const duration = values[key];
    if (!finite(duration) || duration < 0 || duration > 3600000) throw new Error('Invalid timing metadata');
    result[key] = duration;
  }
  return result;
}

export function parseCamera(value: unknown): Camera {
  const camera = record(value);
  if (!tuple(camera.targetMm, 3) || !tuple(camera.rotationXyzw, 4) ||
      Math.hypot(...camera.rotationXyzw) < 1e-8 ||
      !finite(camera.orthoHeightMm) || camera.orthoHeightMm <= 0) {
    throw new Error('Invalid camera');
  }
  return camera as unknown as Camera;
}

export function parseCropBox(value: unknown): CropBox {
  const crop = record(value);
  if (typeof crop.enabled !== 'boolean' || !tuple(crop.minimum, 3) || !tuple(crop.maximum, 3) ||
      crop.minimum.some((minimum, axis) => minimum < 0 || minimum >= (crop.maximum as number[])[axis]!) ||
      crop.maximum.some(maximum => maximum > 1)) throw new Error('Invalid crop box');
  return { enabled: crop.enabled, minimum: [...crop.minimum] as Vec3, maximum: [...crop.maximum] as Vec3 };
}

export function parseClipPlane(value: unknown): ClipPlane {
  const plane = record(value);
  if (typeof plane.enabled !== 'boolean' || typeof plane.keepPositive !== 'boolean' ||
      !tuple(plane.normal, 3) || Math.abs(plane.normal.reduce((sum, component) => sum + component * component, 0) - 1) > 0.001 ||
      !finite(plane.offsetMm) || Math.abs(plane.offsetMm) > 1000000) throw new Error('Invalid clip plane');
  return { enabled: plane.enabled, normal: [...plane.normal] as Vec3,
    offsetMm: plane.offsetMm, keepPositive: plane.keepPositive };
}

export function parseStyle(value: unknown): RenderStyle {
  const style = record(value);
  if (typeof style.presetId !== 'string' || !style.presetId ||
      !finite(style.windowWidth) || style.windowWidth <= 0 || !finite(style.windowLevel) ||
      !finite(style.opacityScale) || style.opacityScale < 0 ||
      (style.lighting !== undefined && typeof style.lighting !== 'boolean') ||
      !tuple(style.backgroundRgb, 3) || style.backgroundRgb.some(channel => channel < 0 || channel > 1)) {
    throw new Error('Invalid render style');
  }
  return style as unknown as RenderStyle;
}

export function parseCatalog(value: unknown): Series[] {
  const catalog = record(value);
  if (!Array.isArray(catalog.series)) throw new Error('Invalid series catalog');
  return catalog.series.map(item => {
    const series = record(item);
    if (typeof series.id !== 'string' || !series.id || typeof series.description !== 'string' ||
        !finite(series.fileCount) || !Number.isSafeInteger(series.fileCount) || series.fileCount < 0 ||
        typeof series.supported !== 'boolean' ||
        (series.reason !== undefined && typeof series.reason !== 'string') ||
        (series.warning !== undefined && typeof series.warning !== 'string')) {
      throw new Error('Invalid series entry');
    }
    return { ...series, reason: series.reason ?? '' } as unknown as Series;
  });
}

export function parsePresetCatalog(value: unknown): AppearancePreset[] {
  const catalog = record(value);
  if (!Array.isArray(catalog.presets) || catalog.presets.length === 0 || catalog.presets.length > 32) {
    throw new Error('Invalid appearance preset catalog');
  }
  const ids = new Set<string>();
  return catalog.presets.map(item => {
    const preset = record(item);
    if (typeof preset.id !== 'string' || !preset.id || preset.id.length > 128 || ids.has(preset.id) ||
        typeof preset.name !== 'string' || !preset.name || preset.name.length > 128 ||
        !finite(preset.windowWidth) || preset.windowWidth <= 0 || preset.windowWidth > 100000 ||
        !finite(preset.windowLevel) || Math.abs(preset.windowLevel) > 100000 ||
        !finite(preset.opacityScale) || preset.opacityScale < 0 || preset.opacityScale > 10 ||
        typeof preset.lighting !== 'boolean' || !Array.isArray(preset.colors) ||
        preset.colors.length < 2 || preset.colors.length > 128 ||
        preset.colors.some(color => !tuple(color, 3) || color.some(channel => channel < 0 || channel > 1))) {
      throw new Error('Invalid appearance preset');
    }
    ids.add(preset.id);
    return preset as unknown as AppearancePreset;
  });
}

export function applyAppearancePreset(style: RenderStyle, preset: AppearancePreset): RenderStyle {
  return { ...style, presetId: preset.id, windowWidth: preset.windowWidth,
    windowLevel: preset.windowLevel, opacityScale: preset.opacityScale, lighting: preset.lighting };
}

export function parseSession(value: unknown): Session {
  const session = record(value);
  const states: SessionState[] = ['loading', 'ready', 'failed', 'closing', 'closed'];
  if (typeof session.sessionId !== 'string' || !session.sessionId ||
      !states.includes(session.state as SessionState)) throw new Error('Invalid session response');
  if (session.state === 'ready') {
    const volume = record(session.volume);
    const initial = record(session.initial);
    if ((session.cropBoxSupported !== undefined && typeof session.cropBoxSupported !== 'boolean') ||
      (session.clipPlaneSupported !== undefined && typeof session.clipPlaneSupported !== 'boolean') ||
      !positiveInteger(session.sceneRevision) || !tuple(volume.dimensions, 3) ||
        !volume.dimensions.every(dimension => positiveInteger(dimension)) ||
        !tuple(volume.spacing, 3) || volume.spacing.some(spacing => spacing <= 0)) {
      throw new Error('Invalid ready volume');
    }
    return {
      sessionId: session.sessionId, state: 'ready', sceneRevision: session.sceneRevision,
      volume: volume as unknown as NonNullable<Session['volume']>,
      initial: { camera: parseCamera(initial.camera), style: parseStyle(initial.style),
        cropBox: initial.cropBox === undefined ? undefined : parseCropBox(initial.cropBox),
        clipPlane: initial.clipPlane === undefined ? undefined : parseClipPlane(initial.clipPlane) },
      cropBoxSupported: session.cropBoxSupported === true,
      clipPlaneSupported: session.clipPlaneSupported === true,
      standardViews: session.standardViews === undefined ? undefined : parseStandardViews(session.standardViews),
      loadTimings: session.loadTimings === undefined ? undefined : timings(session.loadTimings,
        ['queueMs', 'dataLoadMs', 'validationMs', 'uploadMs', 'workerMs']) as unknown as LoadTimings
    };
  }
  return { sessionId: session.sessionId, state: session.state as SessionState };
}

export function parseStandardViews(value: unknown): StandardView[] {
  if (!Array.isArray(value) || value.length !== 6) throw new Error('Invalid standard view catalog');
  const ids = new Set<string>();
  return value.map(item => {
    const view = record(item);
    if (typeof view.id !== 'string' || !['anterior', 'posterior', 'left', 'right', 'superior', 'inferior'].includes(view.id) ||
        ids.has(view.id) || typeof view.name !== 'string' || !view.name || view.name.length > 64 ||
        !tuple(view.rotationXyzw, 4) || Math.abs(Math.hypot(...view.rotationXyzw) - 1) > 0.001) {
      throw new Error('Invalid standard view');
    }
    ids.add(view.id);
    return view as unknown as StandardView;
  });
}

export function parseFramePacket(buffer: ArrayBuffer): { meta: FrameMeta; image: Blob } {
  if (buffer.byteLength < 5) throw new Error('Truncated frame header');
  const jsonLength = new DataView(buffer).getUint32(0, true);
  if (jsonLength === 0 || jsonLength > 64 * 1024 || jsonLength > buffer.byteLength - 4) {
    throw new Error('Invalid frame metadata length');
  }
  const json = new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array(buffer, 4, jsonLength));
  const meta = record(JSON.parse(json) as unknown);
  const payloadBytes = buffer.byteLength - 4 - jsonLength;
  if (meta.protocolVersion !== 1 || typeof meta.sessionId !== 'string' || !meta.sessionId ||
      !positiveInteger(meta.seq) || !positiveInteger(meta.sceneRevision) ||
      !positiveInteger(meta.width, 8192) || !positiveInteger(meta.height, 8192) ||
      (meta.encoding !== 'jpeg' && meta.encoding !== 'png') ||
      (meta.quality !== 'preview' && meta.quality !== 'final') ||
      !positiveInteger(meta.payloadBytes, 64 * 1024 * 1024) || meta.payloadBytes !== payloadBytes) {
    throw new Error('Invalid frame metadata or payload size');
  }
  return {
    meta: { ...meta, camera: meta.camera === undefined ? undefined : parseCamera(meta.camera),
      cropBox: meta.cropBox === undefined ? undefined : parseCropBox(meta.cropBox),
      clipPlane: meta.clipPlane === undefined ? undefined : parseClipPlane(meta.clipPlane),
      timings: meta.timings === undefined ? undefined : timings(meta.timings,
        ['prepareMs', 'kernelMs', 'kernelWallMs', 'readbackMs', 'totalMs', 'encodeMs', 'queueMs']) } as unknown as FrameMeta,
    image: new Blob([buffer.slice(4 + jsonLength)], { type: `image/${meta.encoding}` })
  };
}