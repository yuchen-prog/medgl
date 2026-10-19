import * as api from './api';
import { FramePresenter } from './frames';
import { applyAppearancePreset, defaultClipPlane, fullCropBox, parseClipPlane, parseCropBox, parseStyle, parseTransferFunction } from './protocol';
import type { AppearancePreset, Camera, ClipPlane, CropBox, FrameMeta, Quality, RenderRequest, RenderStyle, Series, Session, TransferPoint } from './protocol';
import { parseSavedPresets, PRESET_STORAGE_KEY } from './transfer';
import type { SavedPreset } from './transfer';

export interface WorkstationState {
  phase: 'idle' | 'catalog' | 'loading' | 'ready' | 'closing' | 'error';
  connection: 'offline' | 'connecting' | 'online' | 'reconnecting';
  series: Series[];
  selectedId: string;
  activeSeries: Series | null;
  session: Session | null;
  camera: Camera | null;
  style: RenderStyle | null;
  cropBox: CropBox | null;
  clipPlane: ClipPlane | null;
  frame: FrameMeta | null;
  rendering: boolean;
  error: string | null;
  catalogError: string | null;
  loadWarning: string | null;
  presets: AppearancePreset[];
  presetError: string | null;
  customPresets: SavedPreset[];
  customPresetId: string | null;
  storageError: string | null;
  width: number;
  height: number;
}

const initialState: WorkstationState = {
  phase: 'idle', connection: 'offline', series: [], selectedId: 'phantom', activeSeries: null,
  session: null, camera: null, style: null, cropBox: null, clipPlane: null, frame: null, rendering: false,
  error: null, catalogError: null, loadWarning: null, width: 1, height: 1, presets: [], presetError: null,
  customPresets: [], customPresetId: null, storageError: null
};

const errorText = (error: unknown): string => error instanceof Error ? error.message : 'Unexpected service error';

export class WorkstationClient {
  private state = initialState;
  private listeners = new Set<() => void>();
  private presentedListeners = new Set<(meta: FrameMeta) => void>();
  private live = false;
  private generation = 0;
  private catalogGeneration = 0;
  private operations: Promise<void> = Promise.resolve();
  private ownedSessionId: string | null = null;
  private socket: WebSocket | null = null;
  private presenter: FramePresenter | null = null;
  private canvas: HTMLCanvasElement | null = null;
  private previewTimer: ReturnType<typeof setTimeout> | undefined;
  private finalTimer: ReturnType<typeof setTimeout> | undefined;
  private reconnectTimer: ReturnType<typeof setTimeout> | undefined;
  private connectTimer: ReturnType<typeof setTimeout> | undefined;
  private frameTimer: ReturnType<typeof setTimeout> | undefined;
  private lastSentAt = 0;
  private reconnectAttempts = 0;
  private frameFailures = 0;
  private requestTimes = new Map<number, number>();
  lastLatencyMs: number | null = null;

  constructor(private readonly sequence: { current: number }) {}

  getSnapshot = (): WorkstationState => this.state;
  subscribe = (listener: () => void): (() => void) => {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  };

  subscribePresented = (listener: (meta: FrameMeta) => void): (() => void) => {
    this.presentedListeners.add(listener);
    return () => this.presentedListeners.delete(listener);
  };

  private patch(values: Partial<WorkstationState>): void {
    this.state = { ...this.state, ...values };
    if (this.live) this.listeners.forEach(listener => listener());
  }

  async start(): Promise<void> {
    this.live = true;
    await this.operations;
    if (!this.live) return;
    this.restoreCustomPresets();
    await Promise.all([this.refreshPresets(), this.refreshCatalog(true)]);
  }

  async refreshPresets(): Promise<void> {
    this.patch({ presetError: null });
    try {
      const presets = await api.appearancePresets();
      if (this.live) this.patch({ presets });
    } catch (error) {
      if (this.live) this.patch({ presetError: errorText(error) });
    }
  }

  applyPreset(presetId: string): void {
    if (!this.state.style || this.state.phase !== 'ready') return;
    const preset = this.state.presets.find(item => item.id === presetId);
    if (preset) {
      this.setStyle(applyAppearancePreset(this.state.style, preset));
      this.patch({ customPresetId: null });
    } else {
      const saved = this.state.customPresets.find(item => item.id === presetId);
      if (saved && this.state.session?.transferFunctionSupported && this.state.presets.some(item => item.id === saved.style.presetId)) {
        this.setStyle({ ...structuredClone(saved.style), backgroundRgb: [...this.state.style.backgroundRgb] });
        this.patch({ customPresetId: saved.id });
      }
    }
  }

  private restoreCustomPresets(): void {
    try {
      if (typeof localStorage === 'undefined') return;
      const saved = localStorage.getItem(PRESET_STORAGE_KEY);
      this.patch({ customPresets: saved ? parseSavedPresets(saved) : [], storageError: null });
    } catch { this.patch({ storageError: 'Local presets could not be read. Stored data was not changed.' }); }
  }

  private storeCustomPresets(presets: SavedPreset[]): boolean {
    try {
      localStorage.setItem(PRESET_STORAGE_KEY, JSON.stringify({ version: 1, presets }));
      this.patch({ customPresets: presets, storageError: null });
      return true;
    } catch {
      this.patch({ storageError: 'Local presets could not be saved. Check browser storage permissions or available space.' });
      return false;
    }
  }

  saveCustomPreset(name: string): void {
    const style = this.state.style;
    if (this.state.phase !== 'ready' || !this.state.session?.transferFunctionSupported || !style?.transferFunction) return;
    if (!name.trim() || name.trim().length > 64 || this.state.customPresets.length >= 32) {
      this.patch({ storageError: 'Enter a name (1-64 characters). Up to 32 local presets can be saved.' });
      return;
    }
    const saved: SavedPreset = { id: `local-${crypto.randomUUID()}`, name: name.trim(),
      style: { ...structuredClone(style), backgroundRgb: [0, 0, 0] } };
    if (this.storeCustomPresets([...this.state.customPresets, saved])) this.patch({ customPresetId: saved.id });
  }

  deleteCustomPreset(): void {
    const id = this.state.customPresetId;
    if (!id) return;
    if (this.storeCustomPresets(this.state.customPresets.filter(preset => preset.id !== id)))
      this.patch({ customPresetId: null });
  }

  setTransferFunction(points: TransferPoint[]): void {
    if (!this.state.session?.transferFunctionSupported || !this.state.style) return;
    this.setStyle({ ...this.state.style, transferFunction: parseTransferFunction(points) });
  }

  resetTransferFunction(): void {
    const preset = this.state.presets.find(item => item.id === this.state.style?.presetId);
    if (preset?.transferFunction) this.setTransferFunction(preset.transferFunction);
  }

  async refreshCatalog(autoLoad = false): Promise<void> {
    const generation = this.generation;
    const request = ++this.catalogGeneration;
    if (this.state.series.length === 0) this.patch({ phase: 'catalog' });
    this.patch({ catalogError: null });
    try {
      const series = await api.catalog();
      if (!this.live || request !== this.catalogGeneration || generation !== this.generation) return;
      const phantom = series.find(item => item.id === 'phantom' && item.supported);
      const preferred = series.find(item => item.id !== 'phantom' && item.supported) ?? phantom;
      const selectedId = series.some(item => item.id === this.state.selectedId) ? this.state.selectedId : (phantom?.id ?? '');
      this.patch({ series, selectedId, phase: this.state.phase === 'catalog' ? 'idle' : this.state.phase });
      if (autoLoad && preferred && !this.ownedSessionId && this.generation === generation)
        void this.queueSeriesLoad(preferred, preferred.id === 'phantom' ? undefined : phantom);
    } catch (error) {
      if (this.live && request === this.catalogGeneration && generation === this.generation)
        this.patch({ catalogError: errorText(error), phase: this.state.phase === 'catalog' ? 'idle' : this.state.phase });
    }
  }

  selectSeries(selectedId: string): void {
    this.patch({ selectedId });
  }

  loadSeries(seriesId = this.state.selectedId): Promise<void> {
    const series = this.state.series.find(item => item.id === seriesId);
    if (!series?.supported || !this.live) return Promise.resolve();
    return this.queueSeriesLoad(series);
  }

  private queueSeriesLoad(series: Series, fallback?: Series): Promise<void> {
    const generation = ++this.generation;
    this.disconnect();
    this.patch({ phase: this.ownedSessionId ? 'closing' : 'loading', selectedId: series.id,
      loadWarning: null, error: null, rendering: false });
    this.operations = this.operations.then(async () => {
      try {
        let fallbackReason: string | null = null;
        for (const candidate of fallback ? [series, fallback] : [series]) {
          if (!this.live || generation !== this.generation) return;
          await this.releaseSession();
          if (!this.live || generation !== this.generation) return;
          this.lastLatencyMs = null;
          this.patch({ phase: 'loading', session: null, camera: null, style: null, cropBox: null, clipPlane: null,
            customPresetId: null, frame: null, activeSeries: candidate, selectedId: candidate.id,
            loadWarning: fallbackReason === null ? null :
              `CT series could not be loaded: ${fallbackReason}. Showing Synthetic phantom instead.` });
          try {
            const created = await api.createSession(candidate.id);
            this.ownedSessionId = created.sessionId;
            if (!this.live || generation !== this.generation) {
              await this.releaseSession();
              return;
            }
            this.patch({ session: created });
            const session = created.state === 'ready' ? created : await api.waitUntilReady(created.sessionId,
              () => !this.live || generation !== this.generation);
            if (!this.live || generation !== this.generation) {
              await this.releaseSession();
              return;
            }
            if (!session.initial) throw new Error('Missing initial view');
            this.patch({ phase: 'ready', session, camera: structuredClone(session.initial.camera),
              style: structuredClone(session.initial.style),
              cropBox: structuredClone(session.initial.cropBox ?? fullCropBox()),
              clipPlane: structuredClone(session.initial.clipPlane ?? defaultClipPlane()), error: null });
            this.reconnectAttempts = 0;
            this.frameFailures = 0;
            this.connect();
            return;
          } catch (error) {
            if (!this.live || generation !== this.generation) return;
            if (!fallback || candidate !== series) throw error;
            fallbackReason = errorText(error);
            this.patch({ phase: this.ownedSessionId ? 'closing' : 'loading', rendering: false });
          }
        }
      } catch (error) {
        if (this.live && generation === this.generation) this.patch({ phase: 'error', error: errorText(error), rendering: false });
      }
    });
    return this.operations;
  }

  private async releaseSession(): Promise<void> {
    if (!this.ownedSessionId) return;
    const sessionId = this.ownedSessionId;
    await api.closeSession(sessionId, !this.live);
    if (this.ownedSessionId === sessionId) this.ownedSessionId = null;
  }

  private cancelRenders(): void {
    clearTimeout(this.previewTimer);
    clearTimeout(this.finalTimer);
    clearTimeout(this.frameTimer);
    this.previewTimer = undefined;
    this.finalTimer = undefined;
    this.frameTimer = undefined;
    this.requestTimes.clear();
  }

  private disconnect(): void {
    this.cancelRenders();
    clearTimeout(this.reconnectTimer);
    clearTimeout(this.connectTimer);
    this.presenter?.dispose();
    this.presenter = null;
    const socket = this.socket;
    this.socket = null;
    socket?.close(1000, 'View released');
    this.patch({ connection: 'offline', rendering: false });
  }

  private connect(): void {
    if (!this.live || this.state.phase !== 'ready' || !this.state.session?.sceneRevision) return;
    const session = this.state.session;
    const socket = new WebSocket(api.streamUrl(session.sessionId));
    socket.binaryType = 'arraybuffer';
    this.socket = socket;
    this.patch({ connection: this.reconnectAttempts ? 'reconnecting' : 'connecting', error: null });
    if (!this.presenter) {
      this.presenter = new FramePresenter(() => {
        const current = this.state.session;
        return current?.sceneRevision ? { sessionId: current.sessionId, revision: current.sceneRevision,
          width: this.state.width, height: this.state.height, issuedSeq: this.sequence.current } : null;
      }, () => this.canvas, meta => this.onFrame(meta));
    }
    this.connectTimer = setTimeout(() => { if (this.socket === socket) socket.close(); }, 8000);
    socket.onopen = () => {
      if (this.socket !== socket || !this.live) { socket.close(); return; }
      clearTimeout(this.connectTimer);
      this.patch({ connection: 'online', error: null });
      this.sendRender('final');
    };
    socket.onmessage = event => {
      if (this.socket !== socket || !this.live) return;
      if (!(event.data instanceof ArrayBuffer)) {
        this.frameError(new Error('Expected a binary image packet'));
        return;
      }
      const presenter = this.presenter;
      void presenter?.accept(event.data).catch(error => {
        if (this.socket === socket && this.presenter === presenter && this.live) this.frameError(error);
      });
    };
    socket.onerror = () => { if (this.socket === socket) this.patch({ error: 'Render stream connection failed' }); };
    socket.onclose = () => {
      if (this.socket !== socket || !this.live) return;
      this.socket = null;
      this.cancelRenders();
      clearTimeout(this.connectTimer);
      if (this.state.phase !== 'ready') return;
      if (this.reconnectAttempts < 3) {
        this.reconnectAttempts += 1;
        this.patch({ connection: 'reconnecting', rendering: false, error: null });
        this.reconnectTimer = setTimeout(() => this.connect(), 500 * 2 ** (this.reconnectAttempts - 1));
      } else {
        this.patch({ connection: 'offline', rendering: false, error: 'Render stream disconnected. Reconnect to retry.' });
      }
    };
  }

  private frameError(error: unknown): void {
    if (!this.live) return;
    this.frameFailures += 1;
    this.patch({ error: errorText(error), rendering: false });
    clearTimeout(this.frameTimer);
    if (this.frameFailures >= 3) {
      this.disconnect();
      this.patch({ error: `Invalid image stream: ${errorText(error)}. Reconnect after checking the service.` });
    }
  }

  private onFrame(meta: FrameMeta): void {
    this.frameFailures = 0;
    const sentAt = this.requestTimes.get(meta.seq);
    this.lastLatencyMs = sentAt === undefined ? null : Math.round(performance.now() - sentAt);
    for (const sequence of this.requestTimes.keys()) if (sequence <= meta.seq) this.requestTimes.delete(sequence);
    const finished = meta.quality === 'final' && meta.seq === this.sequence.current &&
      this.previewTimer === undefined && this.finalTimer === undefined;
    if (finished) clearTimeout(this.frameTimer);
    this.presentedListeners.forEach(listener => listener(meta));
    this.patch({ frame: meta, error: null, rendering: !finished && this.state.rendering });
  }

  reconnect(): void {
    this.disconnect();
    this.reconnectAttempts = 0;
    this.frameFailures = 0;
    this.connect();
  }

  setCanvas(canvas: HTMLCanvasElement | null): void {
    this.canvas = canvas;
  }

  resize(width: number, height: number): void {
    if (width === this.state.width && height === this.state.height) return;
    this.patch({ width, height, frame: null });
    this.scheduleRender();
  }

  setCamera(camera: Camera): void {
    if (this.state.phase !== 'ready') return;
    this.patch({ camera });
    this.scheduleRender();
  }

  setStyle(style: RenderStyle): void {
    if (this.state.phase !== 'ready') return;
    this.patch({ style: structuredClone(parseStyle(style)) });
    this.scheduleRender();
  }

  setCropBox(cropBox: CropBox): void {
    if (this.state.phase !== 'ready' || !this.state.session?.cropBoxSupported) return;
    this.patch({ cropBox: parseCropBox(cropBox) });
    this.scheduleRender();
  }

  resetCropBox(): void {
    this.setCropBox(fullCropBox());
  }

  setClipPlane(clipPlane: ClipPlane): void {
    if (this.state.phase !== 'ready' || !this.state.session?.clipPlaneSupported) return;
    this.patch({ clipPlane: parseClipPlane(clipPlane) });
    this.scheduleRender();
  }

  resetClipPlane(): void {
    this.setClipPlane(defaultClipPlane());
  }

  resetCamera(): void {
    if (this.state.session?.initial) this.setCamera(structuredClone(this.state.session.initial.camera));
  }

  setStandardView(viewId: string): void {
    const initial = this.state.session?.initial;
    const view = this.state.session?.standardViews?.find(item => item.id === viewId);
    if (initial && view && this.state.phase === 'ready') {
      this.setCamera({ ...structuredClone(initial.camera), rotationXyzw: [...view.rotationXyzw] });
    }
  }

  finishInteraction(): void {
    if (this.state.phase !== 'ready' || this.state.connection !== 'online') return;
    clearTimeout(this.finalTimer);
    this.finalTimer = setTimeout(() => {
      this.finalTimer = undefined;
      this.sendRender('final');
    }, 200);
  }

  private scheduleRender(): void {
    if (this.state.phase !== 'ready' || this.state.connection !== 'online') return;
    this.patch({ rendering: true });
    if (this.previewTimer === undefined) {
      this.previewTimer = setTimeout(() => {
        this.previewTimer = undefined;
        this.sendRender('preview');
      }, Math.max(0, 34 - (performance.now() - this.lastSentAt)));
    }
    this.finishInteraction();
  }

  private sendRender(quality: Quality): void {
    const { session, camera, style, width, height } = this.state;
    const socket = this.socket;
    if (!this.live || !socket || socket.readyState !== WebSocket.OPEN || !session?.sceneRevision ||
        !camera || !style || width < 2 || height < 2) return;
    if (socket.bufferedAmount > 256 * 1024) {
      this.patch({ rendering: false, error: 'Render stream is congested. Reconnect to retry.' });
      return;
    }
    if (this.sequence.current >= Number.MAX_SAFE_INTEGER) {
      this.disconnect();
      this.patch({ error: 'Render sequence exhausted. Reload this page.' });
      return;
    }
    if (quality === 'final') {
      clearTimeout(this.previewTimer);
      this.previewTimer = undefined;
    }
    const seq = ++this.sequence.current;
    const outgoingStyle = { ...style };
    if (!session.transferFunctionSupported) delete outgoingStyle.transferFunction;
    const request: RenderRequest = { type: 'render', seq, sceneRevision: session.sceneRevision,
      quality, width, height, camera, style: outgoingStyle,
      ...(session.cropBoxSupported && this.state.cropBox ? { cropBox: this.state.cropBox } : {}),
      ...(session.clipPlaneSupported && this.state.clipPlane ? { clipPlane: this.state.clipPlane } : {}) };
    try {
      socket.send(JSON.stringify(request));
      this.lastSentAt = performance.now();
      this.requestTimes.set(seq, this.lastSentAt);
      if (this.requestTimes.size > 100) this.requestTimes.delete(this.requestTimes.keys().next().value ?? 0);
      this.patch({ rendering: true, error: null });
      clearTimeout(this.frameTimer);
      this.frameTimer = setTimeout(() => this.patch({ rendering: false, error: 'No image received within 12 seconds' }), 12000);
    } catch (error) {
      this.patch({ error: errorText(error), rendering: false });
    }
  }

  async downloadFinal(): Promise<Blob> {
    const { session, frame } = this.state;
    if (!session || !frame || frame.quality !== 'final' || this.state.rendering || frame.seq !== this.sequence.current) {
      throw new Error('Wait for the current final frame');
    }
    const response = await api.request(`${api.sessionPath(session.sessionId)}/frame?seq=${frame.seq}&revision=${frame.sceneRevision}`);
    if (!response.headers.get('content-type')?.includes('image/png')) throw new Error('Expected a cached PNG frame');
    const image = await response.blob();
    if (image.size === 0) throw new Error('Empty cached frame');
    return image;
  }

  stop(): void {
    this.live = false;
    this.generation += 1;
    this.disconnect();
    this.operations = this.operations.then(() => this.releaseSession()).catch(() => undefined);
  }

  pageExit(): void {
    this.stop();
    if (this.ownedSessionId) {
      void fetch(api.sessionPath(this.ownedSessionId), { method: 'DELETE', keepalive: true }).catch(() => undefined);
    }
  }
}