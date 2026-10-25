#include "WoW/Source/WorldClient/TerrainHeightGen.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

// make_heightmaps.py, step for step (names follow it). Masonry and cobble are found by their joints forming one
// thin web (a groove score joins up at 25-30 % of the pixels, noise only near 40-50 %) and get a dome per stone
// from the distance to the nearest joint; everything else gets a gentle smoothed luminance relief within its
// class range. Works at min(256, side) px, hands back min(128, side). Arrays are n x n doubles, row major; the
// texture tiles, so blurs wrap and labels / distances are taken on the 3 x 3 tiling with the centre cut back out.

namespace TerrainHeightGen {

namespace {

typedef std::vector<double> Img;

const int SIZE = 128;
const double FRAC = 0.6;
const double THIN = 12.0;

struct Range {
    double lo;
    double hi;
};

// CLASSES, first match wins; the soft (no joint web) relief of each beside it
struct Class {
    const char* words[16];
    Range range;
    Range soft;
};

const Class s_classes[] = {
    { { "cobble", "brick", "stone", "rock", "tile", "flag", "pave", "cliff", "boulder", "slate", "marble", "crystal", "cobbl", "block", "wall", "floor" }, { 0.35, 1.0 }, { 0.05, 0.95 } },
    { { "grass", "moss", "leaves", "leaf", "flower", "weed", "fern", "vine", "needle", "ivy", "bush", "shrub", "clover" }, { 0.0, 0.65 }, { 0.15, 0.7 } },
    { { "dirt", "sand", "mud", "snow", "ash", "dust", "soil", "clay", "ice", "beach", "silt" }, { 0.0, 0.5 }, { 0.2, 0.6 } },
};
const Range s_defaultSoft = { 0.1, 0.8 };

// index into s_classes, or -1 for the default range
int ClassOf(const char* name) {
    std::string base(name ? name : "");

    for (char& c : base) {
        c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : (c == '/' ? '\\' : c);
    }

    size_t slash = base.rfind('\\');

    if (slash != std::string::npos) {
        base = base.substr(slash + 1);
    }

    for (int i = 0; i < 3; i++) {
        for (const char* word : s_classes[i].words) {
            if (word && base.find(word) != std::string::npos) {
                return i;
            }
        }
    }

    return -1;
}

// in-place radix-2 FFT of re / im, n a power of two; inverse unscaled. Plain doubles: std::complex's operator*
// goes through the NaN-checking __muldc3 and took the blur from 1 to 3 ms
void Fft(double* re, double* im, int n, bool inverse, const std::vector<double>& cosTab, const std::vector<double>& sinTab) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;

        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }

        j ^= bit;

        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }

    double sign = inverse ? 1.0 : -1.0;

    for (int len = 2; len <= n; len <<= 1) {
        int step = n / len;

        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < len / 2; k++) {
                double wr = cosTab[k * step];
                double wi = sign * sinTab[k * step];
                int a = i + k;
                int b = a + len / 2;
                double vr = re[b] * wr - im[b] * wi;
                double vi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - vr;
                im[b] = im[a] - vi;
                re[a] += vr;
                im[a] += vi;
            }
        }
    }
}

// blur_wrap: the FFT Gaussian. The 2D filter is a product of two 1D ones, so rows then columns; two real rows
// ride in one complex FFT (the filter is real and even, so they come back apart in re / im)
Img Blur(const Img& img, int n, double sigma) {
    std::vector<double> filter(n);

    for (int j = 0; j < n; j++) {
        double f = static_cast<double>(j < (n + 1) / 2 ? j : j - n) / n;
        filter[j] = std::exp(-2.0 * (3.14159265358979323846 * sigma) * (3.14159265358979323846 * sigma) * f * f) / n;
    }

    std::vector<double> cosTab(n / 2);
    std::vector<double> sinTab(n / 2);

    for (int j = 0; j < n / 2; j++) {
        cosTab[j] = std::cos(2.0 * 3.14159265358979323846 * j / n);
        sinTab[j] = std::sin(2.0 * 3.14159265358979323846 * j / n);
    }

    Img out(img);
    std::vector<double> re(n);
    std::vector<double> im(n);

    for (int pass = 0; pass < 2; pass++) {
        // pass 0 rows, pass 1 columns
        for (int r = 0; r < n; r += 2) {
            for (int i = 0; i < n; i++) {
                size_t a = pass ? static_cast<size_t>(i) * n + r : static_cast<size_t>(r) * n + i;
                size_t b = pass ? a + 1 : a + n;
                re[i] = out[a];
                im[i] = r + 1 < n ? out[b] : 0.0;
            }

            Fft(re.data(), im.data(), n, false, cosTab, sinTab);

            for (int i = 0; i < n; i++) {
                re[i] *= filter[i];
                im[i] *= filter[i];
            }

            Fft(re.data(), im.data(), n, true, cosTab, sinTab);

            for (int i = 0; i < n; i++) {
                size_t a = pass ? static_cast<size_t>(i) * n + r : static_cast<size_t>(r) * n + i;
                size_t b = pass ? a + 1 : a + n;
                out[a] = re[i];

                if (r + 1 < n) {
                    out[b] = im[i];
                }
            }
        }
    }

    return out;
}

// np.percentile, linear
double Percentile(std::vector<double> v, double p) {
    double pos = p / 100.0 * (v.size() - 1);
    size_t lo = static_cast<size_t>(pos);
    std::nth_element(v.begin(), v.begin() + lo, v.end());
    double a = v[lo];

    if (lo + 1 >= v.size()) {
        return a;
    }

    double b = *std::min_element(v.begin() + lo + 1, v.end());
    return a + (b - a) * (pos - lo);
}

Img Stretch(const Img& x, double a = 2.0, double b = 98.0) {
    double lo = Percentile(x, a);
    double hi = Percentile(x, b);
    double span = std::max(hi - lo, 1e-6);
    Img out(x.size());

    for (size_t i = 0; i < x.size(); i++) {
        out[i] = std::min(std::max((x[i] - lo) / span, 0.0), 1.0);
    }

    return out;
}

Img ZScore(const Img& x) {
    double mean = 0.0;
    double var = 0.0;

    for (double v : x) {
        mean += v;
    }

    mean /= x.size();

    for (double v : x) {
        var += (v - mean) * (v - mean);
    }

    double sd = std::sqrt(var / x.size()) + 1e-9;
    Img out(x.size());

    for (size_t i = 0; i < x.size(); i++) {
        out[i] = (x[i] - mean) / sd;
    }

    return out;
}

// valley for both polarities from one blur: the larger Hessian eigenvalue of +-blurred luminance, times s^2
void Valley(const Img& lum, int n, double s, Img& dark, Img& bright) {
    Img g = Blur(lum, n, s);
    dark.resize(g.size());
    bright.resize(g.size());

    for (int y = 0; y < n; y++) {
        int ym = (y + n - 1) % n;
        int yp = (y + 1) % n;

        for (int x = 0; x < n; x++) {
            int xm = (x + n - 1) % n;
            int xp = (x + 1) % n;
            double c = g[y * n + x];
            double gxx = g[y * n + xp] - 2.0 * c + g[y * n + xm];
            double gyy = g[yp * n + x] - 2.0 * c + g[ym * n + x];
            double gxy = (g[yp * n + xp] - g[yp * n + xm] - g[ym * n + xp] + g[ym * n + xm]) / 4.0;
            double root = std::sqrt((gxx - gyy) / 2.0 * (gxx - gyy) / 2.0 + gxy * gxy);
            dark[y * n + x] = ((gxx + gyy) / 2.0 + root) * s * s;
            bright[y * n + x] = (-(gxx + gyy) / 2.0 + root) * s * s;
        }
    }
}

// gap_score, normalised by the local contrast so dim corners keep their joints
Img GapScore(const Img& hpBlur, const Img& v1, const Img& v2, int n, int pol) {
    Img a(hpBlur.size());
    Img b(hpBlur.size());

    for (size_t i = 0; i < a.size(); i++) {
        a[i] = -pol * hpBlur[i];
        b[i] = v1[i] + v2[i];
    }

    a = ZScore(a);
    b = ZScore(b);
    Img g(a.size());

    for (size_t i = 0; i < g.size(); i++) {
        g[i] = a[i] + 2.0 * b[i];
    }

    Img m = Blur(g, n, n / 10.0);
    Img dev(g.size());

    for (size_t i = 0; i < g.size(); i++) {
        dev[i] = (g[i] - m[i]) * (g[i] - m[i]);
    }

    dev = Blur(dev, n, n / 10.0);

    for (size_t i = 0; i < g.size(); i++) {
        g[i] = (g[i] - m[i]) / std::sqrt(std::max(dev[i], 1e-6));
    }

    return g;
}

// root of i in a union-find whose roots hold -size and others parent index + 1 (0 = not in the set)
int32_t Root(std::vector<int32_t>& uf, int32_t i) {
    int32_t root = i;

    while (uf[root] > 0) {
        root = uf[root] - 1;
    }

    while (uf[i] > 0 && uf[i] - 1 != root) {
        int32_t next = uf[i] - 1;
        uf[i] = root + 1;
        i = next;
    }

    return root;
}

// percolation: the smallest top fraction (%) of g whose largest 4-connected piece (on the 3 x 3 tiling) holds
// over half of it. One pass: pixels go in from the top, union-find on the tiling, checked at each threshold
int Percolation(const Img& g, int n) {
    int w = 3 * n;
    // (value, pixel), highest first: the order pixels go in and, read backwards, the sorted values for percentiles
    std::vector<std::pair<double, int32_t>> order(g.size());

    for (size_t i = 0; i < order.size(); i++) {
        order[i] = std::make_pair(-g[i], static_cast<int32_t>(i));
    }

    std::sort(order.begin(), order.end());
    size_t m = order.size();

    std::vector<int32_t> uf(static_cast<size_t>(w) * w, 0);
    int32_t largest = 0;
    size_t added = 0;

    for (int q = 10; q < 62; q += 2) {
        double pos = (100.0 - q) / 100.0 * (m - 1);
        size_t lo = static_cast<size_t>(pos);
        double low = -order[m - 1 - lo].first;
        double t = low + (lo + 1 < m ? (-order[m - 2 - lo].first - low) * (pos - lo) : 0.0);

        for (; added < m && -order[added].first > t; added++) {
            int bx = order[added].second % n;
            int by = order[added].second / n;

            for (int ty = 0; ty < 3; ty++) {
                for (int tx = 0; tx < 3; tx++) {
                    int x = bx + tx * n;
                    int y = by + ty * n;
                    int32_t p = y * w + x;
                    uf[p] = -1;
                    largest = std::max(largest, 1);
                    const int nx[4] = { x - 1, x + 1, x, x };
                    const int ny[4] = { y, y, y - 1, y + 1 };

                    for (int k = 0; k < 4; k++) {
                        if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= w || !uf[ny[k] * w + nx[k]]) {
                            continue;
                        }

                        int32_t a = Root(uf, p);
                        int32_t b = Root(uf, ny[k] * w + nx[k]);

                        if (a != b) {
                            if (uf[a] > uf[b]) {
                                std::swap(a, b);
                            }

                            uf[a] += uf[b];
                            uf[b] = a + 1;
                            largest = std::max(largest, -uf[a]);
                        }
                    }
                }
            }
        }

        if (added && largest > 0.5 * 9.0 * added) {
            return q;
        }
    }

    return 62;
}

int Find(std::vector<int32_t>& parent, int32_t i) {
    while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }

    return i;
}

// nd.label (4-connected) of a w x w mask: label per pixel (0 = background), returns the count
int Label(const std::vector<uint8_t>& mask, int w, std::vector<int32_t>& lab) {
    std::vector<int32_t> parent(mask.size());

    for (size_t i = 0; i < mask.size(); i++) {
        parent[i] = static_cast<int32_t>(i);
    }

    for (int y = 0; y < w; y++) {
        for (int x = 0; x < w; x++) {
            int32_t p = y * w + x;

            if (!mask[p]) {
                continue;
            }

            if (x > 0 && mask[p - 1]) {
                parent[Find(parent, p)] = Find(parent, p - 1);
            }

            if (y > 0 && mask[p - w]) {
                parent[Find(parent, p)] = Find(parent, p - w);
            }
        }
    }

    lab.assign(mask.size(), 0);
    std::vector<int32_t> id(mask.size(), 0);
    int count = 0;

    for (size_t i = 0; i < mask.size(); i++) {
        if (mask[i]) {
            int32_t root = Find(parent, static_cast<int32_t>(i));

            if (!id[root]) {
                id[root] = ++count;
            }

            lab[i] = id[root];
        }
    }

    return count;
}

// 1D lower envelope of parabolas (Felzenszwalb and Huttenlocher), squared distances in place
void Edt1(double* f, int n, int stride, std::vector<double>& d, std::vector<int>& v, std::vector<double>& z) {
    int k = 0;
    v[0] = 0;
    z[0] = -1e30;
    z[1] = 1e30;

    for (int q = 1; q < n; q++) {
        double s = ((f[q * stride] + q * q) - (f[v[k] * stride] + v[k] * v[k])) / (2.0 * q - 2.0 * v[k]);

        while (s <= z[k]) {
            k--;
            s = ((f[q * stride] + q * q) - (f[v[k] * stride] + v[k] * v[k])) / (2.0 * q - 2.0 * v[k]);
        }

        k++;
        v[k] = q;
        z[k] = s;
        z[k + 1] = 1e30;
    }

    k = 0;

    for (int q = 0; q < n; q++) {
        while (z[k + 1] < q) {
            k++;
        }

        d[q] = (q - v[k]) * (q - v[k]) + f[v[k] * stride];
    }

    for (int q = 0; q < n; q++) {
        f[q * stride] = d[q];
    }
}

// wrap(distance_transform_edt, mask): exact Euclidean distance of each set pixel to the nearest unset one, on the
// 3 x 3 tiling, centre cut out. The tiling repeats every n columns, so the column pass runs on n of them and the
// row pass on the n centre rows only (a ninth of the full transform, same numbers)
Img Edt(const std::vector<uint8_t>& mask, int n) {
    int w = 3 * n;
    // ponytail: a mask with no unset pixel at all comes out 2w ("far"), scipy's own answer differs; never seen in a texture
    const double far = static_cast<double>(w) * w * 4.0;
    // cols[x * w + y]: column x (of n), tiled 3 high, transposed so each runs contiguous
    std::vector<double> cols(static_cast<size_t>(n) * w);

    for (int x = 0; x < n; x++) {
        for (int y = 0; y < w; y++) {
            cols[x * w + y] = mask[(y % n) * n + x] ? far : 0.0;
        }
    }

    std::vector<double> d(w);
    std::vector<int> v(w);
    std::vector<double> z(w + 1);

    for (int x = 0; x < n; x++) {
        Edt1(&cols[x * w], w, 1, d, v, z);
    }

    Img out(static_cast<size_t>(n) * n);
    std::vector<double> row(w);

    for (int y = 0; y < n; y++) {
        for (int x = 0; x < w; x++) {
            row[x] = cols[(x % n) * w + y + n];
        }

        Edt1(row.data(), w, 1, d, v, z);

        for (int x = 0; x < n; x++) {
            out[y * n + x] = std::sqrt(std::min(row[x + n], far));
        }
    }

    return out;
}

// labels of the tiled mask, with the max of the (tiled) value over each
int LabelMax(const std::vector<uint8_t>& mask, int n, const Img& value, std::vector<int32_t>& lab, std::vector<double>& max, std::vector<int64_t>* area) {
    int w = 3 * n;
    std::vector<uint8_t> tiled(static_cast<size_t>(w) * w);

    for (int y = 0; y < w; y++) {
        for (int x = 0; x < w; x++) {
            tiled[y * w + x] = mask[(y % n) * n + (x % n)];
        }
    }

    int count = Label(tiled, w, lab);
    max.assign(count + 1, 0.0);

    if (area) {
        area->assign(count + 1, 0);
    }

    for (int y = 0; y < w; y++) {
        for (int x = 0; x < w; x++) {
            int32_t l = lab[y * w + x];

            if (area) {
                (*area)[l]++;
            }

            if (l) {
                max[l] = std::max(max[l], value[(y % n) * n + (x % n)]);
            }
        }
    }

    return count;
}

// Image.BOX resize to side `to`, for an integer factor
Img Box(const Img& x, int from, int to) {
    int f = from / to;
    Img out(static_cast<size_t>(to) * to, 0.0);

    for (int y = 0; y < from; y++) {
        for (int i = 0; i < from; i++) {
            out[(y / f) * to + i / f] += x[y * from + i];
        }
    }

    for (double& v : out) {
        v /= f * f;
    }

    return out;
}

} // namespace

Kind Generate(const uint8_t* bgra, uint32_t side, const char* name, std::vector<float>& out, uint32_t& outSide) {
    if (!bgra || side < 4 || (side & (side - 1))) {
        return Kind_Failed;
    }

    int n = static_cast<int>(std::min<uint32_t>(256, side));
    int f = static_cast<int>(side) / n;
    double k = n / 128.0;
    Img lum(static_cast<size_t>(n) * n);

    // the RGB box-resized to n (rounded to bytes like PIL's), then Rec. 709 luminance
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            uint32_t sum[3] = { 0, 0, 0 };

            for (int sy = 0; sy < f; sy++) {
                const uint8_t* row = bgra + (static_cast<size_t>(y * f + sy) * side + x * f) * 4;

                for (int sx = 0; sx < f; sx++) {
                    sum[0] += row[sx * 4 + 2];
                    sum[1] += row[sx * 4 + 1];
                    sum[2] += row[sx * 4 + 0];
                }
            }

            double rgb[3];

            for (int c = 0; c < 3; c++) {
                rgb[c] = ((sum[c] + f * f / 2) / (f * f)) / 255.0;
            }

            lum[y * n + x] = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
        }
    }

    Img hp = Blur(lum, n, n / 8.0);

    for (size_t i = 0; i < hp.size(); i++) {
        hp[i] = lum[i] - hp[i];
    }

    Img det = Stretch(Blur(hp, n, 1.3 * k));
    Img hpBlur = Blur(hp, n, 0.8 * k);
    Img dark1, bright1, dark2, bright2;
    Valley(lum, n, 1.2 * k, dark1, bright1);
    Valley(lum, n, 2.5 * k, dark2, bright2);

    // index 0 = dark joints (pol 1), 1 = bright (pol -1)
    Img score[2] = { GapScore(hpBlur, dark1, dark2, n, 1), GapScore(hpBlur, bright1, bright2, n, -1) };
    int q[2] = { Percolation(score[0], n), Percolation(score[1], n) };
    int p = q[0] <= q[1] ? 0 : 1;
    int cls = ClassOf(name);
    bool web = q[p] <= 34 && q[1 - p] - q[p] >= 12;
    // a bright web is mortar only in masonry (elsewhere it is raised roots or snow), and leaves are no stones
    bool stones = web && (p == 1 ? cls == 0 : cls != 1);

    Img h;
    Range range;
    Kind kind;

    if (stones) {
        int w = 3 * n;
        double cut = Percentile(score[p], 100.0 - FRAC * q[p]);
        std::vector<uint8_t> gap(score[p].size());

        for (size_t i = 0; i < gap.size(); i++) {
            gap[i] = score[p][i] > cut;
        }

        std::vector<int32_t> lab;
        std::vector<double> wide;
        std::vector<int64_t> area;
        int count = LabelMax(gap, n, Edt(gap, n), lab, wide, &area);
        std::vector<uint8_t> keep(count + 1, 0);

        // the web, or a thin piece of it
        for (int i = 1; i <= count; i++) {
            keep[i] = area[i] > 0.002 * n * n * 9 || (area[i] > 20.0 * k * k && area[i] > THIN * wide[i] * wide[i]);
        }

        // the web only: a dark speck inside a stone is no joint
        std::vector<uint8_t> stone(gap.size());

        for (int y = 0; y < n; y++) {
            for (int x = 0; x < n; x++) {
                stone[y * n + x] = !keep[lab[(y + n) * w + x + n]];
            }
        }

        Img d = Edt(stone, n);
        std::vector<double> dmax;
        LabelMax(stone, n, d, lab, dmax, nullptr);
        Img dome(d.size());

        // each stone's own size, up to 8 px at 128
        for (int y = 0; y < n; y++) {
            for (int x = 0; x < n; x++) {
                double r = std::min(std::max(dmax[lab[(y + n) * w + x + n]], 1.0), 8.0 * k);
                double t = 1.0 - std::min(d[y * n + x] / r, 1.0);
                dome[y * n + x] = 1.0 - t * t;
            }
        }

        dome = Blur(dome, n, 1.0 * k);
        Img detail = Stretch(Blur(hp, n, 2.0 * k));
        h.resize(dome.size());

        for (size_t i = 0; i < h.size(); i++) {
            h[i] = 0.85 * dome[i] + 0.15 * detail[i];
        }

        range = { 0.0, 1.0 };
        kind = p == 0 ? Kind_Stones : Kind_StonesBright;
    } else {
        h = det;
        range = cls < 0 ? s_defaultSoft : s_classes[cls].soft;
        kind = Kind_Soft;
    }

    int size = std::min(SIZE, n);

    if (size != n) {
        h = Box(h, n, size);
    }

    h = Stretch(h, 1.0, 99.0);
    out.resize(h.size());

    for (size_t i = 0; i < h.size(); i++) {
        out[i] = static_cast<float>(range.lo + (range.hi - range.lo) * h[i]);
    }

    outSide = static_cast<uint32_t>(size);
    return kind;
}

} // namespace TerrainHeightGen
