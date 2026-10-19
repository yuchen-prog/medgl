import { useEffect, useRef, useState } from 'react';
import type { ButtonHTMLAttributes, PointerEvent as ReactPointerEvent, ReactNode } from 'react';
import { Plus, RotateCcw, Save, Trash2, X } from 'lucide-react';
import { colorHex, hexColor, histogramBars, insertPoint, intensityAt, movePoint, positionAt } from './transfer';
import type { TransferPoint } from './protocol';
import type { WorkstationClient, WorkstationState } from './workstation';

function Tool({ label, children, ...props }: { label: string; children: ReactNode } & ButtonHTMLAttributes<HTMLButtonElement>) {
  return <span className="tooltip-anchor"><button type="button" className="icon-button" aria-label={label} {...props}>{children}</button>
    <span className="tooltip" role="tooltip">{label}</span></span>;
}

export function TransferEditor({ client, state, close }: { client: WorkstationClient; state: WorkstationState; close: () => void }) {
  const style = state.style;
  const preset = state.customPresets.find(item => item.id === state.customPresetId);
  const builtIn = state.presets.find(item => item.id === style?.presetId);
  const points = style?.transferFunction ?? builtIn?.transferFunction ?? [];
  const enabled = state.phase === 'ready' && state.session?.transferFunctionSupported === true && points.length >= 2;
  const [selection, setSelection] = useState(2);
  const [opacityRange, setOpacityRange] = useState(0);
  const [dragScale, setDragScale] = useState<number | null>(null);
  const [name, setName] = useState('My preset');
  const [saving, setSaving] = useState(false);
  const [chartWidth, setChartWidth] = useState(256);
  const nameInput = useRef<HTMLInputElement>(null);
  const chart = useRef<HTMLDivElement>(null);
  const drag = useRef<{ id: number; index: number; scale: number; bounds: DOMRect; width: number } | null>(null);
  useEffect(() => {
    const host = chart.current;
    if (!host) return;
    const observer = new ResizeObserver(([entry]) => {
      if (entry && entry.contentRect.width > 0) setChartWidth(Math.max(100, entry.contentRect.width));
    });
    observer.observe(host);
    return () => observer.disconnect();
  }, []);
  useEffect(() => { if (saving) nameInput.current?.focus(); }, [saving]);
  const plotWidth = chartWidth - 32;
  const selectedIndex = Math.min(selection, Math.max(0, points.length - 1));
  const selected = points[selectedIndex];
  const endpoint = selectedIndex === 0 || selectedIndex === points.length - 1;
  const maximum = Math.max(0.01, ...points.map(point => point.rgba[3]));
  const scale = dragScale ?? (opacityRange || [0.05, 0.1, 0.25, 0.5, 1].find(value => value >= maximum * 1.1) || 1);
  const bars = style ? histogramBars(state.session?.histogram, style) : [];
  const source = preset?.style.transferFunction ?? builtIn?.transferFunction;
  const edited = enabled && source && JSON.stringify(source) !== JSON.stringify(points);
  const update = (position: number, opacity: number) => {
    const current = client.getSnapshot().style?.transferFunction ?? points;
    client.setTransferFunction(movePoint(current, selectedIndex, position, opacity));
  };
  const add = (position?: number) => {
    const index = Math.min(selectedIndex, points.length - 2);
    const target = position ?? (points[index]!.position + points[index + 1]!.position) / 2;
    const next = insertPoint(points, target);
    if (next !== points) {
      setSelection(next.findIndex(point => point.position === target));
      client.setTransferFunction(next);
    }
  };
  const remove = () => {
    if (endpoint) return;
    client.setTransferFunction(points.filter((_, index) => index !== selectedIndex));
    setSelection(Math.max(0, selectedIndex - 1));
  };
  const move = (event: ReactPointerEvent<HTMLButtonElement>) => {
    const current = drag.current;
    if (!current || current.id !== event.pointerId) return;
    const position = ((event.clientX - current.bounds.left) / current.bounds.width * current.width - 20) / (current.width - 32);
    const opacity = (138 - (event.clientY - current.bounds.top) / current.bounds.height * 174) / 128 * current.scale;
    const live = client.getSnapshot().style?.transferFunction ?? points;
    client.setTransferFunction(movePoint(live, current.index, position, opacity));
  };
  const release = (event: ReactPointerEvent<HTMLButtonElement>) => {
    if (drag.current?.id !== event.pointerId) return;
    drag.current = null;
    setDragScale(null);
    if (event.currentTarget.hasPointerCapture(event.pointerId)) event.currentTarget.releasePointerCapture(event.pointerId);
    client.finishInteraction();
  };
  const gradient = points.map(point => `${colorHex(point.rgba)} ${point.position * 100}%`).join(', ');
  return <div className="transfer-editor" data-testid="transfer-editor">
    <div className="transfer-heading"><h3>Transfer function</h3>
      <span className="transfer-preset-name">{preset?.name ?? builtIn?.name}</span>
      {edited && <span className="transfer-edited" role="status">Edited</span>}
      <div className="toolbar-spacer" />
      <Tool label="Restore built-in curve" disabled={!enabled || !builtIn} onClick={() => client.resetTransferFunction()}><RotateCcw size={14} /></Tool>
      <Tool label="Close transfer editor" onClick={close}><X size={16} /></Tool>
    </div>
    <div className="transfer-body"><div className="transfer-plot">
    <div className="curve-chart" ref={chart} data-testid="curve-chart" aria-disabled={!enabled}
      onDoubleClick={event => {
        if (!enabled || (event.target as Element).closest('button')) return;
        const bounds = event.currentTarget.getBoundingClientRect();
        add(((event.clientX - bounds.left) / bounds.width * chartWidth - 20) / plotWidth);
      }}>
      <svg viewBox={`0 0 ${chartWidth} 174`} role="img" aria-label="HU histogram and opacity curve" preserveAspectRatio="none">
        <defs><clipPath id="curve-area"><rect x="20" y="10" width={plotWidth} height="128" /></clipPath></defs>
        {[0, 0.5, 1].map(value => <g key={value}>
          <line x1="20" x2={chartWidth - 12} y1={138 - value * 128} y2={138 - value * 128} className="curve-grid" />
          <text x="16" y={141 - value * 128} textAnchor="end">{Math.round(value * scale * 100)}</text>
        </g>)}
        <g clipPath="url(#curve-area)" data-testid="histogram-bars">
          {bars.map((height, index) => <rect key={index} x={20 + index * plotWidth / bars.length} y={138 - height * 120}
            width={plotWidth / bars.length} height={height * 120} className="histogram-bar" />)}
          <polyline points={points.map(point => `${20 + point.position * plotWidth},${138 - point.rgba[3] / scale * 128}`).join(' ')} className="opacity-line" />
        </g>
        {[0, 0.5, 1].map(position => <text key={position} x={20 + position * plotWidth} y="159"
          textAnchor={position === 0 ? 'start' : position === 1 ? 'end' : 'middle'}>
          {style ? Math.round(intensityAt(position, style)) : '--'}</text>)}
        <text x={chartWidth - 12} y="172" textAnchor="end">HU</text>
      </svg>
      {points.map((point, index) => <button key={index} type="button" className={`curve-point${selectedIndex === index ? ' selected' : ''}`}
        aria-label={`Curve point ${index + 1}`} aria-pressed={selectedIndex === index} disabled={!enabled}
        title={`${style ? Math.round(intensityAt(point.position, style)) : '--'} HU / ${(point.rgba[3] * 100).toFixed(1)}%`}
        style={{ left: `${(20 + point.position * plotWidth) / chartWidth * 100}%`, top: `${Math.max(10, 138 - point.rgba[3] / scale * 128) / 174 * 100}%`, color: colorHex(point.rgba) }}
        onFocus={() => setSelection(index)} onClick={() => setSelection(index)}
        onPointerDown={event => {
          if (!event.isPrimary || event.button !== 0 || !chart.current) return;
          event.preventDefault();
          event.currentTarget.focus({ preventScroll: true });
          setSelection(index);
          setDragScale(scale);
          drag.current = { id: event.pointerId, index, scale, bounds: chart.current.getBoundingClientRect(), width: chartWidth };
          event.currentTarget.setPointerCapture(event.pointerId);
        }} onPointerMove={move} onPointerUp={release} onPointerCancel={release} onLostPointerCapture={release}
        onKeyDown={event => {
          if (!selected) return;
          if (event.key === 'Delete' || event.key === 'Backspace') { event.preventDefault(); remove(); }
          const amount = event.shiftKey ? 0.02 : 0.002;
          if (['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown'].includes(event.key)) {
            event.preventDefault();
            const horizontal = event.key === 'ArrowLeft' ? -amount : event.key === 'ArrowRight' ? amount : 0;
            const vertical = event.key === 'ArrowDown' ? -amount : event.key === 'ArrowUp' ? amount : 0;
            update(selected.position + horizontal, selected.rgba[3] + vertical);
          }
        }} />)}
    </div>
    <div className="curve-gradient" role="img" aria-label="Current transfer colors"
      style={points.length ? { background: `linear-gradient(to right, ${gradient})` } : undefined} />
    {state.session?.histogram && <div className="histogram-summary" title="Full volume histogram; logarithmic counts">
      <span>{Math.round(state.session.histogram.minimum)} to {Math.round(state.session.histogram.maximum)} HU</span>
      <span>{state.session.histogram.voxelCount.toLocaleString()} voxels</span>
    </div>}
    </div><div className="transfer-properties">
    <div className="curve-options"><label>Opacity range<select aria-label="Opacity axis range" value={opacityRange}
      onChange={event => setOpacityRange(Number(event.target.value))} disabled={!enabled}>
      <option value={0}>Auto</option>{[0.05, 0.1, 0.25, 0.5, 1].map(value => <option key={value} value={value}>{value * 100}%</option>)}
    </select></label><span className="toolbar-spacer" />
      <Tool label="Add curve point" disabled={!enabled || points.length >= 64} onClick={() => add()}><Plus size={15} /></Tool>
      <Tool label="Delete curve point" disabled={!enabled || endpoint} onClick={remove}><Trash2 size={14} /></Tool>
    </div>
    <fieldset className="curve-fields" disabled={!enabled}>
      <label>Intensity<input type="number" aria-label="Point intensity" value={style && selected ? Number(intensityAt(selected.position, style).toFixed(2)) : 0}
        disabled={!enabled || endpoint} step={1} min={style ? intensityAt(points[Math.max(0, selectedIndex - 1)]?.position ?? 0, style) + style.windowWidth * 0.001 : 0}
        max={style ? intensityAt(points[Math.min(points.length - 1, selectedIndex + 1)]?.position ?? 1, style) - style.windowWidth * 0.001 : 1}
        onChange={event => { if (style && selected && Number.isFinite(event.target.valueAsNumber)) update(positionAt(event.target.valueAsNumber, style), selected.rgba[3]); }}
        onBlur={() => client.finishInteraction()} /><small>HU</small></label>
      <label>Opacity<input type="number" aria-label="Point opacity" min={0} max={100} step={0.1}
        value={selected ? Number((selected.rgba[3] * 100).toFixed(2)) : 0}
        onChange={event => { if (selected && Number.isFinite(event.target.valueAsNumber)) update(selected.position, event.target.valueAsNumber / 100); }}
        onBlur={() => client.finishInteraction()} /><small>% / 1 mm</small></label>
      <label className="point-color">Color<input type="color" aria-label="Point color" value={selected ? colorHex(selected.rgba) : '#000000'}
        onChange={event => {
          if (!selected) return;
          const color = hexColor(event.target.value);
          client.setTransferFunction(points.map((point, index): TransferPoint => index === selectedIndex ?
            { ...point, rgba: [...color, point.rgba[3]] } : point));
        }} /></label>
    </fieldset>
    <div className="preset-save">
      {saving && <input ref={nameInput} type="text" aria-label="Custom preset name" placeholder="Preset name" maxLength={64}
        value={name} disabled={!enabled} onChange={event => setName(event.target.value)}
        onKeyDown={event => {
          if (event.key === 'Enter') client.saveCustomPreset(name);
          if (event.key === 'Escape') { event.stopPropagation(); setSaving(false); }
        }} />}
      <Tool label={saving ? 'Confirm save preset' : 'Save custom preset'} disabled={!enabled || !name.trim()}
        onClick={() => { if (saving) client.saveCustomPreset(name); else setSaving(true); }}><Save size={15} /></Tool>
      {saving && <Tool label="Cancel save preset" onClick={() => setSaving(false)}><X size={14} /></Tool>}
      {preset && <Tool label="Delete custom preset" disabled={!enabled} onClick={() => {
        if (confirm(`Delete local preset "${preset.name}"?`)) client.deleteCustomPreset();
      }}><Trash2 size={15} /></Tool>}
    </div>
    {state.storageError && <p className="inline-error" role="alert">{state.storageError}</p>}
    </div></div>
  </div>;
}