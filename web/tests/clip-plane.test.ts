import { describe, expect, it } from 'vitest';
import { OrthographicCamera, Vector3 } from 'three';
import { planeAngles, planeNormal, planeRadius } from '../src/clip-plane';
import { overlayCamera } from '../src/glass-camera';
import { rotateVector } from '../src/camera';
import type { Camera } from '../src/protocol';

describe('patient-space glass geometry', () => {
  it('roundtrips arbitrary LPS normals and preserves unit length', () => {
    for (const azimuth of [-180, -90, 0, 37, 90, 180]) for (const tilt of [0, 12, 90, 155, 180]) {
      const normal = planeNormal(azimuth, tilt);
      const angles = planeAngles(normal);
      const result = planeNormal(angles.azimuth, angles.tilt);
      expect(Math.hypot(...normal)).toBeCloseTo(1, 12);
      normal.forEach((value, axis) => expect(result[axis]).toBeCloseTo(value, 12));
    }
  });
  it('sizes the glass from physical volume extent, not camera zoom', () => {
    expect(planeRadius({ dimensions: [4, 6, 8], spacing: [2, 1, 0.5] })).toBeCloseTo(Math.hypot(8, 6, 4) / 2);
  });
  it('matches CUDA screen-right/up and target under translated and rotated cameras', () => {
    for (const rotationXyzw of [[0, 0, 0, 1], [0.5, 0.5, 0.5, 0.5], [0, Math.SQRT1_2, Math.SQRT1_2, 0]] as Camera['rotationXyzw'][]) {
      const view: Camera = { targetMm: [41, -23, 7], rotationXyzw, orthoHeightMm: 200 };
      const camera = new OrthographicCamera();
      overlayCamera(camera, view, 1.5, 100);
      const center = new Vector3(...view.targetMm);
      const projected = center.clone().project(camera);
      expect(projected.x).toBeCloseTo(0, 12);
      expect(projected.y).toBeCloseTo(0, 12);
      const right = center.clone().addScaledVector(new Vector3(...rotateVector([1, 0, 0], rotationXyzw)), 75).project(camera);
      const up = center.clone().addScaledVector(new Vector3(...rotateVector([0, 1, 0], rotationXyzw)), 50).project(camera);
      expect(right.x).toBeCloseTo(0.5, 12);
      expect(right.y).toBeCloseTo(0, 12);
      expect(up.x).toBeCloseTo(0, 12);
      expect(up.y).toBeCloseTo(0.5, 12);
    }
  });
});