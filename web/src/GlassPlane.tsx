import { useEffect, useRef } from 'react';
import { ArrowHelper, BufferGeometry, DoubleSide, EdgesGeometry, Group, LineBasicMaterial, LineSegments,
  Mesh, MeshBasicMaterial, Object3D, OrthographicCamera, PlaneGeometry, Scene, Vector3, WebGLRenderer } from 'three';
import { TransformControls } from 'three/addons/controls/TransformControls.js';
import { planeRadius } from './clip-plane';
import { overlayCamera } from './glass-camera';
import type { ClipPlane, FrameMeta, Vec3 } from './protocol';
import type { WorkstationClient, WorkstationState } from './workstation';

class GlassScene {
  private renderer: WebGLRenderer;
  private scene = new Scene();
  private camera = new OrthographicCamera();
  private glass = new Group();
  private proxy = new Object3D();
  private controls: TransformControls[] = [];
  private activeControl: TransformControls | null = null;
  private hoveredControl: TransformControls | null = null;
  private arrow = new ArrowHelper(new Vector3(0, 0, 1), new Vector3(), 1, 0xf3ce76);
  private normal = new Vector3(0, 0, 1);
  private center = new Vector3();
  private radius = 1;
  private observer: ResizeObserver;
  private lastState: WorkstationState | null = null;
  private frame: FrameMeta | null = null;
  private disposed = false;
  private cancelling = false;
  private planePointer: number | null = null;
  private lastPointerWasPlane = false;

  constructor(private host: HTMLDivElement, private client: WorkstationClient) {
    this.renderer = new WebGLRenderer({ alpha: true, antialias: true, preserveDrawingBuffer: true });
    this.renderer.setClearColor(0, 0);
    this.renderer.setPixelRatio(Math.min(devicePixelRatio || 1, 2));
    this.renderer.domElement.setAttribute('aria-label', 'Glass clipping plane');
    host.append(this.renderer.domElement);
    const geometry = new PlaneGeometry(1, 1);
    this.glass.add(new Mesh(geometry, new MeshBasicMaterial({ color: 0x8be8e2, transparent: true,
      opacity: 0.10, side: DoubleSide, depthTest: false, depthWrite: false })));
    this.glass.add(new LineSegments(new EdgesGeometry(geometry), new LineBasicMaterial({ color: 0x82ded9,
      transparent: true, opacity: 0.9, depthTest: false })));
    const cross = new BufferGeometry().setFromPoints([
      new Vector3(-0.06, 0, 0), new Vector3(0.06, 0, 0),
      new Vector3(0, -0.06, 0), new Vector3(0, 0.06, 0)
    ]);
    this.glass.add(new LineSegments(cross, new LineBasicMaterial({ color: 0xc4faf5, depthTest: false })));
    this.scene.add(this.glass, this.proxy, this.arrow);
    for (const mode of ['translate', 'rotate'] as const) {
      const control = new TransformControls(this.camera, this.renderer.domElement);
      control.disconnect();
      control.setMode(mode);
      control.setSpace('local');
      control.setSize(0.85);
      control.showX = control.showY = mode === 'rotate';
      control.showZ = mode === 'translate';
      control.showE = mode === 'rotate';
      control.showXY = control.showYZ = control.showXZ = control.showXYZE = false;
      control.attach(this.proxy);
      this.controls.push(control);
      this.scene.add(control.getHelper());
      control.addEventListener('change', this.draw);
      control.addEventListener('objectChange', this.change);
    }
    this.renderer.domElement.addEventListener('pointercancel', this.cancel);
    this.renderer.domElement.addEventListener('lostpointercapture', this.cancel);
    host.addEventListener('pointerdown', this.routePointerDown);
    host.addEventListener('pointermove', this.routePointerMove);
    host.addEventListener('pointerup', this.routePointerUp);
    host.addEventListener('wheel', this.routeWheel, { passive: false });
    host.addEventListener('dblclick', this.routeDoubleClick);
    this.observer = new ResizeObserver(() => {
      if (this.lastState) this.update(this.lastState, this.frame);
    });
    this.observer.observe(host);
  }

  private draw = (): void => {
    if (this.disposed) return;
    const control = this.activeControl ?? this.hoveredControl;
    this.renderer.domElement.style.cursor = this.activeControl ? 'grabbing' : control ? 'crosshair' : 'grab';
    this.renderer.domElement.title = control ?
      control.mode === 'translate' ? 'Move clipping plane along its normal' : 'Rotate clipping plane' : 'Rotate volume';
    this.renderer.render(this.scene, this.camera);
  };

  private pointer(event: PointerEvent): PointerEvent {
    const bounds = this.renderer.domElement.getBoundingClientRect();
    return { x: (event.clientX - bounds.left) / bounds.width * 2 - 1,
      y: -(event.clientY - bounds.top) / bounds.height * 2 + 1, button: event.button } as PointerEvent;
  }

  private hover(event: PointerEvent): TransformControls | null {
    const pointer = this.pointer(event);
    this.scene.updateMatrixWorld(true);
    for (const control of this.controls) {
      if (control.enabled) control.pointerHover(pointer);
      else control.axis = null;
    }
    const selected = this.controls.find(control => control.enabled && control.axis !== null) ?? null;
    for (const control of this.controls) if (control !== selected) control.axis = null;
    this.hoveredControl = selected;
    this.draw();
    return selected;
  }

  private routePointerDown = (event: PointerEvent): void => {
    if (this.planePointer !== null) { event.stopPropagation(); return; }
    this.lastPointerWasPlane = false;
    if (!event.isPrimary || event.button !== 0) return;
    const control = this.hover(event);
    if (!control) return;
    control.pointerDown(this.pointer(event));
    if (!control.dragging) return;
    this.activeControl = control;
    this.lastPointerWasPlane = true;
    this.planePointer = event.pointerId;
    this.client.beginInteraction();
    this.renderer.domElement.setPointerCapture(event.pointerId);
    this.host.dataset.dragging = 'true';
    this.host.dataset.dragAction = control.mode;
    event.preventDefault();
    event.stopPropagation();
    this.host.parentElement?.focus({ preventScroll: true });
  };

  private routePointerMove = (event: PointerEvent): void => {
    if (event.pointerId === this.planePointer && this.activeControl) {
      this.activeControl.pointerMove(this.pointer(event));
      event.stopPropagation();
    } else if (this.planePointer === null && event.buttons === 0 && event.isPrimary) this.hover(event);
  };

  private routePointerUp = (event: PointerEvent): void => {
    if (event.pointerId !== this.planePointer) return;
    event.stopPropagation();
    this.cancel();
  };

  private routeWheel = (event: WheelEvent): void => {
    if (!this.activeControl) return;
    event.preventDefault();
    event.stopPropagation();
  };

  private routeDoubleClick = (event: MouseEvent): void => {
    if (this.lastPointerWasPlane) event.stopPropagation();
  };

  private change = (): void => {
    const plane = this.client.getSnapshot().clipPlane;
    if (!plane || !this.activeControl?.dragging) return;
    const normal = new Vector3(0, 0, 1).applyQuaternion(this.proxy.quaternion).normalize();
    const offsetMm = Math.max(-this.radius, Math.min(this.radius,
      normal.dot(this.proxy.position.clone().sub(this.center))));
    this.client.setClipPlane({ ...plane, normal: normal.toArray() as Vec3, offsetMm });
  };

  private finish = (): void => {
    if (this.cancelling) return;
    this.client.finishInteraction();
    if (this.lastState) this.update(this.client.getSnapshot(), this.frame);
  };

  private cancel = (event?: PointerEvent): void => {
    if (event && event.pointerId !== this.planePointer) return;
    if (event) event.stopPropagation();
    if (this.activeControl && !this.cancelling) {
      this.cancelling = true;
      try { this.activeControl.pointerUp(null); }
      finally { this.cancelling = false; }
      const pointer = this.planePointer;
      this.planePointer = null;
      this.activeControl = null;
      this.hoveredControl = null;
      this.host.dataset.dragging = 'false';
      if (pointer !== null && this.renderer.domElement.hasPointerCapture(pointer))
        this.renderer.domElement.releasePointerCapture(pointer);
      this.finish();
    }
  };

  private pose(object: Object3D, plane: ClipPlane): void {
    const normal = new Vector3(...plane.normal).normalize();
    object.position.copy(this.center).addScaledVector(normal, plane.offsetMm);
    object.quaternion.setFromUnitVectors(this.normal, normal);
  }

  update(state: WorkstationState, frame = state.frame): void {
    this.lastState = state;
    this.frame = frame;
    const width = this.host.clientWidth;
    const height = this.host.clientHeight;
    if (!width || !height) return;
    this.radius = planeRadius(state.session?.volume);
    this.center.fromArray(state.session?.initial?.camera.targetMm ?? [0, 0, 0]);
    const displayed = frame?.clipPlane;
    const visible = state.phase === 'ready' && !!frame?.camera && displayed?.enabled === true;
    this.glass.visible = this.arrow.visible = visible;
    if (frame?.camera) overlayCamera(this.camera, frame.camera, frame.width / frame.height, this.radius);
    const editable = visible && state.connection === 'online' && state.clipPlane?.enabled === true;
    if (!editable && this.activeControl) this.cancel();
    for (const control of this.controls) {
      control.enabled = editable;
      control.getHelper().visible = editable;
    }
    this.host.style.pointerEvents = editable ? 'auto' : 'none';
    this.host.dataset.seq = String(frame?.seq ?? '');
    this.host.dataset.plane = displayed ? JSON.stringify(displayed) : '';
    this.host.dataset.editing = String(editable);
    this.renderer.setSize(width, height, false);
    if (visible && displayed) {
      this.pose(this.glass, displayed);
      const right = new Vector3(1, 0, 0).applyQuaternion(this.camera.quaternion);
      const up = new Vector3(0, 1, 0).applyQuaternion(this.camera.quaternion);
      const planeRight = new Vector3(1, 0, 0).applyQuaternion(this.glass.quaternion);
      const planeUp = new Vector3(0, 1, 0).applyQuaternion(this.glass.quaternion);
      const horizontal = this.camera.right - this.camera.left;
      const vertical = this.camera.top - this.camera.bottom;
      const size = Math.min(this.radius * 2.15,
        horizontal * 0.86 / Math.max(0.15, Math.abs(right.dot(planeRight)) + Math.abs(right.dot(planeUp))),
        vertical * 0.86 / Math.max(0.15, Math.abs(up.dot(planeRight)) + Math.abs(up.dot(planeUp))));
      this.glass.scale.setScalar(size);
      this.arrow.position.copy(this.glass.position);
      const direction = new Vector3(...displayed.normal).multiplyScalar(displayed.keepPositive ? 1 : -1);
      this.arrow.setDirection(direction);
      const length = Math.min(this.radius * 0.55, Math.min(horizontal, vertical) * 0.3);
      this.arrow.setLength(length, length * 0.18, length * 0.11);
    }
    if (editable && state.clipPlane && !this.activeControl) {
      this.pose(this.proxy, state.clipPlane);
    }
    this.draw();
  }

  dispose(): void {
    this.disposed = true;
    this.observer.disconnect();
    this.controls.forEach(control => control.dispose());
    this.renderer.domElement.removeEventListener('pointercancel', this.cancel);
    this.renderer.domElement.removeEventListener('lostpointercapture', this.cancel);
    this.host.removeEventListener('pointerdown', this.routePointerDown);
    this.host.removeEventListener('pointermove', this.routePointerMove);
    this.host.removeEventListener('pointerup', this.routePointerUp);
    this.host.removeEventListener('wheel', this.routeWheel);
    this.host.removeEventListener('dblclick', this.routeDoubleClick);
    for (const group of [this.glass, this.arrow]) group.traverse(object => {
      if (object instanceof Mesh || object instanceof LineSegments) {
        object.geometry.dispose();
        const materials = Array.isArray(object.material) ? object.material : [object.material];
        materials.forEach(material => material.dispose());
      }
    });
    this.renderer.dispose();
    this.renderer.domElement.remove();
  }
}

export function GlassPlane({ client, state }: {
  client: WorkstationClient; state: WorkstationState;
}) {
  const host = useRef<HTMLDivElement>(null);
  const scene = useRef<GlassScene | null>(null);
  useEffect(() => {
    if (!host.current) return;
    let glass: GlassScene;
    try { glass = new GlassScene(host.current, client); }
    catch { host.current.dataset.failed = 'true'; return; }
    scene.current = glass;
    const unsubscribe = client.subscribePresented(meta => glass.update(client.getSnapshot(), meta));
    glass.update(client.getSnapshot());
    return () => { unsubscribe(); scene.current = null; glass.dispose(); };
  }, [client]);
  useEffect(() => {
    scene.current?.update(state);
  }, [state]);
  return <><div ref={host} className="glass-overlay" data-testid="glass-overlay" />
    <span className="glass-error" role="alert">Glass overlay unavailable. WebGL is required.</span></>;
}