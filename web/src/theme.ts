export type Theme = 'light' | 'dark';

export const THEME_STORAGE_KEY = 'medgl.theme.v1';

export function readTheme(): Theme {
  try { return localStorage.getItem(THEME_STORAGE_KEY) === 'dark' ? 'dark' : 'light'; }
  catch { return 'light'; }
}

export function applyTheme(theme: Theme): void {
  document.documentElement.dataset.theme = theme;
  document.querySelector('meta[name="theme-color"]')?.setAttribute('content', theme === 'dark' ? '#161a1a' : '#f6f8f8');
}

export function saveTheme(theme: Theme): boolean {
  try { localStorage.setItem(THEME_STORAGE_KEY, theme); return true; }
  catch { return false; }
}

export function initializeTheme(): void {
  applyTheme(readTheme());
}