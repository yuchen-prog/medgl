import { defineConfig } from '@playwright/test';
import { fileURLToPath } from 'node:url';

process.env.PLAYWRIGHT_BROWSERS_PATH ??= fileURLToPath(new URL('node_modules/.cache/ms-playwright', import.meta.url));
const url = process.env.MEDGL_REAL_URL;
if (!url || new URL(url).hostname !== '127.0.0.1' || ['8080', '8081'].includes(new URL(url).port))
  throw new Error('Package tests require a dedicated loopback origin outside user ports');

export default defineConfig({
  testDir: './tests/package', outputDir: 'test-results-package', workers: 1,
  timeout: 300000, reporter: 'list', use: { baseURL: url, browserName: 'chromium', trace: 'retain-on-failure' }
});