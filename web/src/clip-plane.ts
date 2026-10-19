import type { Session, Vec3 } from './protocol';

export type InteractionMode = 'rotate' | 'pan';

export function planeNormal(azimuth: number, tilt: number): Vec3 {
  const horizontal = azimuth * Math.PI / 180;
  const vertical = tilt * Math.PI / 180;
  return [Math.sin(vertical) * Math.cos(horizontal), Math.sin(vertical) * Math.sin(horizontal), Math.cos(vertical)];
}

export function planeAngles(normal: Vec3): { azimuth: number; tilt: number } {
  return { azimuth: Math.atan2(normal[1], normal[0]) * 180 / Math.PI,
    tilt: Math.atan2(Math.hypot(normal[0], normal[1]), normal[2]) * 180 / Math.PI };
}

export function planeRadius(volume: Session['volume']): number {
  return volume ? Math.hypot(...volume.dimensions.map((size, axis) => size * volume.spacing[axis]!)) / 2 : 1;
}
