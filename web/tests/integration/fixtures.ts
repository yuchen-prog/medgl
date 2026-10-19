import { expect, test as base } from '@playwright/test';
import type { Response } from '@playwright/test';

export const test = base.extend<{ sessionCleanup: void }>({
  sessionCleanup: [async ({ page, request }, runTest) => {
    const sessions = new Set<string>();
    const pending = new Set<Promise<void>>();
    const capture = (response: Response) => {
      if (response.request().method() !== 'POST' || response.status() !== 202 ||
          new URL(response.url()).pathname !== '/api/v1/sessions') return;
      const task = response.json().then((body: { sessionId?: unknown }) => {
        if (typeof body.sessionId === 'string') sessions.add(body.sessionId);
      }).catch(() => undefined);
      pending.add(task);
      void task.finally(() => pending.delete(task));
    };
    page.on('response', capture);
    try { await runTest(); }
    finally {
      await page.context().setOffline(false);
      if (!page.isClosed()) await page.goto('about:blank');
      await Promise.all(pending);
      page.off('response', capture);
      for (const session of sessions) {
        const path = `/api/v1/sessions/${encodeURIComponent(session)}`;
        const deletion = await request.delete(path);
        expect([202, 404, 410]).toContain(deletion.status());
        await deletion.dispose();
        await expect.poll(async () => {
          const response = await request.get(path);
          try {
            if (response.status() === 404 || response.status() === 410) return true;
            return response.status() === 200 && (await response.json() as { state: string }).state === 'closed';
          } finally { await response.dispose(); }
        }, { timeout: 30000, intervals: [50, 100, 250] }).toBe(true);
      }
    }
  }, { auto: true }]
});