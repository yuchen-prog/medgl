import { defineConfig } from '@playwright/test';
import { fileURLToPath } from 'node:url';

process.env.PLAYWRIGHT_BROWSERS_PATH ??= fileURLToPath(new URL('node_modules/.cache/ms-playwright', import.meta.url));
const projectRoot = fileURLToPath(new URL('..', import.meta.url));
const externalUrl = process.env.MEDGL_REAL_URL;
const pngCompression = process.env.MEDGL_PNG_COMPRESSION ?? 'default';
if (!['default', 'fast'].includes(pngCompression)) throw new Error('Invalid PNG benchmark mode');

export default defineConfig({
  testDir: './tests/integration',
  outputDir: 'test-results-real',
  timeout: 60000,
  workers: 1,
  reporter: 'list',
  use: {
    baseURL: externalUrl ?? 'http://127.0.0.1:8082',
    browserName: 'chromium',
    acceptDownloads: true,
    trace: 'retain-on-failure'
  },
  webServer: externalUrl ? undefined : {
    command: `out\\build\\v1-release\\medgl_server.exe --port 8082 --data-root out\\validation\\synthetic-ct --web-root web\\dist --png-compression ${pngCompression}`,
    cwd: projectRoot,
    url: 'http://127.0.0.1:8082/api/v1/health',
    reuseExistingServer: false,
    timeout: 30000
  }
});