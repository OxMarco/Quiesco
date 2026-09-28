// Little-endian readers and writers for the Quiesco wire formats
// (firmware/src/protocol/LittleEndian.h). Every multi-byte field is LE.

export class ByteReader {
  private readonly view: DataView;

  constructor(readonly bytes: Uint8Array) {
    this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  }

  get length(): number {
    return this.bytes.byteLength;
  }

  u8(offset: number): number {
    return this.view.getUint8(offset);
  }
  u16(offset: number): number {
    return this.view.getUint16(offset, true);
  }
  i16(offset: number): number {
    return this.view.getInt16(offset, true);
  }
  u32(offset: number): number {
    return this.view.getUint32(offset, true);
  }
  f32(offset: number): number {
    return this.view.getFloat32(offset, true);
  }
  // u64 as a JS number: exact below 2^53, which covers every u64 the protocol
  // sends (epoch ms, ms since boot).
  u64(offset: number): number {
    return this.u32(offset) + this.u32(offset + 4) * 2 ** 32;
  }
  // u48 (SCD41 serial) as a number: always exact.
  u48(offset: number): number {
    return this.u32(offset) + this.u16(offset + 4) * 2 ** 32;
  }
}

export class ByteWriter {
  readonly bytes: Uint8Array;
  private readonly view: DataView;

  constructor(length: number) {
    this.bytes = new Uint8Array(length);
    this.view = new DataView(this.bytes.buffer);
  }

  u8(offset: number, value: number): this {
    this.view.setUint8(offset, value);
    return this;
  }
  u16(offset: number, value: number): this {
    this.view.setUint16(offset, value, true);
    return this;
  }
  u32(offset: number, value: number): this {
    this.view.setUint32(offset, value, true);
    return this;
  }
  f32(offset: number, value: number): this {
    this.view.setFloat32(offset, value, true);
    return this;
  }
  u64(offset: number, value: number): this {
    this.view.setUint32(offset, value % 2 ** 32, true);
    this.view.setUint32(offset + 4, Math.floor(value / 2 ** 32), true);
    return this;
  }
}

export function toHex(bytes: Uint8Array): string {
  return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join(' ');
}

export function fromHex(hex: string): Uint8Array {
  const clean = hex.replace(/[^0-9a-f]/gi, '');
  const out = new Uint8Array(clean.length / 2);
  for (let i = 0; i < out.length; i++) {
    out[i] = parseInt(clean.slice(i * 2, i * 2 + 2), 16);
  }
  return out;
}
