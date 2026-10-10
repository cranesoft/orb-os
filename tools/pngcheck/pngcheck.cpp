// Decode PNGs with PNGdec, the firmware's own decoder, and print each as raw RGBA so a
// script can compare it with a reference decoder. Built by tools/bigorb_theme.py, which
// refuses to ship a PNG the Orb would draw differently from how the artist saved it.
//
//   pngcheck <in.png> <out.rgba>     prints "W H" and writes W*H*4 bytes
#include <PNGdec.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

static PNG png;
static std::vector<uint8_t> out;
static int W = 0;

static int line_cb(PNGDRAW *d) {
    uint8_t *dst = out.data() + (size_t)d->y * W * 4;
    if (d->iPixelType == PNG_PIXEL_TRUECOLOR_ALPHA && d->iBpp == 8) {
        for (int x = 0; x < d->iWidth * 4; ++x) dst[x] = d->pPixels[x];
    } else if (d->iPixelType == PNG_PIXEL_TRUECOLOR && d->iBpp == 8) {
        for (int x = 0; x < d->iWidth; ++x) {
            dst[x * 4] = d->pPixels[x * 3]; dst[x * 4 + 1] = d->pPixels[x * 3 + 1];
            dst[x * 4 + 2] = d->pPixels[x * 3 + 2]; dst[x * 4 + 3] = 255;
        }
    } else {
        return 0;   // the firmware only reads 8-bit RGB/RGBA; anything else is a failure here too
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: pngcheck in.png out.rgba\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    std::vector<uint8_t> data;
    uint8_t buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) data.insert(data.end(), buf, buf + n);
    fclose(f);
    if (png.openRAM(data.data(), (int)data.size(), line_cb) != PNG_SUCCESS) { fprintf(stderr, "open failed\n"); return 1; }
    W = png.getWidth();
    const int H = png.getHeight();
    out.assign((size_t)W * H * 4, 0);
    const int r = png.decode(nullptr, 0);
    png.close();
    if (r != PNG_SUCCESS) { fprintf(stderr, "decode failed (%d)\n", r); return 1; }
    FILE *o = fopen(argv[2], "wb");
    fwrite(out.data(), 1, out.size(), o);
    fclose(o);
    printf("%d %d\n", W, H);
    return 0;
}
