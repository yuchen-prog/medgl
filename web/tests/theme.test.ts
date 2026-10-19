import { afterEach, describe, expect, it, vi } from 'vitest';
import { applyTheme, initializeTheme, readTheme, saveTheme, THEME_STORAGE_KEY } from '../src/theme';

afterEach(() => vi.unstubAllGlobals());

describe('workstation theme preference', () => {
  it('defaults to light and only accepts known saved values', () => {
    for (const value of [null, 'light', 'system', 'invalid', '{broken']) {
      vi.stubGlobal('localStorage', { getItem: () => value });
      expect(readTheme()).toBe('light');
    }
    vi.stubGlobal('localStorage', { getItem: () => 'dark' });
    expect(readTheme()).toBe('dark');
  });

  it('changes only the document theme and browser chrome color', () => {
    const dataset: Record<string, string> = {};
    const setAttribute = vi.fn();
    const querySelector = vi.fn(() => ({ setAttribute }));
    vi.stubGlobal('document', { documentElement: { dataset }, querySelector });
    applyTheme('dark');
    expect(dataset.theme).toBe('dark');
    expect(querySelector).toHaveBeenCalledExactlyOnceWith('meta[name="theme-color"]');
    expect(setAttribute).toHaveBeenLastCalledWith('content', '#161a1a');
    applyTheme('light');
    expect(dataset.theme).toBe('light');
    expect(setAttribute).toHaveBeenLastCalledWith('content', '#f6f8f8');
  });

  it('initializes the saved theme and stores only a preference string', () => {
    const dataset: Record<string, string> = {};
    const setItem = vi.fn();
    vi.stubGlobal('document', { documentElement: { dataset }, querySelector: () => null });
    vi.stubGlobal('localStorage', { getItem: () => 'dark', setItem });
    initializeTheme();
    expect(dataset.theme).toBe('dark');
    expect(saveTheme('light')).toBe(true);
    expect(setItem).toHaveBeenCalledExactlyOnceWith(THEME_STORAGE_KEY, 'light');
  });

  it('keeps theme switching usable if browser storage is unavailable', () => {
    vi.stubGlobal('localStorage', { getItem: () => { throw new Error('denied'); },
      setItem: () => { throw new Error('quota'); } });
    expect(readTheme()).toBe('light');
    expect(saveTheme('dark')).toBe(false);
  });
});