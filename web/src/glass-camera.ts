import { Vector3 } from 'three';
import type { OrthographicCamera } from 'three';
import type { Camera } from './protocol';

export function overlayCamera(camera: OrthographicCamera, view: Camera, aspect: number, radius: number): void {
  const halfHeight = view.orthoHeightMm / 2;
  camera.left = -halfHeight * aspect;
  camera.right = halfHeight * aspect;
  camera.top = halfHeight;
  camera.bottom = -halfHeight;
  camera.near = 0.01;
  camera.far = radius * 12 + view.orthoHeightMm * 4;
  camera.quaternion.fromArray(view.rotationXyzw).normalize();
  camera.position.copy(new Vector3(0, 0, radius * 4 + view.orthoHeightMm)
    .applyQuaternion(camera.quaternion)).add(new Vector3(...view.targetMm));
  camera.updateProjectionMatrix();
  camera.updateMatrixWorld(true);
}