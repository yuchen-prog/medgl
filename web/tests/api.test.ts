import { afterEach, describe, expect, it, vi } from 'vitest';
import { appearancePresets, closeSession, createSession, getSession, streamUrl, waitUntilReady } from '../src/api';

afterEach(() => { vi.unstubAllGlobals(); vi.useRealTimers(); });
const response = (body: unknown, status = 200) => new Response(JSON.stringify(body), { status });

describe('HTTP session adapter', () => {
  it('loads appearance defaults from the server catalog', async () => {
    const preset = { id: 'ct-skin', name: 'CT / Skin', windowWidth: 800, windowLevel: -200,
      opacityScale: 1, lighting: true, colors: [[0, 0, 0], [1, 0.8, 0.6]] };
    const fetch = vi.fn().mockResolvedValue(response({ presets: [preset] }));
    vi.stubGlobal('fetch', fetch);
    expect(await appearancePresets()).toEqual([preset]);
    expect(fetch).toHaveBeenCalledWith('/api/v1/presets', expect.any(Object));
  });
  it('posts the exact series contract and accepts HTTP 202', async () => {
    const fetch = vi.fn().mockResolvedValue(response({ sessionId: 'one', state: 'loading' }, 202));
    vi.stubGlobal('fetch', fetch);
    expect(await createSession('phantom')).toEqual({ sessionId: 'one', state: 'loading' });
    expect(fetch).toHaveBeenCalledWith('/api/v1/sessions', expect.objectContaining({
      method: 'POST', body: '{"seriesId":"phantom"}', headers: { 'Content-Type': 'application/json' }
    }));
  });

  it('surfaces HTTP errors and rejects mismatched session identity', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(new Response('Not available', { status: 503 }))
      .mockResolvedValueOnce(response({ sessionId: 'other', state: 'loading' })));
    await expect(getSession('one')).rejects.toThrow('HTTP 503');
    await expect(getSession('one')).rejects.toThrow('identity');
  });

  it('polls after accepted DELETE and resolves only when closed', async () => {
    vi.useFakeTimers();
    const fetch = vi.fn().mockResolvedValueOnce(response({ state: 'closing' }, 202))
      .mockResolvedValueOnce(response({ sessionId: 'one', state: 'closing' }))
      .mockResolvedValueOnce(response({ sessionId: 'one', state: 'closed' }));
    vi.stubGlobal('fetch', fetch);
    let finished = false;
    const task = closeSession('one').then(() => { finished = true; });
    await vi.advanceTimersByTimeAsync(1);
    expect(finished).toBe(false);
    await vi.advanceTimersByTimeAsync(300);
    await task;
    expect(finished).toBe(true);
    expect(fetch).toHaveBeenCalledTimes(3);
  });

  it('rejects close timeouts instead of pretending ownership was released', async () => {
    vi.useFakeTimers();
    vi.stubGlobal('fetch', vi.fn().mockImplementation(async () => response({ sessionId: 'one', state: 'closing' })));
    const task = expect(closeSession('one')).rejects.toThrow('creation remains locked');
    await vi.advanceTimersByTimeAsync(16000);
    await task;
  });

  it.each([404, 410])('treats HTTP %i as a confirmed absence', async status => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response(null, { status })));
    await expect(closeSession('one', true)).resolves.toBeUndefined();
  });

  it('does not poll a cancelled load', async () => {
    const fetch = vi.fn();
    vi.stubGlobal('fetch', fetch);
    await expect(waitUntilReady('one', () => true)).rejects.toThrow('cancelled');
    expect(fetch).not.toHaveBeenCalled();
  });

  it('uses same-origin TLS and URL-encodes session IDs', () => {
    vi.stubGlobal('location', { href: 'https://medgl.example/', protocol: 'https:' });
    expect(streamUrl('id/with space')).toBe('wss://medgl.example/api/v1/sessions/id%2Fwith%20space/stream');
  });
});