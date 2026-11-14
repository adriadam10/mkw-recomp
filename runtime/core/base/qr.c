// QR codes (qr.h), written from ISO/IEC 18004: byte mode, versions 1 to 5. Those versions hold their codewords in a
// single Reed-Solomon block at the levels used here, so there is no interleaving, and below version 7 there is no
// version information. Level M up to version 3 (42 bytes: "https://192.168.100.200:65535/#1234" is 35), level L for
// versions 4 and 5. Checked module by module against python-qrcode for every mask (tests/runtime/test_phonepad.py's
// reference rows come from it).
#include <string.h>

#include "qr.h"

// version, the format's two level bits (M 00, L 01), data codewords, error correction codewords
typedef struct { uint8_t version, level, data, ec; } QrFit;
static const QrFit fits[] = {{1, 0, 16, 10}, {2, 0, 28, 16}, {3, 0, 44, 26}, {4, 1, 80, 20}, {5, 1, 108, 26}};

enum { MAX_CODEWORDS = 134, MAX_EC = 26 };

// GF(2^8) modulo x^8 + x^4 + x^3 + x^2 + 1
static uint8_t gf_mul(uint8_t a, uint8_t b) {
    unsigned r = 0;
    for (int i = 7; i >= 0; i--) {
        r = (r << 1) ^ ((r >> 7) * 0x11D);
        r ^= ((b >> i) & 1) * a;
    }
    return (uint8_t)r;
}

// The n error correction codewords of data: the remainder of data * x^n by the generator (x - 2^0)...(x - 2^(n-1))
static void reed_solomon(const uint8_t *data, int len, int n, uint8_t *out) {
    uint8_t gen[MAX_EC] = {0};   // the generator's coefficients below its leading 1, highest power first
    gen[n - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            gen[j] = gf_mul(gen[j], root);
            if (j + 1 < n) gen[j] ^= gen[j + 1];
        }
        root = gf_mul(root, 2);
    }
    memset(out, 0, (size_t)n);
    for (int i = 0; i < len; i++) {
        uint8_t f = data[i] ^ out[0];
        memmove(out, out + 1, (size_t)n - 1);
        out[n - 1] = 0;
        for (int j = 0; j < n; j++) out[j] ^= gf_mul(gen[j], f);
    }
}

typedef struct {
    int size;
    uint8_t *dark;                               // the caller's modules
    uint8_t fixed[QR_MAX_SIZE * QR_MAX_SIZE];    // a function module: no data, no mask
} Qr;

static void put(Qr *q, int x, int y, int dark) {
    if (x < 0 || y < 0 || x >= q->size || y >= q->size) return;
    q->dark[y * q->size + x] = (uint8_t)dark;
    q->fixed[y * q->size + x] = 1;
}

static int iabs(int v) { return v < 0 ? -v : v; }
static int ring(int dx, int dy) { return iabs(dx) > iabs(dy) ? iabs(dx) : iabs(dy); }

// A finder pattern (7x7: dark, light, 3x3 dark) with its light separator, centred on (cx, cy)
static void finder(Qr *q, int cx, int cy) {
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++) put(q, cx + dx, cy + dy, ring(dx, dy) != 2 && ring(dx, dy) != 4);
}

// The 15 format bits (level and mask, BCH(15,5), masked with 101010000010010), twice: around the top left finder,
// and split between the other two
static void format_bits(Qr *q, int level, int mask) {
    unsigned data = (unsigned)(level << 3 | mask), rem = data;
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    unsigned bits = (data << 10 | rem) ^ 0x5412;
    for (int i = 0; i < 15; i++) {
        int b = (bits >> i) & 1, s = q->size;
        if (i < 6) put(q, 8, i, b);
        else if (i == 6) put(q, 8, 7, b);
        else if (i == 7) put(q, 8, 8, b);
        else if (i == 8) put(q, 7, 8, b);
        else put(q, 14 - i, 8, b);
        if (i < 8) put(q, s - 1 - i, 8, b);
        else put(q, 8, s - 15 + i, b);
    }
    put(q, 8, q->size - 8, 1);   // the module that is always dark
}

static int mask_bit(int mask, int x, int y) {
    switch (mask) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return x * y % 2 + x * y % 3 == 0;
    case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
    default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

static void apply_mask(Qr *q, int mask) {
    for (int y = 0; y < q->size; y++)
        for (int x = 0; x < q->size; x++)
            if (!q->fixed[y * q->size + x]) q->dark[y * q->size + x] ^= (uint8_t)mask_bit(mask, x, y);
}

// One line's share of the first and third penalties: runs of five or more modules of one colour (3, and 1 more per
// module past five), and each finder-like 1:1:3:1:1 run with four light modules on one side (40)
static int line_penalty(const uint8_t *line, int n) {
    int p = 0, run = 1;
    for (int i = 1; i <= n; i++) {
        if (i < n && line[i] == line[i - 1]) {
            run++;
            continue;
        }
        if (run >= 5) p += run - 2;
        run = 1;
    }
    static const uint8_t a[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0}, b[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
    for (int i = 0; i + 11 <= n; i++)
        if (!memcmp(line + i, a, 11) || !memcmp(line + i, b, 11)) p += 40;
    return p;
}

// The penalty a reader's work is estimated by (ISO/IEC 18004, 7.8.3): the lower, the easier the code is to read
static int penalty(const Qr *q) {
    int s = q->size, p = 0, dark = 0;
    uint8_t col[QR_MAX_SIZE];
    for (int y = 0; y < s; y++) p += line_penalty(q->dark + y * s, s);
    for (int x = 0; x < s; x++) {
        for (int y = 0; y < s; y++) col[y] = q->dark[y * s + x];
        p += line_penalty(col, s);
    }
    for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++) {
            int c = q->dark[y * s + x];
            dark += c;
            if (x + 1 < s && y + 1 < s && c == q->dark[y * s + x + 1] && c == q->dark[(y + 1) * s + x] &&
                c == q->dark[(y + 1) * s + x + 1])
                p += 3;   // a 2x2 block of one colour
        }
    return p + 10 * (iabs(dark * 20 - s * s * 10) / (s * s));   // 10 per 5 % away from half dark
}

int qr_encode(const char *text, int mask, uint8_t modules[QR_MAX_SIZE * QR_MAX_SIZE]) {
    size_t len = strlen(text);
    const QrFit *fit = NULL;
    for (size_t i = 0; i < sizeof fits / sizeof *fits && !fit; i++)
        if (len + 2 <= fits[i].data) fit = &fits[i];   // 4 bits of mode and 8 of length, and room for the terminator
    if (!fit) return 0;

    // the codewords: mode 0100 (bytes), the length, the bytes, a terminator, then the pad bytes
    uint8_t cw[MAX_CODEWORDS] = {0};
    cw[0] = (uint8_t)(0x40 | len >> 4);
    cw[1] = (uint8_t)(len << 4);
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)text[i];
        cw[1 + i] |= c >> 4;
        cw[2 + i] = (uint8_t)(c << 4);
    }
    for (size_t i = len + 2, pad = 0; i < fit->data; i++, pad ^= 1) cw[i] = pad ? 0x11 : 0xEC;
    reed_solomon(cw, fit->data, fit->ec, cw + fit->data);
    int bits = (fit->data + fit->ec) * 8;

    Qr q = {.size = 17 + 4 * fit->version, .dark = modules};
    int s = q.size;
    memset(modules, 0, QR_MAX_SIZE * QR_MAX_SIZE);
    for (int i = 0; i < s; i++) put(&q, i, 6, i % 2 == 0), put(&q, 6, i, i % 2 == 0);   // the timing patterns
    finder(&q, 3, 3), finder(&q, s - 4, 3), finder(&q, 3, s - 4);
    if (fit->version > 1)   // one alignment pattern: 5x5, dark, light, a dark centre
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++) put(&q, s - 7 + dx, s - 7 + dy, ring(dx, dy) != 1);
    format_bits(&q, fit->level, 0);   // reserves their modules

    // the codewords' bits, most significant first, in column pairs from the right edge, up then down, skipping the
    // vertical timing pattern's column
    int bit = 0, up = 1;
    for (int right = s - 1; right >= 1; right -= 2, up = !up) {
        if (right == 6) right = 5;
        for (int v = 0; v < s; v++)
            for (int j = 0; j < 2; j++) {
                int x = right - j, y = up ? s - 1 - v : v;
                if (q.fixed[y * s + x] || bit >= bits) continue;
                modules[y * s + x] = (cw[bit >> 3] >> (7 - (bit & 7))) & 1;
                bit++;
            }
    }

    if (mask < 0 || mask > 7) {
        int best = 0;
        for (int m = 0; m < 8; m++) {
            apply_mask(&q, m);
            format_bits(&q, fit->level, m);
            int p = penalty(&q);
            if (m == 0 || p < best) best = p, mask = m;
            apply_mask(&q, m);   // the same mask again takes it off
        }
    }
    apply_mask(&q, mask);
    format_bits(&q, fit->level, mask);
    return s;
}
