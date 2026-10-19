import { lazy, Suspense, useEffect, useRef, useState, useSyncExternalStore } from 'react';
import type { PointerEvent as ReactPointerEvent, ReactNode } from 'react';
import { Activity, AlertCircle, Check, ChevronDown, Download, Hand, LoaderCircle, Maximize,
  FlipHorizontal2, PanelLeftClose, PanelLeftOpen, Play, RefreshCw, Rotate3D, RotateCcw, Scan, X, ZoomIn, ZoomOut } from 'lucide-react';
import { clamp, matchedStandardView, orbitCamera, orientationLabels, panCamera, zoomCamera } from './camera';
import { planeAngles, planeNormal, planeRadius } from './clip-plane';
import type { InteractionMode } from './clip-plane';
import type { CropBox, RenderStyle, Vec3 } from './protocol';
import { WorkstationClient } from './workstation';
import type { WorkstationState } from './workstation';

const GlassPlane = lazy(() => import('./GlassPlane').then(module => ({ default: module.GlassPlane })));

const backgrounds: { name: string; rgb: Vec3; hex: string }[] = [
  { name: 'Charcoal', rgb: [0.055, 0.065, 0.065], hex: '#0e1111' },
  { name: 'Black', rgb: [0, 0, 0], hex: '#000000' },
  { name: 'Gray', rgb: [0.32, 0.34, 0.34], hex: '#525757' },
  { name: 'White', rgb: [1, 1, 1], hex: '#ffffff' }
];

const viewButtons = [
  { id: 'anterior', name: 'Anterior', short: 'A' },
  { id: 'posterior', name: 'Posterior', short: 'P' },
  { id: 'left', name: 'Left', short: 'L' },
  { id: 'right', name: 'Right', short: 'R' },
  { id: 'superior', name: 'Superior', short: 'S' },
  { id: 'inferior', name: 'Inferior', short: 'I' }
];

function IconButton({ label, children, active = false, ...props }: {
  label: string; children: ReactNode; active?: boolean;
} & Omit<React.ButtonHTMLAttributes<HTMLButtonElement>, 'children'>) {
  return <span className="tooltip-anchor">
    <button type="button" className={`icon-button${active ? ' active' : ''}`} aria-label={label}
      aria-pressed={active || undefined} {...props}>{children}</button>
    <span className="tooltip" role="tooltip">{label}</span>
  </span>;
}

function Parameter({ id, label, value, min, max, step = 1, suffix, disabled, change, finish }: {
  id: string; label: string; value: number; min: number; max: number; step?: number; suffix?: string;
  disabled: boolean; change: (value: number) => void; finish: () => void;
}) {
  const [draft, setDraft] = useState<string | null>(null);
  const commit = () => {
    if (draft !== null && draft.trim() && Number.isFinite(Number(draft))) change(clamp(Number(draft), min, max));
    setDraft(null);
    finish();
  };
  return <div className="parameter">
    <div className="parameter-heading">
      <label htmlFor={`${id}-number`}>{label}</label>
      <div className="number-with-unit">
        <input id={`${id}-number`} type="number" min={min} max={max} step={step} disabled={disabled}
          value={draft ?? value} onFocus={() => setDraft(String(value))}
          onChange={event => {
            setDraft(event.target.value);
            const number = event.target.valueAsNumber;
            if (Number.isFinite(number) && number >= min && number <= max) change(number);
          }} onBlur={commit} onKeyDown={event => {
            if (event.key === 'Enter') event.currentTarget.blur();
            if (event.key === 'Escape') { setDraft(null); event.currentTarget.blur(); }
          }} />
        {suffix && <span>{suffix}</span>}
      </div>
    </div>
    <input className="range" type="range" min={min} max={max} step={step} disabled={disabled}
      aria-label={`${label} slider`} value={clamp(value, min, max)}
      onChange={event => change(event.target.valueAsNumber)} onPointerUp={finish} onKeyUp={finish} />
    <div className="range-limits" aria-hidden="true"><span>{min}</span><span>{max}</span></div>
  </div>;
}

function CropControls({ client, state }: { client: WorkstationClient; state: WorkstationState }) {
  const crop = state.cropBox;
  const available = state.phase === 'ready' && state.session?.cropBoxSupported === true && crop !== null;
  const editable = available && crop.enabled;
  const change = (axis: number, bound: 'minimum' | 'maximum', percentage: number) => {
    const current = client.getSnapshot().cropBox;
    if (!current || !Number.isFinite(percentage)) return;
    const next: CropBox = structuredClone(current);
    const lower = bound === 'minimum' ? 0 : current.minimum[axis]! + 0.01;
    const upper = bound === 'maximum' ? 1 : current.maximum[axis]! - 0.01;
    next[bound][axis] = Math.round(clamp(percentage / 100, lower, upper) * 100) / 100;
    client.setCropBox(next);
  };
  return <section className="settings-section" data-testid="crop-controls">
    <div className="section-heading"><h2>Crop volume</h2>
      <IconButton label="Reset crop" disabled={!available} onClick={() => client.resetCropBox()}><RotateCcw size={15} /></IconButton>
    </div>
    <label className="background-setting crop-toggle"><span className="field-label">Enable crop</span>
      <input type="checkbox" aria-label="Enable crop" disabled={!available} checked={crop?.enabled ?? false}
        onChange={event => { if (crop) client.setCropBox({ ...crop, enabled: event.target.checked }); }} />
    </label>
    {['Columns (X)', 'Rows (Y)', 'Slices (Z)'].map((label, axis) => <fieldset className="crop-axis" key={axis} disabled={!editable}>
      <legend>{label}</legend>
      {(['minimum', 'maximum'] as const).map(bound => {
        const name = bound === 'minimum' ? 'From' : 'To';
        const value = Math.round((crop?.[bound][axis] ?? (bound === 'minimum' ? 0 : 1)) * 100);
        const minimum = bound === 'minimum' ? 0 : Math.round((crop?.minimum[axis] ?? 0) * 100) + 1;
        const maximum = bound === 'maximum' ? 100 : Math.round((crop?.maximum[axis] ?? 1) * 100) - 1;
        const accessible = `${label} ${name.toLowerCase()}`;
        return <div className="crop-bound" key={bound}>
          <label htmlFor={`crop-${axis}-${bound}`}>{name}</label>
          <input type="range" className="range" aria-label={`${accessible} slider`} min={minimum} max={maximum} step={1}
            value={value} onChange={event => change(axis, bound, event.target.valueAsNumber)}
            onPointerUp={() => client.finishInteraction()} onKeyUp={() => client.finishInteraction()} />
          <input id={`crop-${axis}-${bound}`} type="number" aria-label={accessible} min={minimum} max={maximum}
            step={1} value={value} onChange={event => change(axis, bound, event.target.valueAsNumber)}
            onBlur={() => client.finishInteraction()} /><span>%</span>
        </div>;
      })}
    </fieldset>)}
  </section>;
}

function PlaneControls({ client, state }: {
  client: WorkstationClient; state: WorkstationState;
}) {
  const plane = state.clipPlane;
  const available = state.phase === 'ready' && state.session?.clipPlaneSupported === true && plane !== null;
  const editable = available && plane.enabled;
  const angles = planeAngles(plane?.normal ?? [0, 0, 1]);
  const radius = Math.ceil(planeRadius(state.session?.volume) * 10) / 10;
  const rotate = (azimuth: number, tilt: number) => {
    const current = client.getSnapshot().clipPlane;
    if (current) client.setClipPlane({ ...current, normal: planeNormal(azimuth, tilt) });
  };
  return <section className="settings-section" data-testid="plane-controls">
    <div className="section-heading"><h2>Glass clipping plane</h2>
      <div className="toolbar-group">
        <IconButton label="Flip retained side" disabled={!editable} active={plane?.keepPositive === false}
          onClick={() => { if (plane) client.setClipPlane({ ...plane, keepPositive: !plane.keepPositive }); }}><FlipHorizontal2 size={16} /></IconButton>
        <IconButton label="Reset clipping plane" disabled={!available} onClick={() => client.resetClipPlane()}><RotateCcw size={15} /></IconButton>
      </div>
    </div>
    <label className="background-setting crop-toggle"><span className="field-label">Enable plane</span>
      <input type="checkbox" aria-label="Enable clipping plane" disabled={!available} checked={plane?.enabled ?? false}
        onChange={event => { if (plane) client.setClipPlane({ ...plane, enabled: event.target.checked }); }} />
    </label>
    <Parameter id="plane-azimuth" label="Azimuth (LPS)" value={Number(angles.azimuth.toFixed(1))}
      min={-180} max={180} step={0.1} suffix="deg" disabled={!editable}
      change={azimuth => rotate(azimuth, angles.tilt)} finish={() => client.finishInteraction()} />
    <Parameter id="plane-tilt" label="Tilt (LPS)" value={Number(angles.tilt.toFixed(1))}
      min={0} max={180} step={0.1} suffix="deg" disabled={!editable}
      change={tilt => rotate(angles.azimuth, tilt)} finish={() => client.finishInteraction()} />
    <Parameter id="plane-offset" label="Plane position" value={Number((plane?.offsetMm ?? 0).toFixed(1))}
      min={-radius} max={radius} step={0.1} suffix="mm" disabled={!editable}
      change={offsetMm => { if (plane) client.setClipPlane({ ...plane, offsetMm }); }} finish={() => client.finishInteraction()} />
  </section>;
}

function Sidebar({ client, state, open, dismiss }: {
  client: WorkstationClient; state: WorkstationState; open: boolean; dismiss: () => void;
}) {
  const ready = state.phase === 'ready';
  const busy = ['loading', 'closing', 'catalog'].includes(state.phase);
  const selected = state.series.find(series => series.id === state.selectedId);
  const style = state.style;
  const preset = state.presets.find(item => item.id === style?.presetId);
  const setStyle = (patch: Partial<RenderStyle>) => {
    const current = client.getSnapshot().style;
    if (current) client.setStyle({ ...current, ...patch });
  };
  return <>
    {open && <button className="drawer-scrim" aria-label="Close settings" onClick={dismiss} />}
    <aside id="settings-panel" className={`sidebar${open ? ' drawer-open' : ''}`} aria-label="Volume settings">
      <div className="sidebar-mobile-heading"><strong>Volume settings</strong>
        <IconButton label="Close settings" onClick={dismiss}><X size={18} /></IconButton>
      </div>
      <section className="settings-section">
        <div className="section-heading"><h2>Data source</h2>
          <IconButton label="Refresh series" disabled={busy} onClick={() => void client.refreshCatalog()}><RefreshCw size={15} /></IconButton>
        </div>
        <label className="field-label" htmlFor="series">Series</label>
        <div className="select-wrap"><select id="series" value={state.selectedId} disabled={busy || !state.series.length}
          onChange={event => client.selectSeries(event.target.value)}>
          {!state.series.length && <option value={state.selectedId}>No series available</option>}
          {state.series.map(series => <option key={series.id} value={series.id}>
            {series.description || series.id}{series.supported ? '' : ' (unsupported)'}
          </option>)}
        </select><ChevronDown size={14} /></div>
        <div className="series-meta"><span>{selected?.id === 'phantom' ? 'SYNTHETIC' : 'DICOM SERIES'}</span>
          <span>{selected ? `${selected.fileCount} files` : '--'}</span></div>
        {selected && !selected.supported && <p className="inline-error" role="status">{selected.reason || 'Unsupported series'}</p>}
        {selected?.warning && <p className="inline-error" role="status">{selected.warning}</p>}
        {state.catalogError && <p className="inline-error" role="alert">{state.catalogError}</p>}
        <button className="load-button" type="button" disabled={busy || !selected?.supported}
          onClick={() => { void client.loadSeries(); dismiss(); }}>
          {busy ? <LoaderCircle className="spin" size={16} /> : <Play size={15} />}
          <span>{state.phase === 'closing' ? 'Closing session' : state.phase === 'loading' ? 'Loading volume' : 'Load series'}</span>
        </button>
      </section>
      <section className="settings-section">
        <div className="section-heading"><h2>Appearance</h2><span className="section-index">01</span></div>
        <label className="field-label" htmlFor="preset">Transfer preset</label>
        <div className="preset-select-row">
        <div className="select-wrap"><select id="preset" disabled={!ready || state.presets.length === 0} value={style?.presetId ?? 'ct-bone'}
          onChange={event => client.applyPreset(event.target.value)}>
          {!state.presets.length && <option value={style?.presetId ?? 'ct-bone'}>{style?.presetId ?? 'CT / Bone'}</option>}
          {style && state.presets.length > 0 && !preset && <option value={style.presetId}>{style.presetId}</option>}
          {state.presets.map(item => <option key={item.id} value={item.id}>{item.name}</option>)}
        </select><ChevronDown size={14} /></div>
        <IconButton label="Reset preset" disabled={!ready || !preset}
          onClick={() => { if (preset) client.applyPreset(preset.id); }}><RotateCcw size={16} /></IconButton>
        </div>
        {preset && <div className="preset-colors" role="img" aria-label={`${preset.name} color table`}>
          {preset.colors.map((color, index) => <span key={index}
            style={{ backgroundColor: `rgb(${color.map(channel => Math.round(channel * 255)).join(' ')})` }} />)}
        </div>}
        {state.presetError && <div className="preset-error"><p className="inline-error" role="alert">{state.presetError}</p>
          <IconButton label="Retry presets" onClick={() => void client.refreshPresets()}><RefreshCw size={15} /></IconButton>
        </div>}
        <Parameter id="window-width" label="Window width" value={style?.windowWidth ?? 1800} min={1} max={5000}
          suffix="HU" disabled={!ready} change={windowWidth => setStyle({ windowWidth })} finish={() => client.finishInteraction()} />
        <Parameter id="window-level" label="Window level" value={style?.windowLevel ?? 400} min={-1500} max={3000}
          suffix="HU" disabled={!ready} change={windowLevel => setStyle({ windowLevel })} finish={() => client.finishInteraction()} />
        <Parameter id="opacity" label="Opacity scale" value={style?.opacityScale ?? 1} min={0} max={3} step={0.05}
          disabled={!ready} change={opacityScale => setStyle({ opacityScale })} finish={() => client.finishInteraction()} />
        <label className="background-setting"><span className="field-label">Gradient lighting</span>
          <input type="checkbox" disabled={!ready} checked={style?.lighting ?? false}
            onChange={event => setStyle({ lighting: event.target.checked })} />
        </label>
        <div className="background-setting"><span className="field-label">Background</span>
          <div className="swatches" role="group" aria-label="Background color">
            {backgrounds.map(background => {
              const active = style?.backgroundRgb.every((value, index) => Math.abs(value - (background.rgb[index] ?? 0)) < 0.005);
              return <button key={background.name} type="button" className={`swatch${active ? ' selected' : ''}`}
                style={{ backgroundColor: background.hex }} aria-label={`${background.name} background`} title={background.name}
                aria-pressed={!!active} disabled={!ready} onClick={() => setStyle({ backgroundRgb: background.rgb })}>
                {active && <Check size={13} color={background.name === 'White' ? '#1b2929' : '#ffffff'} />}
              </button>;
            })}
          </div>
        </div>
      </section>
      <PlaneControls client={client} state={state} />
      <CropControls client={client} state={state} />
      <section className="settings-section geometry-section">
        <div className="section-heading"><h2>Volume geometry</h2><span className="section-index">02</span></div>
        <dl className="geometry">
          <div><dt>Matrix</dt><dd>{state.session?.volume?.dimensions.join(' \u00d7 ') ?? '--'}</dd></div>
          <div><dt>Spacing</dt><dd>{state.session?.volume ? state.session.volume.spacing.map(value => Number(value.toFixed(2))).join(' / ') : '--'}<small>mm</small></dd></div>
          <div><dt>Field height</dt><dd>{state.camera?.orthoHeightMm.toFixed(1) ?? '--'}<small>mm</small></dd></div>
          <div><dt>Revision</dt><dd>{state.session?.sceneRevision ?? '--'}</dd></div>
        </dl>
      </section>
      <section className="settings-section">
        <div className="section-heading"><h2>Frame timings</h2><span className="section-index">03</span></div>
        <dl className="frame-metrics" data-testid="frame-timings">
          {[
            ['Kernel', state.frame?.timings?.kernelMs],
            ['Readback', state.frame?.timings?.readbackMs],
            ['Encode', state.frame?.timings?.encodeMs],
            ['Worker wait', state.frame?.timings?.queueMs],
            ['Render total', state.frame?.timings?.totalMs],
            ['Decode', state.frame?.presentationTimings?.decodeMs],
            ['Canvas submit', state.frame?.presentationTimings?.drawMs],
            ['Load data', state.session?.loadTimings?.dataLoadMs],
            ['GPU setup', state.session?.loadTimings?.uploadMs]
          ].map(([label, value]) => <div key={String(label)}><dt>{label}</dt>
            <dd>{typeof value === 'number' ? value.toFixed(2) : '--'}<small>ms</small></dd></div>)}
        </dl>
      </section>
      <div className="sidebar-bottom"><Activity size={14} /><span>CUDA volume rendering</span><span className="version">v1</span></div>
    </aside>
  </>;
}

function Viewport({ client, state, mode, resolution }: {
  client: WorkstationClient; state: WorkstationState; mode: InteractionMode; resolution: number;
}) {
  const container = useRef<HTMLDivElement>(null);
  const canvas = useRef<HTMLCanvasElement>(null);
  const drag = useRef<{ id: number; x: number; y: number; pan: boolean } | null>(null);
  const interactive = state.phase === 'ready' && state.connection === 'online';

  useEffect(() => {
    const host = container.current;
    if (!host) return;
    client.setCanvas(canvas.current);
    const resize = (width: number, height: number) => {
      if (width <= 0 || height <= 0) return;
      const scale = Math.min(window.devicePixelRatio || 1, 2) * resolution;
      const constraint = Math.min(1, 2048 / Math.max(1, width * scale, height * scale));
      client.resize(Math.max(2, Math.round(width * scale * constraint)), Math.max(2, Math.round(height * scale * constraint)));
    };
    const measure = () => resize(host.clientWidth, host.clientHeight);
    const observer = new ResizeObserver(([entry]) => {
      if (entry) resize(entry.contentRect.width, entry.contentRect.height);
    });
    measure();
    observer.observe(host);
    document.addEventListener('visibilitychange', measure);
    return () => {
      observer.disconnect();
      document.removeEventListener('visibilitychange', measure);
      client.setCanvas(null);
    };
  }, [client, resolution]);

  useEffect(() => {
    const host = container.current;
    if (!host) return;
    const wheel = (event: WheelEvent) => {
      const current = client.getSnapshot();
      if (!current.camera || current.phase !== 'ready' || current.connection !== 'online') return;
      event.preventDefault();
      const delta = event.deltaY * (event.deltaMode === 1 ? 16 : event.deltaMode === 2 ? host.clientHeight : 1);
      client.setCamera(zoomCamera(current.camera, delta));
    };
    host.addEventListener('wheel', wheel, { passive: false });
    return () => host.removeEventListener('wheel', wheel);
  }, [client]);

  const release = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (drag.current?.id !== event.pointerId) return;
    drag.current = null;
    if (event.currentTarget.hasPointerCapture(event.pointerId)) event.currentTarget.releasePointerCapture(event.pointerId);
    client.finishInteraction();
  };

  const background = state.style?.backgroundRgb ?? backgrounds[0]!.rgb;
  const light = background.reduce((sum, value) => sum + value, 0) > 2;
  const phaseTitle = state.phase === 'catalog' ? 'Connecting to service' : state.phase === 'loading' ? 'Loading volume' :
    state.phase === 'closing' ? 'Closing session' : state.phase === 'error' ? 'Session unavailable' :
    state.phase === 'ready' ? state.error ? 'Image unavailable' : 'Awaiting render' : 'No volume loaded';
  const loading = ['catalog', 'loading', 'closing'].includes(state.phase) ||
    (state.phase === 'ready' && !state.error && state.connection !== 'offline');
  const hasFrame = !!state.frame && state.phase === 'ready';
  const orientation = state.frame?.camera ? orientationLabels(state.frame.camera.rotationXyzw) : null;
  return <div ref={container} className={`viewport ${mode}${light ? ' light-viewport' : ''}`}
    style={{ backgroundColor: `rgb(${background.map(value => Math.round(value * 255)).join(' ')})` }}
    data-testid="viewport" aria-label="Volume rendering viewport" tabIndex={0}
    onContextMenu={event => event.preventDefault()}
    onDoubleClick={() => { if (interactive) client.resetCamera(); }}
    onPointerDown={event => {
      if (!interactive || drag.current || !event.isPrimary || (event.button !== 0 && event.button !== 2)) return;
      event.preventDefault();
      event.currentTarget.focus();
      event.currentTarget.setPointerCapture(event.pointerId);
      drag.current = { id: event.pointerId, x: event.clientX, y: event.clientY, pan: event.button === 2 || mode === 'pan' };
    }} onPointerMove={event => {
      const previous = drag.current;
      const camera = client.getSnapshot().camera;
      if (!previous || previous.id !== event.pointerId || !camera || !interactive) return;
      const rect = event.currentTarget.getBoundingClientRect();
      const updated = previous.pan ? panCamera(camera, event.clientX - previous.x, event.clientY - previous.y, rect.height) :
        orbitCamera(camera, [previous.x - rect.left, previous.y - rect.top], [event.clientX - rect.left, event.clientY - rect.top], rect.width, rect.height);
      client.setCamera(updated);
      previous.x = event.clientX;
      previous.y = event.clientY;
    }} onPointerUp={release} onPointerCancel={release} onLostPointerCapture={event => {
      if (event.target === event.currentTarget) release(event);
    }}>
    <canvas ref={canvas} width={state.width} height={state.height} aria-label="Server-rendered volume image"
      className={hasFrame ? 'image-canvas' : 'image-canvas hidden-canvas'} />
    {state.session?.clipPlaneSupported && (state.clipPlane?.enabled || state.frame?.clipPlane?.enabled) &&
      <Suspense fallback={null}><GlassPlane client={client} state={state} /></Suspense>}
    {hasFrame && orientation && <div className="orientation-overlay" aria-label="Patient orientation">
      <span className="orientation-top" data-testid="orientation-top">{orientation.top}</span>
      <span className="orientation-left" data-testid="orientation-left">{orientation.left}</span>
      <span className="orientation-right" data-testid="orientation-right">{orientation.right}</span>
      <span className="orientation-bottom" data-testid="orientation-bottom">{orientation.bottom}</span>
    </div>}
    <div className="viewport-top-left"><span className="viewport-label">VOLUME / 3D</span>
      <span className="viewport-series">{state.activeSeries?.description || 'Volume workstation'}</span>
      {state.activeSeries?.id === 'phantom' && <span className="synthetic-label">SYNTHETIC PHANTOM</span>}
      {hasFrame && state.frame?.cropBox?.enabled && <span className="crop-status" data-testid="crop-status">CROPPED</span>}
      {hasFrame && state.frame?.clipPlane?.enabled && <span className="plane-status" data-testid="plane-status">PLANE CLIPPED</span>}
    </div>
    <div className="viewport-top-right"><span className="viewport-label">ORTHOGRAPHIC</span>
      <span className="mono">{state.camera ? `${state.camera.orthoHeightMm.toFixed(1)} mm` : '--'}</span></div>
    {!hasFrame && <div className="empty-viewport" role="status" aria-live="polite">
      <div className={`empty-mark${state.error ? ' error-mark' : ''}`}>
        {loading ? <LoaderCircle size={26} className="spin" /> : state.error ? <AlertCircle size={26} /> : <Scan size={28} />}
      </div>
      <h2>{phaseTitle}</h2>
      <span className="empty-detail">{state.phase === 'ready' ? state.connection === 'online' ? 'Image stream connected' :
        state.connection === 'reconnecting' ? 'Reconnecting image stream' : 'Image stream offline' :
        state.activeSeries?.description || 'Select a series'}</span>
    </div>}
    {hasFrame && state.rendering && <div className="render-indicator" role="status"><LoaderCircle className="spin" size={12} />Rendering</div>}
    <div className="viewport-bottom-left"><span className="viewport-label">{state.style?.presetId ?? 'ct-bone'}</span>
      <span className="mono">W {state.style?.windowWidth ?? '--'} / L {state.style?.windowLevel ?? '--'}</span></div>
    <div className="viewport-bottom-right"><span className="viewport-label">{state.frame?.quality.toUpperCase() ?? 'NO FRAME'}</span>
      <span className="mono">{state.frame ? `#${state.frame.seq}` : '--'}</span></div>
  </div>;
}

export function App() {
  const sequence = useRef(0);
  const [client] = useState(() => new WorkstationClient(sequence));
  const state = useSyncExternalStore(client.subscribe, client.getSnapshot);
  const [drawerOpen, setDrawerOpen] = useState(false);
  const [mode, setMode] = useState<InteractionMode>('rotate');
  const [resolution, setResolution] = useState(1);
  const [exporting, setExporting] = useState(false);
  const [exportError, setExportError] = useState<string | null>(null);
  const viewer = useRef<HTMLElement>(null);
  const errorBanner = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const host = viewer.current;
    const banner = errorBanner.current;
    if (!host || !banner) return;
    const observer = new ResizeObserver(() => {
      host.style.setProperty('--alert-height', `${banner.getBoundingClientRect().height}px`);
    });
    observer.observe(banner);
    return () => { observer.disconnect(); host.style.removeProperty('--alert-height'); };
  }, [state.error, exportError]);

  useEffect(() => {
    void client.start();
    const exit = () => client.pageExit();
    const restore = (event: PageTransitionEvent) => { if (event.persisted) void client.start(); };
    window.addEventListener('pagehide', exit);
    window.addEventListener('pageshow', restore);
    return () => {
      window.removeEventListener('pagehide', exit);
      window.removeEventListener('pageshow', restore);
      client.stop();
    };
  }, [client]);

  useEffect(() => {
    if (!drawerOpen) return;
    const button = document.querySelector<HTMLButtonElement>('#settings-panel [aria-label="Close settings"]');
    button?.focus();
    const keyboard = (event: KeyboardEvent) => {
      if (event.key === 'Escape') { setDrawerOpen(false); document.querySelector<HTMLButtonElement>('[aria-controls="settings-panel"]')?.focus(); }
      if (event.key !== 'Tab') return;
      const focusable = Array.from(document.querySelectorAll<HTMLElement>(
        '#settings-panel button:not(:disabled), #settings-panel input:not(:disabled), #settings-panel select:not(:disabled)'));
      const first = focusable[0];
      const last = focusable.at(-1);
      if (event.shiftKey && document.activeElement === first) { event.preventDefault(); last?.focus(); }
      if (!event.shiftKey && document.activeElement === last) { event.preventDefault(); first?.focus(); }
    };
    window.addEventListener('keydown', keyboard);
    return () => window.removeEventListener('keydown', keyboard);
  }, [drawerOpen]);

  const ready = state.phase === 'ready' && state.connection === 'online';
  const selectedView = matchedStandardView(state.camera, state.session?.standardViews ?? []);
  const canExport = ready && !!state.frame && state.frame.quality === 'final' && !state.rendering && !exporting;
  const status = state.phase === 'loading' ? 'Loading' : state.phase === 'closing' ? 'Closing' : state.phase === 'error' ? 'Error' :
    state.connection === 'online' ? 'Connected' : state.connection === 'connecting' ? 'Connecting' : state.connection === 'reconnecting' ? 'Reconnecting' : 'Idle';
  const zoom = (delta: number) => {
    const camera = client.getSnapshot().camera;
    if (camera) client.setCamera(zoomCamera(camera, delta));
  };
  const download = async () => {
    setExporting(true);
    setExportError(null);
    try {
      const image = await client.downloadFinal();
      const url = URL.createObjectURL(image);
      const anchor = document.createElement('a');
      anchor.href = url;
      anchor.download = `medgl-frame-${state.frame?.seq ?? 'final'}.png`;
      anchor.click();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
    } catch (error) { setExportError(error instanceof Error ? error.message : 'Export failed'); }
    finally { setExporting(false); }
  };

  return <div className="workstation">
    <header className="app-header">
      <div className="brand"><span className="brand-mark"><Scan size={21} strokeWidth={1.8} /></span>
        <strong>MEDGL<span className="brand-dot">.</span></strong><span className="brand-divider" />
        <span className="product-title">Volume workstation</span></div>
      <div className="header-status"><span className={`status-dot${state.connection === 'online' ? ' online' : state.error ? ' failed' : ''}`} />
        <span>{status}</span><span className="header-protocol">CUDA / VR</span></div>
    </header>
    <main className="workspace">
      <Sidebar client={client} state={state} open={drawerOpen} dismiss={() => setDrawerOpen(false)} />
      <section ref={viewer} className="viewer" aria-label="Volume viewer">
        <div className="viewer-toolbar">
          <div className="toolbar-group mobile-settings"><IconButton label={drawerOpen ? 'Close settings' : 'Open settings'}
            aria-expanded={drawerOpen} aria-controls="settings-panel" onClick={() => setDrawerOpen(!drawerOpen)}>
            {drawerOpen ? <PanelLeftClose size={18} /> : <PanelLeftOpen size={18} />}</IconButton></div>
          <div className="toolbar-group" role="group" aria-label="Camera interaction">
            <IconButton label="Rotate volume" active={mode === 'rotate'} onClick={() => setMode('rotate')}><Rotate3D size={18} /></IconButton>
            <IconButton label="Pan volume" active={mode === 'pan'} onClick={() => setMode('pan')}><Hand size={18} /></IconButton>
          </div>
          <span className="toolbar-divider" />
          <div className="toolbar-group" role="group" aria-label="Camera controls">
            <IconButton label="Reset camera" disabled={!ready} onClick={() => client.resetCamera()}><RotateCcw size={17} /></IconButton>
            <IconButton label="Zoom out" disabled={!ready} onClick={() => zoom(120)}><ZoomOut size={18} /></IconButton>
            <IconButton label="Zoom in" disabled={!ready} onClick={() => zoom(-120)}><ZoomIn size={18} /></IconButton>
          </div>
          <div className="toolbar-spacer" />
          <div className="resolution-control"><label htmlFor="resolution">Render size</label>
            <div className="select-wrap compact"><select id="resolution" value={resolution}
              onChange={event => setResolution(Number(event.target.value))}>
              <option value={1}>Native</option><option value={0.75}>75%</option><option value={0.5}>50%</option>
            </select><ChevronDown size={12} /></div>
          </div>
          <span className="toolbar-divider export-divider" />
          <IconButton label="Download final PNG" disabled={!canExport} onClick={() => void download()}>
            {exporting ? <LoaderCircle className="spin" size={18} /> : <Download size={18} />}</IconButton>
        </div>
        <div className="standard-viewbar" role="group" aria-label="Standard views">
          {viewButtons.map(view => <span key={view.id} className="tooltip-anchor">
            <button type="button" className="standard-view-button" aria-label={`${view.name} view`}
              aria-pressed={selectedView === view.id}
              disabled={!ready || !state.session?.standardViews?.some(item => item.id === view.id)}
              onClick={() => client.setStandardView(view.id)}>
              <span className="standard-view-name">{view.name}</span>
              <span className="standard-view-short" aria-hidden="true">{view.short}</span>
            </button>
            <span className="tooltip" role="tooltip">{view.name}</span>
          </span>)}
        </div>
        {(state.error || exportError) && <div ref={errorBanner} className="error-banner" role="alert"><AlertCircle size={16} />
          <span>{exportError || state.error}</span>
          {exportError ? <IconButton label="Dismiss export error" onClick={() => setExportError(null)}><X size={16} /></IconButton> :
            <IconButton label={state.phase === 'ready' ? 'Reconnect stream' : 'Retry session'} onClick={() => {
              if (state.phase === 'ready') client.reconnect(); else void client.loadSeries();
            }}><RefreshCw size={16} /></IconButton>}
        </div>}
        <Viewport client={client} state={state} mode={mode} resolution={resolution} />
        <footer className="viewer-footer"><div><span className={`status-dot${state.connection === 'online' ? ' online' : ''}`} />
          <span>{state.connection === 'online' ? state.rendering ? 'Rendering' : state.frame ? 'Ready' : 'Awaiting image' : status}</span></div>
          <div className="frame-stats"><Maximize size={12} /><span>{state.width}{' \u00d7 '}{state.height}</span>
            <span className="latency"><span data-testid="send-to-canvas">{client.lastLatencyMs === null ? '-- ms' : `${client.lastLatencyMs} ms`}</span><span className="stat-label"> send to canvas</span></span></div>
        </footer>
      </section>
    </main>
  </div>;
}