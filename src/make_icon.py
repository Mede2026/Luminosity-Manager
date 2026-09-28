# Genere app.ico (soleil orange) et app_off.ico (gris) : icones lissees, 16 a 256 px.
import math, struct, zlib

def coverage(x, y, n):
    """Part du pixel (x, y) couverte par le soleil, et position relative du centre (pour le degrade)."""
    ss = 4
    hit = 0
    for sy in range(ss):
        for sx in range(ss):
            px = (x + (sx + 0.5) / ss) / n - 0.5
            py = (y + (sy + 0.5) / ss) / n - 0.5
            r = math.hypot(px, py)
            if r <= 0.215:
                hit += 1
                continue
            # 8 rayons en forme de capsule
            for k in range(8):
                a = k * math.pi / 4
                ux, uy = math.cos(a), math.sin(a)
                t = px * ux + py * uy
                t = min(max(t, 0.31), 0.44)
                if math.hypot(px - t * ux, py - t * uy) <= 0.042:
                    hit += 1
                    break
    return hit / (ss * ss)

def render(n, c1, c2):
    rows = []
    for y in range(n):
        row = []
        for x in range(n):
            a = coverage(x, y, n)
            g = y / max(n - 1, 1)            # degrade vertical
            col = [round(c1[i] + (c2[i] - c1[i]) * g) for i in range(3)]
            row.append((col[0], col[1], col[2], round(a * 255)))
        rows.append(row)
    return rows

def bmp_entry(rows):
    n = len(rows)
    xor = b''.join(bytes((b, g, r, a)) for row in reversed(rows) for (r, g, b, a) in row)
    andm = b'\x00' * (((n + 31) // 32 * 4) * n)
    hdr = struct.pack('<IiiHHIIiiII', 40, n, 2 * n, 1, 32, 0, len(xor) + len(andm), 0, 0, 0, 0)
    return hdr + xor + andm

def png_entry(rows):
    n = len(rows)
    raw = b''.join(b'\x00' + b''.join(bytes(p) for p in row) for row in rows)
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', n, n, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))

def write_ico(path, c1, c2):
    sizes = [16, 20, 24, 32, 40, 48, 64, 256]
    datas = [(png_entry if s >= 32 else bmp_entry)(render(s, c1, c2)) for s in sizes]
    out = struct.pack('<HHH', 0, 1, len(sizes))
    off = 6 + 16 * len(sizes)
    for s, d in zip(sizes, datas):
        out += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(d), off)
        off += len(d)
    open(path, 'wb').write(out + b''.join(datas))

write_ico('app.ico', (251, 191, 36), (245, 130, 11))
write_ico('app_off.ico', (170, 176, 186), (130, 136, 146))
