import { describe, expect, it } from 'vitest';
import { directionLabel, matchedStandardView, multiplyQuaternion, normalizeQuaternion, orbitCamera, orientationLabels, panCamera, rotateVector, zoomCamera } from '../src/camera';
import type { Camera } from '../src/protocol';

const camera: Camera = { targetMm: [10, 20, 30], rotationXyzw: [0, 0, 0, 1], orthoHeightMm: 200 };

describe('patient-coordinate camera math', () => {
  it('labels frame directions in LPS rather than screen coordinates', () => {
    expect(orientationLabels([0, 0, 0, 1])).toEqual({ left: 'R', right: 'L', top: 'P', bottom: 'A' });
    const half = Math.SQRT1_2;
    expect(orientationLabels([0, half, half, 0])).toEqual({ left: 'L', right: 'R', top: 'S', bottom: 'I' });
    expect(directionLabel([0.7, -0.6, 0.1])).toBe('LA');
    expect(directionLabel([0, 0, 0])).toBe('');
    expect(directionLabel([NaN, 0, 0])).toBe('');
  });
  it('matches equivalent quaternion signs and distinguishes free rotation', () => {
    const views = [{ id: 'inferior', name: 'Inferior', rotationXyzw: [0, 0, 0, 1] as [number, number, number, number] }];
    expect(matchedStandardView({ ...camera, rotationXyzw: [0, 0, 0, -1] }, views)).toBe('inferior');
    expect(matchedStandardView({ ...camera, rotationXyzw: [0, 0, Math.SQRT1_2, Math.SQRT1_2] }, views)).toBe('free');
  });
  it('normalizes quaternions and falls back for zero or invalid inputs', () => {
    expect(normalizeQuaternion([0, 0, 0, 2])).toEqual([0, 0, 0, 1]);
    expect(normalizeQuaternion([0, 0, 0, 0])).toEqual([0, 0, 0, 1]);
    expect(normalizeQuaternion([NaN, 0, 0, 1])).toEqual([0, 0, 0, 1]);
  });
  it('composes rotation and transforms camera-local axes', () => {
    const quarterTurn = normalizeQuaternion([0, 0, 1, 1]);
    const twice = multiplyQuaternion(quarterTurn, quarterTurn);
    expect(rotateVector([1, 0, 0], twice)[0]).toBeCloseTo(-1);
    expect(rotateVector([1, 0, 0], quarterTurn)[1]).toBeCloseTo(1);
  });
  it('pans with scale in millimeters, including rotated camera axes', () => {
    expect(panCamera(camera, 20, 10, 400).targetMm).toEqual([0, 25, 30]);
    const rotated = panCamera({ ...camera, rotationXyzw: normalizeQuaternion([0, 0, 1, 1]) }, 20, 0, 400);
    expect(rotated.targetMm[0]).toBeCloseTo(10);
    expect(rotated.targetMm[1]).toBeCloseTo(10);
  });
  it('keeps stationary trackball rotation unchanged', () => {
    expect(orbitCamera(camera, [200, 200], [200, 200], 400, 400).rotationXyzw).toEqual(camera.rotationXyzw);
  });
  it('keeps repeated trackball rotations normalized', () => {
    let current = camera;
    for (let index = 0; index < 100; index++) current = orbitCamera(current, [200, 200], [250, 240], 400, 400);
    expect(Math.hypot(...current.rotationXyzw)).toBeCloseTo(1);
    expect(current.targetMm).toEqual(camera.targetMm);
  });
  it('zooms in both directions and clamps extremes', () => {
    expect(zoomCamera(camera, -100).orthoHeightMm).toBeLessThan(200);
    expect(zoomCamera(camera, 100).orthoHeightMm).toBeGreaterThan(200);
    expect(zoomCamera({ ...camera, orthoHeightMm: 5 }, -1000).orthoHeightMm).toBe(5);
    expect(zoomCamera({ ...camera, orthoHeightMm: 5000 }, 1000).orthoHeightMm).toBe(5000);
  });
});