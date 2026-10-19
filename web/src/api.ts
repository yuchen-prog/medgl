import { parseCatalog, parsePresetCatalog, parseSession } from './protocol';
import type { Session } from './protocol';

const API_ROOT = '/api/v1';
export const sessionPath = (sessionId: string): string => `${API_ROOT}/sessions/${encodeURIComponent(sessionId)}`;

export async function request(path: string, init: RequestInit = {}): Promise<Response> {
  const signal = init.signal ? AbortSignal.any([init.signal, AbortSignal.timeout(10000)]) : AbortSignal.timeout(10000);
  const response = await fetch(path, { ...init, signal });
  if (!response.ok) {
    const detail = (await response.text()).slice(0, 200);
    throw new Error(`HTTP ${response.status}${detail ? `: ${detail}` : ''}`);
  }
  return response;
}

export async function catalog() {
  return parseCatalog(await (await request(`${API_ROOT}/series`)).json() as unknown);
}

export async function appearancePresets() {
  return parsePresetCatalog(await (await request(`${API_ROOT}/presets`)).json() as unknown);
}

export async function createSession(seriesId: string): Promise<Session> {
  const response = await request(`${API_ROOT}/sessions`, {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ seriesId })
  });
  return parseSession(await response.json() as unknown);
}

export async function getSession(sessionId: string): Promise<Session> {
  const session = parseSession(await (await request(sessionPath(sessionId))).json() as unknown);
  if (session.sessionId !== sessionId) throw new Error('Session identity mismatch');
  return session;
}

function pause(milliseconds: number): Promise<void> {
  return new Promise(resolve => setTimeout(resolve, milliseconds));
}

export async function closeSession(sessionId: string, keepalive = false): Promise<void> {
  const response = await fetch(sessionPath(sessionId), {
    method: 'DELETE', keepalive, signal: AbortSignal.timeout(10000)
  });
  if (response.status === 404 || response.status === 410) return;
  if (!response.ok) throw new Error(`Session close failed (HTTP ${response.status})`);
  const deadline = Date.now() + 15000;
  while (Date.now() < deadline) {
    const status = await fetch(sessionPath(sessionId), { signal: AbortSignal.timeout(3000) });
    if (status.status === 404 || status.status === 410) return;
    if (!status.ok) throw new Error(`Close confirmation failed (HTTP ${status.status})`);
    const session = parseSession(await status.json() as unknown);
    if (session.sessionId !== sessionId) throw new Error('Close identity mismatch');
    if (session.state === 'closed') return;
    await pause(300);
  }
  throw new Error('Session close timed out; creation remains locked until closure is confirmed');
}

export async function waitUntilReady(sessionId: string, cancelled: () => boolean): Promise<Session> {
  const deadline = Date.now() + 60000;
  while (Date.now() < deadline && !cancelled()) {
    const session = await getSession(sessionId);
    if (session.state === 'ready') return session;
    if (session.state !== 'loading') throw new Error(`Session ${session.state}`);
    await pause(350);
  }
  throw new Error(cancelled() ? 'Session request cancelled' : 'Volume loading timed out');
}

export function streamUrl(sessionId: string): string {
  const url = new URL(`${sessionPath(sessionId)}/stream`, location.href);
  url.protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
  return url.href;
}