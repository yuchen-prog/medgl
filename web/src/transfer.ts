import { parseStyle, parseTransferFunction, record } from './protocol';
import type { Histogram, RenderStyle, TransferPoint, Vec3 } from './protocol';

export const PRESET_STORAGE_KEY = 'medgl.transfer-presets.v1';
export interface SavedPreset {
  id: string;
  name: string;
  style: RenderStyle;
}

export function intensityAt(position: number, style: Pick<RenderStyle, 'windowWidth' | 'windowLevel'>): number {
  return style.windowLevel + (position - 0.5) * style.windowWidth;
}

export function positionAt(intensity: number, style: Pick<RenderStyle, 'windowWidth' | 'windowLevel'>): number {
  return (intensity - style.windowLevel) / style.windowWidth + 0.5;
}

export function insertPoint(points: TransferPoint[], position: number): TransferPoint[] {
  if (points.length >= 64 || !Number.isFinite(position) || position <= 0 || position >= 1 ||
      points.some(point => Math.abs(point.position - position) < 0.001)) return points;
  const upper = points.findIndex(point => point.position > position);
  const first = points[Math.max(0, upper - 1)] ?? points[0]!;
  const second = points[upper] ?? points.at(-1)!;
  const fraction = Math.max(0, Math.min(1, (position - first.position) / (second.position - first.position || 1)));
  const rgba = first.rgba.map((channel, axis) => channel + (second.rgba[axis]! - channel) * fraction) as TransferPoint['rgba'];
  return [...points, { position, rgba }].sort((left, right) => left.position - right.position);
}

export function movePoint(points: TransferPoint[], index: number, position: number, opacity: number): TransferPoint[] {
  if (!points[index] || !Number.isFinite(position) || !Number.isFinite(opacity)) return points;
  const previous = points[Math.max(0, index - 1)]!.position;
  const next = points[Math.min(points.length - 1, index + 1)]!.position;
  const gap = Math.min(0.001, (next - previous) / 3);
  const minimum = index === 0 ? points[0]!.position : previous + gap;
  const maximum = index === points.length - 1 ? points.at(-1)!.position : next - gap;
  return points.map((point, current) => current === index ? {
    position: index === 0 || index === points.length - 1 ? point.position : Math.max(minimum, Math.min(maximum, position)),
    rgba: [point.rgba[0], point.rgba[1], point.rgba[2], Math.max(0, Math.min(1, opacity))]
  } : { position: point.position, rgba: [...point.rgba] });
}

export function histogramBars(histogram: Histogram | undefined, style: RenderStyle, count = 80): number[] {
  const bars = Array<number>(count).fill(0);
  if (!histogram) return bars;
  histogram.counts.forEach((amount, index) => {
    const intensity = histogram.minimum + (index + 0.5) / histogram.counts.length * (histogram.maximum - histogram.minimum);
    const position = positionAt(intensity, style);
    if (position >= 0 && position <= 1) bars[Math.min(count - 1, Math.floor(position * count))]! += amount;
  });
  const maximum = Math.max(1, ...bars);
  return bars.map(value => Math.log1p(value) / Math.log1p(maximum));
}

export function colorHex(color: Vec3 | TransferPoint['rgba']): string {
  return '#' + color.slice(0, 3).map(channel => Math.round(channel * 255).toString(16).padStart(2, '0')).join('');
}

export function hexColor(hex: string): Vec3 {
  return [1, 3, 5].map(start => Number.parseInt(hex.slice(start, start + 2), 16) / 255) as Vec3;
}

export function parseSavedPresets(text: string): SavedPreset[] {
  const library = record(JSON.parse(text) as unknown);
  if (library.version !== 1 || !Array.isArray(library.presets) || library.presets.length > 32)
    throw new Error('Invalid local preset library');
  const ids = new Set<string>();
  return library.presets.map(item => {
    const preset = record(item);
    if (typeof preset.id !== 'string' || !/^local-[\w-]{1,80}$/.test(preset.id) || ids.has(preset.id) ||
        typeof preset.name !== 'string' || !preset.name.trim() || preset.name.length > 64)
      throw new Error('Invalid local preset');
    const style = parseStyle(preset.style);
    if (!/^ct-[a-z-]+$/.test(style.presetId) || style.windowWidth > 100000 ||
        Math.abs(style.windowLevel) > 100000 || style.opacityScale > 10 || typeof style.lighting !== 'boolean')
      throw new Error('Invalid local preset style');
    ids.add(preset.id);
    return { id: preset.id, name: preset.name.trim(), style: { ...style,
      transferFunction: parseTransferFunction(style.transferFunction), backgroundRgb: [0, 0, 0] } };
  });
}