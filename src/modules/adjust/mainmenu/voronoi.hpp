#pragma once

// Geometry helpers for the Adjust main menu background (com.adjustclient.ui.mainmenu.VoronoiBackground):
//   * JavaRandom - bit-exact port of java.util.Random (Adjust seeds every frame with the same value, so the cell
//     shading must come from the same generator to look the same).
//   * build_voronoi - replacement for JTS VoronoiDiagramBuilder: one convex polygon per site (half-plane
//     clipping), clipped to the sites' envelope expanded by max(width, height) like JTS, plus the edge neighbours
//     of every cell (JTS Geometry.touches()).
//   * polygon_contains - java.awt.Polygon.contains(int, int) on integer-truncated vertices.

#include <cstdint>
#include <vector>

namespace slinky::modules::adjust_main_menu {

class JavaRandom {
public:
    explicit JavaRandom(int64_t seed) noexcept;

    void set_seed(int64_t seed) noexcept;
    int32_t next(int bits) noexcept;
    int32_t next_int() noexcept;
    int32_t next_int(int32_t bound) noexcept;
    float next_float() noexcept;
    double next_double() noexcept;

private:
    uint64_t seed_ = 0;
};

struct Point {
    double x = 0.0;
    double y = 0.0;
};

struct VoronoiPolygon {
    std::vector<Point> points;    // convex, counter-clockwise or clockwise (as clipped from the envelope)
    std::vector<int> neighbours;  // indices of the cells sharing an edge with this one
};

// One polygon per site, in site order.
std::vector<VoronoiPolygon> build_voronoi(const std::vector<Point>& sites, double min_x, double min_y,
                                          double max_x, double max_y);

bool polygon_contains(const std::vector<Point>& points, int x, int y) noexcept;

// com.adjustclient.util.HashUtil.hash(String) - 33 * h + b over the UTF-16BE bytes, starting at 64206.
int32_t adjust_string_hash(const char* utf8) noexcept;

} // namespace slinky::modules::adjust_main_menu
