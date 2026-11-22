#include "modules/adjust/mainmenu/voronoi.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>

namespace slinky::modules::adjust_main_menu {

namespace {

constexpr uint64_t kMultiplier = 0x5DEECE66DULL;
constexpr uint64_t kAddend = 0xBULL;
constexpr uint64_t kMask = (1ULL << 48) - 1ULL;

struct LabelledPoint {
    Point p;
    int label = -1; // site whose bisector forms the edge starting at this vertex (-1 = envelope)
};

// Clips the convex polygon to {p : a.p <= b}; edges created on the clip line get `label`.
void clip(std::vector<LabelledPoint>& poly, std::vector<LabelledPoint>& scratch, double ax, double ay, double b,
          int label) {
    scratch.clear();
    const size_t n = poly.size();
    for (size_t k = 0; k < n; ++k) {
        const LabelledPoint& cur = poly[k];
        const LabelledPoint& nxt = poly[(k + 1) % n];
        const double dc = ax * cur.p.x + ay * cur.p.y - b;
        const double dn = ax * nxt.p.x + ay * nxt.p.y - b;
        const auto intersection = [&]() noexcept {
            const double t = dc / (dc - dn);
            return Point{cur.p.x + (nxt.p.x - cur.p.x) * t, cur.p.y + (nxt.p.y - cur.p.y) * t};
        };
        if (dc <= 0.0) {
            scratch.push_back(cur);
            if (dn > 0.0) {
                scratch.push_back(LabelledPoint{intersection(), label});
            }
        } else if (dn <= 0.0) {
            scratch.push_back(LabelledPoint{intersection(), cur.label});
        }
    }
    // Drop zero-length edges (a vertex equal to its successor keeps the successor's edge label).
    poly.clear();
    const size_t m = scratch.size();
    for (size_t k = 0; k < m; ++k) {
        const LabelledPoint& cur = scratch[k];
        const LabelledPoint& nxt = scratch[(k + 1) % m];
        if (m > 1 && std::fabs(cur.p.x - nxt.p.x) < 1e-9 && std::fabs(cur.p.y - nxt.p.y) < 1e-9) {
            continue;
        }
        poly.push_back(cur);
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// java.util.Random
// ---------------------------------------------------------------------------------------------------------------

JavaRandom::JavaRandom(int64_t seed) noexcept {
    set_seed(seed);
}

void JavaRandom::set_seed(int64_t seed) noexcept {
    seed_ = (static_cast<uint64_t>(seed) ^ kMultiplier) & kMask;
}

int32_t JavaRandom::next(int bits) noexcept {
    seed_ = (seed_ * kMultiplier + kAddend) & kMask;
    return static_cast<int32_t>(static_cast<uint32_t>(seed_ >> (48 - bits)));
}

int32_t JavaRandom::next_int() noexcept {
    return next(32);
}

int32_t JavaRandom::next_int(int32_t bound) noexcept {
    if (bound <= 0) {
        return 0;
    }
    int32_t r = next(31);
    const int32_t m = bound - 1;
    if ((bound & m) == 0) {
        return static_cast<int32_t>((static_cast<int64_t>(bound) * static_cast<int64_t>(r)) >> 31);
    }
    for (int32_t u = r;; u = next(31)) {
        r = u % bound;
        // Java: u - r + m < 0 (int overflow) -> reject
        if (static_cast<int64_t>(u) - r + m <= static_cast<int64_t>(INT32_MAX)) {
            break;
        }
    }
    return r;
}

float JavaRandom::next_float() noexcept {
    return static_cast<float>(next(24)) / static_cast<float>(1 << 24);
}

double JavaRandom::next_double() noexcept {
    const int64_t high = static_cast<int64_t>(next(26)) << 27;
    const int64_t low = next(27);
    return static_cast<double>(high + low) * 0x1.0p-53;
}

// ---------------------------------------------------------------------------------------------------------------
// Voronoi
// ---------------------------------------------------------------------------------------------------------------

std::vector<VoronoiPolygon> build_voronoi(const std::vector<Point>& sites, double min_x, double min_y,
                                          double max_x, double max_y) {
    std::vector<VoronoiPolygon> cells(sites.size());
    std::vector<LabelledPoint> poly;
    std::vector<LabelledPoint> scratch;
    poly.reserve(32);
    scratch.reserve(32);

    for (size_t i = 0; i < sites.size(); ++i) {
        const Point& si = sites[i];
        poly.clear();
        poly.push_back(LabelledPoint{{min_x, min_y}, -1});
        poly.push_back(LabelledPoint{{max_x, min_y}, -1});
        poly.push_back(LabelledPoint{{max_x, max_y}, -1});
        poly.push_back(LabelledPoint{{min_x, max_y}, -1});

        for (size_t j = 0; j < sites.size() && poly.size() >= 3; ++j) {
            if (j == i) {
                continue;
            }
            const Point& sj = sites[j];
            const double ax = sj.x - si.x;
            const double ay = sj.y - si.y;
            if (ax == 0.0 && ay == 0.0) {
                continue; // duplicate site
            }
            const double b = ((sj.x * sj.x + sj.y * sj.y) - (si.x * si.x + si.y * si.y)) * 0.5;
            clip(poly, scratch, ax, ay, b, static_cast<int>(j));
        }

        VoronoiPolygon& cell = cells[i];
        cell.points.reserve(poly.size());
        for (const LabelledPoint& lp : poly) {
            cell.points.push_back(lp.p);
            if (lp.label >= 0 &&
                std::find(cell.neighbours.begin(), cell.neighbours.end(), lp.label) == cell.neighbours.end()) {
                cell.neighbours.push_back(lp.label);
            }
        }
    }
    return cells;
}

bool polygon_contains(const std::vector<Point>& points, int x, int y) noexcept {
    // java.awt.Polygon.contains: even-odd rule on the integer vertices.
    const size_t n = points.size();
    if (n < 3) {
        return false;
    }
    bool inside = false;
    const double px = x;
    const double py = y;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double xi = static_cast<int>(points[i].x);
        const double yi = static_cast<int>(points[i].y);
        const double xj = static_cast<int>(points[j].x);
        const double yj = static_cast<int>(points[j].y);
        if ((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi) {
            inside = !inside;
        }
    }
    return inside;
}

int32_t adjust_string_hash(const char* utf8) noexcept {
    if (utf8 == nullptr) {
        return 0;
    }
    uint32_t hash = 64206u; // int arithmetic with wrap-around, like Java
    const auto feed_unit = [&](uint32_t unit) noexcept {
        const auto hi = static_cast<int8_t>((unit >> 8) & 0xFFu);
        const auto lo = static_cast<int8_t>(unit & 0xFFu);
        hash = 33u * hash + static_cast<uint32_t>(static_cast<int32_t>(hi));
        hash = 33u * hash + static_cast<uint32_t>(static_cast<int32_t>(lo));
    };
    const auto* s = reinterpret_cast<const unsigned char*>(utf8);
    while (*s != 0) {
        uint32_t cp = *s++;
        int extra = 0;
        if (cp >= 0xF0u) {
            cp &= 0x07u;
            extra = 3;
        } else if (cp >= 0xE0u) {
            cp &= 0x0Fu;
            extra = 2;
        } else if (cp >= 0xC0u) {
            cp &= 0x1Fu;
            extra = 1;
        }
        for (int k = 0; k < extra && (*s & 0xC0u) == 0x80u; ++k) {
            cp = (cp << 6) | (*s++ & 0x3Fu);
        }
        if (cp >= 0x10000u) {
            cp -= 0x10000u;
            feed_unit(0xD800u + (cp >> 10));
            feed_unit(0xDC00u + (cp & 0x3FFu));
        } else {
            feed_unit(cp);
        }
    }
    return static_cast<int32_t>(hash);
}

} // namespace slinky::modules::adjust_main_menu
