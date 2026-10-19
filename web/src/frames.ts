import { parseFramePacket } from './protocol';
import type { FrameMeta } from './protocol';

export interface FrameTarget {
  sessionId: string;
  revision: number;
  width: number;
  height: number;
  issuedSeq: number;
}

export class FramePresenter {
  latestDecodedSeq = 0;
  private active = true;

  constructor(
    private readonly target: () => FrameTarget | null,
    private readonly canvas: () => HTMLCanvasElement | null,
    private readonly presented: (meta: FrameMeta) => void,
    private readonly decode: (blob: Blob) => Promise<ImageBitmap> = blob => createImageBitmap(blob)
  ) {}

  private matches(meta: FrameMeta): boolean {
    const target = this.target();
    return this.active && target !== null && meta.sessionId === target.sessionId &&
      meta.sceneRevision === target.revision && meta.seq <= target.issuedSeq &&
      meta.seq > this.latestDecodedSeq && meta.width === target.width && meta.height === target.height;
  }

  async accept(buffer: ArrayBuffer): Promise<void> {
    const receivedAt = performance.now();
    const { meta, image } = parseFramePacket(buffer);
    if (!this.matches(meta)) return;
    const decodeStart = performance.now();
    const bitmap = await this.decode(image);
    const decodedAt = performance.now();
    try {
      if (!this.matches(meta)) return;
      if (bitmap.width !== meta.width || bitmap.height !== meta.height) throw new Error('Decoded image dimensions mismatch');
      const canvas = this.canvas();
      const context = canvas?.getContext('2d');
      if (!canvas || !context) return;
      const drawStart = performance.now();
      context.drawImage(bitmap, 0, 0, canvas.width, canvas.height);
      const drawnAt = performance.now();
      this.latestDecodedSeq = meta.seq;
      this.presented({ ...meta, presentationTimings: { decodeMs: decodedAt - decodeStart,
        drawMs: drawnAt - drawStart, receiveToDrawMs: drawnAt - receivedAt } });
    } finally {
      bitmap.close();
    }
  }

  dispose(): void {
    this.active = false;
  }
}