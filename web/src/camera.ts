import type { Camera, Quaternion, StandardView, Vec3 } from './protocol';

export const MIN_ORTHO_HEIGHT = 5;
export const MAX_ORTHO_HEIGHT = 5000;

export function clamp(value: number, minimum: number, maximum: number): number {
  return Math.min(maximum, Math.max(minimum, value));
}

export function normalizeQuaternion(rotation: Quaternion): Quaternion {
  const length = Math.hypot(...rotation);
  if (!Number.isFinite(length) || length < 1e-10) return [0, 0, 0, 1];
  return rotation.map(value => value / length) as Quaternion;
}

export function multiplyQuaternion(left: Quaternion, right: Quaternion): Quaternion {
  const [leftX, leftY, leftZ, leftW] = left;
  const [rightX, rightY, rightZ, rightW] = right;
  return normalizeQuaternion([
    leftW * rightX + leftX * rightW + leftY * rightZ - leftZ * rightY,
    leftW * rightY - leftX * rightZ + leftY * rightW + leftZ * rightX,
    leftW * rightZ + leftX * rightY - leftY * rightX + leftZ * rightW,
    leftW * rightW - leftX * rightX - leftY * rightY - leftZ * rightZ
  ]);
}

export function rotateVector(vector: Vec3, rotation: Quaternion): Vec3 {
  const [quatX, quatY, quatZ, quatW] = normalizeQuaternion(rotation);
  const [vectorX, vectorY, vectorZ] = vector;
  const crossX = 2 * (quatY * vectorZ - quatZ * vectorY);
  const crossY = 2 * (quatZ * vectorX - quatX * vectorZ);
  const crossZ = 2 * (quatX * vectorY - quatY * vectorX);
  return [
    vectorX + quatW * crossX + quatY * crossZ - quatZ * crossY,
    vectorY + quatW * crossY + quatZ * crossX - quatX * crossZ,
    vectorZ + quatW * crossZ + quatX * crossY - quatY * crossX
  ];
}

function trackballPoint(point: [number, number], width: number, height: number): Vec3 {
  const diameter = Math.max(1, Math.min(width, height));
  const positionX = (2 * point[0] - width) / diameter;
  const positionY = (height - 2 * point[1]) / diameter;
  const distanceSquared = positionX * positionX + positionY * positionY;
  const positionZ = distanceSquared <= 0.5 ? Math.sqrt(1 - distanceSquared) : 0.5 / Math.sqrt(distanceSquared);
  const length = Math.hypot(positionX, positionY, positionZ);
  return [positionX / length, positionY / length, positionZ / length];
}

export function orbitCamera(camera: Camera, from: [number, number], to: [number, number],
  width: number, height: number): Camera {
  const start = trackballPoint(from, width, height);
  const end = trackballPoint(to, width, height);
  const delta: Quaternion = [
    start[1] * end[2] - start[2] * end[1],
    start[2] * end[0] - start[0] * end[2],
    start[0] * end[1] - start[1] * end[0],
    1 + start[0] * end[0] + start[1] * end[1] + start[2] * end[2]
  ];
  return { ...camera, rotationXyzw: multiplyQuaternion(camera.rotationXyzw, delta) };
}

export function panCamera(camera: Camera, deltaX: number, deltaY: number, viewportHeight: number): Camera {
  const mmPerPixel = camera.orthoHeightMm / Math.max(1, viewportHeight);
  const offset = rotateVector([-deltaX * mmPerPixel, deltaY * mmPerPixel, 0], camera.rotationXyzw);
  return { ...camera, targetMm: camera.targetMm.map((value, index) => value + (offset[index] ?? 0)) as Vec3 };
}

export function zoomCamera(camera: Camera, delta: number): Camera {
  return { ...camera, orthoHeightMm: clamp(camera.orthoHeightMm * Math.exp(clamp(delta, -1000, 1000) * 0.0015),
    MIN_ORTHO_HEIGHT, MAX_ORTHO_HEIGHT) };
}

export function directionLabel(direction: Vec3): string {
  const positive = ['L', 'P', 'S'];
  const negative = ['R', 'A', 'I'];
  const length = Math.hypot(...direction);
  if (!Number.isFinite(length) || length < 1e-8) return '';
  return direction.map((value, axis) => ({ value, axis }))
    .filter(({ value }) => Math.abs(value) / length >= 0.2)
    .sort((first, second) => Math.abs(second.value) - Math.abs(first.value))
    .map(({ value, axis }) => value >= 0 ? positive[axis] : negative[axis]).join('');
}

export function orientationLabels(rotation: Quaternion) {
  const right = rotateVector([1, 0, 0], rotation);
  const up = rotateVector([0, 1, 0], rotation);
  return { left: directionLabel(right.map(value => -value) as Vec3), right: directionLabel(right),
    top: directionLabel(up), bottom: directionLabel(up.map(value => -value) as Vec3) };
}

export function matchedStandardView(camera: Camera | null, views: StandardView[]): string {
  if (!camera) return 'free';
  const rotation = normalizeQuaternion(camera.rotationXyzw);
  return views.find(view => {
    const dot = rotation.reduce((sum, value, index) => sum + value * view.rotationXyzw[index]!, 0);
    return Math.abs(dot) > 0.999999;
  })?.id ?? 'free';
}