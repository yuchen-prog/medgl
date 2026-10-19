import { defineConfig } from '@playwright/test';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('.', import.meta.url));
const vite = fileURLToPath(new URL('node_modules/vite/bin/vite.js', import.meta.url));
process.env.PLAYWRIGHT_BROWSERS_PATH ??= fileURLToPath(new URL('node_modules/.cache/ms-playwright', import.meta.url));

export default defineConfig({
  testDir: './tests/browser',
  timeout: 30000,
  fullyParallel: false,
  workers: 1,
  reporter: 'list',
  use: { baseURL: 'http://127.0.0.1:5178', browserName: 'chromium', trace: 'retain-on-failure' },
  webServer: {
    command: `"${process.execPath}" "${vite}" --port 5178 --strictPort`,
    cwd: root,
    url: 'http://127.0.0.1:5178',
    reuseExistingServer: false,
    timeout: 15000
  }
});