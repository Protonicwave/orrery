/**
 * Where a trajectory's bytes come from.
 *
 * One interface with two implementations: a ranged fetch for the published
 * gallery, and a plain buffer for the tests. The reader above it never learns
 * which it has, which is what lets the reader be tested against a file the C++
 * wrote without a server being started to serve it.
 */

/** A range of a file, and how long the whole file turned out to be. */
export interface Chunk {
  readonly bytes: Uint8Array;
  /** The length of the whole resource, not of this chunk. */
  readonly totalLength: number;
}

export interface ByteSource {
  /** A name for the thing being read, for a message when it fails. */
  readonly name: string;
  /**
   * Bytes `[start, end)`. May return fewer at the end of the file, and never
   * returns more.
   */
  read(start: number, end: number): Promise<Chunk>;
}

/** A source that cannot be read, with the reason in the message. */
export class SourceError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'SourceError';
  }
}

/**
 * A source over an ArrayBuffer, for tests and for a file dropped onto the page.
 */
export function bufferSource(buffer: ArrayBuffer, name = 'a buffer'): ByteSource {
  const bytes = new Uint8Array(buffer);
  return {
    name,
    read(start, end) {
      return Promise.resolve({
        bytes: bytes.subarray(start, Math.min(end, bytes.length)),
        totalLength: bytes.length,
      });
    },
  };
}

/**
 * A source over HTTP, one ranged request per call.
 *
 * The total length comes out of the `Content-Range` header of the first
 * response rather than out of a separate `HEAD`, which is one request rather
 * than two and, more usefully, is the length of the representation actually
 * being served rather than the length of one that a second request might have
 * received instead.
 *
 * A server that ignores the range and answers 200 with the whole file is not an
 * error: the response is sliced to the range that was asked for and the length
 * comes from `Content-Length`. It costs the download this reader exists to
 * avoid, so it is worth knowing about, but a client that refused would be
 * refusing a correct response.
 */
export function fetchSource(url: string): ByteSource {
  return {
    name: url,

    async read(start, end) {
      const response = await fetch(url, {
        headers: { Range: `bytes=${start}-${end - 1}` },
      });

      if (!response.ok) {
        throw new SourceError(
          `${url} answered ${response.status} ${response.statusText}`,
        );
      }

      const buffer = await response.arrayBuffer();
      const range = response.headers.get('Content-Range');

      if (response.status === 206 && range !== null) {
        // `bytes 0-65535/38452048`, of which the part after the solidus is what
        // is wanted. A server may answer `*` there, in which case the length is
        // not yet known and the caller is told so.
        const total = Number.parseInt(range.slice(range.indexOf('/') + 1), 10);
        return {
          bytes: new Uint8Array(buffer),
          totalLength: Number.isFinite(total) ? total : Number.NaN,
        };
      }

      const length = Number(
        response.headers.get('Content-Length') ?? buffer.byteLength,
      );
      return {
        bytes: new Uint8Array(buffer).subarray(start, end),
        totalLength: Number.isFinite(length) ? length : buffer.byteLength,
      };
    },
  };
}
