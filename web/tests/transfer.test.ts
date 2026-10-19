import { describe, expect, it } from 'vitest';
import { applyAppearancePreset, parseHistogram, parseTransferFunction } from '../src/protocol';
import type { RenderStyle, TransferPoint } from '../src/protocol';
import { colorHex, hexColor, histogramBars, insertPoint, intensityAt, movePoint, parseSavedPresets, positionAt } from '../src/transfer';

const points: TransferPoint[] = [{ position: 0, rgba: [0, 0, 0, 0] }, { position: 0.5, rgba: [1, 0, 0, 0.1] },
  { position: 1, rgba: [1, 1, 1, 0.4] }];
const style: RenderStyle = { presetId: 'ct-bone', windowWidth: 1800, windowLevel: 400,
  opacityScale: 1, lighting: true, backgroundRgb: [0, 0, 0], transferFunction: points };

describe('transfer curve contracts and editing', () => {
  it('validates, copies and rejects unordered or nonfinite control points', () => {
    const parsed = parseTransferFunction(points);
    parsed[0]!.rgba[0] = 1;
    expect(points[0]!.rgba[0]).toBe(0);
    for (const value of [[], [points[0]], [points[1], points[0]], [...points, points[2]],
      [{ position: 0, rgba: [0, 0, 0, NaN] }, points[2]],
      [{ position: -0.1, rgba: [0, 0, 0, 0] }, points[2]],
      [{ position: 0, rgba: [0, 0, 0, 2] }, points[2]], Array(65).fill(points[0])])
      expect(() => parseTransferFunction(value)).toThrow();
  });
  it('maps the editable normalized domain through WW/WL to HU', () => {
    expect(intensityAt(0, style)).toBe(-500);
    expect(intensityAt(1, style)).toBe(1300);
    for (const position of [0, 0.12, 0.5, 1]) expect(positionAt(intensityAt(position, style), style)).toBeCloseTo(position);
  });
  it('inserts interpolated colors/alpha and constrains movement without crossing', () => {
    const inserted = insertPoint(points, 0.75);
    expect(inserted[2]).toEqual({ position: 0.75, rgba: [1, 0.5, 0.5, 0.25] });
    expect(movePoint(inserted, 1, 0.9, 2)[1]).toEqual({ position: 0.749, rgba: [1, 0, 0, 1] });
    expect(movePoint(points, 0, 0.2, -1)[0]).toEqual(points[0]);
    expect(insertPoint(points, 0.5)).toBe(points);
    expect(insertPoint(points, NaN)).toBe(points);
    const dense: TransferPoint[] = [{ position: 0, rgba: [0, 0, 0, 0] },
      { position: 0.0001, rgba: [0, 0, 0, 0] }, { position: 0.0002, rgba: [0, 0, 0, 0] }];
    expect(() => parseTransferFunction(movePoint(dense, 1, 1, 0.1))).not.toThrow();
  });
  it('restores built-in curves and removes custom overrides for older presets', () => {
    const preset = { id: 'ct-lung', name: 'Lung', windowWidth: 1500, windowLevel: -600,
      opacityScale: 1, lighting: true, colors: [[0, 0, 0], [1, 1, 1]] as [number, number, number][], transferFunction: points };
    const restored = applyAppearancePreset(style, preset);
    restored.transferFunction![0]!.rgba[0] = 1;
    expect(preset.transferFunction[0]!.rgba[0]).toBe(0);
    expect(applyAppearancePreset(style, { ...preset, transferFunction: undefined }).transferFunction).toBeUndefined();
  });
  it('validates histogram counts and maps constant data without NaN', () => {
    const histogram = { minimum: 400, maximum: 400, counts: [2, ...Array<number>(255).fill(0)], voxelCount: 2 };
    expect(parseHistogram(histogram)).toEqual(histogram);
    expect(histogramBars(histogram, style).every(Number.isFinite)).toBe(true);
    expect(histogramBars(histogram, style).filter(value => value > 0)).toHaveLength(1);
    for (const override of [{ voxelCount: 3 }, { counts: [1] }, { minimum: 500 },
      { counts: [-1, ...Array<number>(255).fill(0)] }]) expect(() => parseHistogram({ ...histogram, ...override })).toThrow();
  });
  it('roundtrips colors and validates versioned local storage without image data', () => {
    expect(colorHex(hexColor('#14b8a6'))).toBe('#14b8a6');
    const preset = { id: 'local-one', name: 'My bone', style };
    const text = JSON.stringify({ version: 1, presets: [preset] });
    expect(parseSavedPresets(text)).toEqual([preset]);
    for (const invalid of ['{}', '{', JSON.stringify({ version: 2, presets: [] }),
      JSON.stringify({ version: 1, presets: [preset, preset] }),
      JSON.stringify({ version: 1, presets: [{ ...preset, style: { ...style, transferFunction: undefined } }] }),
      JSON.stringify({ version: 1, presets: [{ ...preset, style: { ...style, transferFunction: [] } }] })])
      expect(() => parseSavedPresets(invalid)).toThrow();
  });
});