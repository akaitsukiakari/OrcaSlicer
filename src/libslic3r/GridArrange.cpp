#include "GridArrange.hpp"

#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "ExPolygon.hpp"
#include "MinAreaBoundingBox.hpp"
#include "Polygon.hpp"
#include "Print.hpp"
#include "libslic3r.h"

#include <libnest2d/common.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace Slic3r { namespace arrangement {

namespace {

// Spans of taken cells, by row from the raster's origin cell, as inclusive column ranges
// from its origin column.
struct Raster
{
    int ox = 0, oy = 0; // origin cell where the shape was rasterized
    int w = 0, h = 0;
    std::vector<std::vector<std::pair<int, int>>> rows;

    bool empty() const { return rows.empty(); }
};

// The cells of a bed: cell (c, r) covers [anchor + c * size, anchor + (c + 1) * size) on each axis.
struct Grid
{
    Vec2d  anchor;
    double size;

    int col(double x) const { return int(std::floor((x - anchor.x()) / size)); }
    int row(double y) const { return int(std::floor((y - anchor.y()) / size)); }
    double row_y(int r) const { return anchor.y() + r * size; }
};

// The cells the shape touches. Every point of the shape inside a row's band is in the band's
// part of the shape, whose projection on X is that of its boundary: the parts of the edges in
// the band and the shape's runs along the band's two lines.
Raster rasterize(const ExPolygons &shape, const Grid &grid)
{
    Raster out;
    const BoundingBox bb = get_extents(shape);
    if (!bb.defined || shape.empty())
        return out;

    const int r0 = grid.row(double(bb.min.y())), r1 = grid.row(double(bb.max.y()));
    std::vector<std::vector<std::pair<double, double>>> ranges(r1 - r0 + 1);
    // Crossings of the row lines r0 + 1 .. r1, by line.
    std::vector<std::vector<double>> crossings(r1 - r0);

    const auto add_edge = [&](const Point &a, const Point &b) {
        const double ax = double(a.x()), ay = double(a.y()), bx = double(b.x()), by = double(b.y());
        const double ylo = std::min(ay, by), yhi = std::max(ay, by);
        for (int r = std::max(r0, grid.row(ylo)); r <= std::min(r1, grid.row(yhi)); ++r) {
            const double y0 = std::max(ylo, grid.row_y(r)), y1 = std::min(yhi, grid.row_y(r + 1));
            const auto   x_at = [&](double y) { return ay == by ? ax : ax + (bx - ax) * (y - ay) / (by - ay); };
            double       xa = x_at(y0), xb = x_at(y1);
            if (ay == by) {
                xa = std::min(ax, bx);
                xb = std::max(ax, bx);
            }
            ranges[r - r0].emplace_back(std::min(xa, xb), std::max(xa, xb));
        }
        // Half open, so a vertex on a line is counted once.
        for (int l = r0 + 1; l <= r1; ++l) {
            const double y = grid.row_y(l);
            if ((ay <= y && y < by) || (by <= y && y < ay))
                crossings[l - r0 - 1].push_back(ax + (bx - ax) * (y - ay) / (by - ay));
        }
    };
    for (const ExPolygon &expoly : shape) {
        const auto add_polygon = [&](const Polygon &poly) {
            for (size_t i = 0; i < poly.points.size(); ++i)
                add_edge(poly.points[i], poly.points[(i + 1) % poly.points.size()]);
        };
        add_polygon(expoly.contour);
        for (const Polygon &hole : expoly.holes)
            add_polygon(hole);
    }
    for (size_t l = 0; l < crossings.size(); ++l) {
        std::vector<double> &xs = crossings[l];
        std::sort(xs.begin(), xs.end());
        for (size_t i = 0; i + 1 < xs.size(); i += 2) {
            ranges[l].emplace_back(xs[i], xs[i + 1]);
            ranges[l + 1].emplace_back(xs[i], xs[i + 1]);
        }
    }

    out.oy = r0;
    out.h  = r1 - r0 + 1;
    out.rows.resize(out.h);
    int cmin = std::numeric_limits<int>::max(), cmax = std::numeric_limits<int>::min();
    for (int r = 0; r < out.h; ++r) {
        std::vector<std::pair<double, double>> &rr = ranges[r];
        std::sort(rr.begin(), rr.end());
        for (const auto &[x0, x1] : rr) {
            const int c0 = grid.col(x0), c1 = grid.col(x1);
            std::vector<std::pair<int, int>> &spans = out.rows[r];
            if (!spans.empty() && c0 <= spans.back().second + 1)
                spans.back().second = std::max(spans.back().second, c1);
            else
                spans.emplace_back(c0, c1);
            cmin = std::min(cmin, c0);
            cmax = std::max(cmax, c1);
        }
    }
    if (cmin > cmax)
        return {};
    out.ox = cmin;
    out.w  = cmax - cmin + 1;
    for (auto &spans : out.rows)
        for (auto &[c0, c1] : spans) {
            c0 -= cmin;
            c1 -= cmin;
        }
    return out;
}

// Which cells of a bed are taken, with each cell's run of free cells to its right, so a
// raster's fit at a spot costs one lookup per span.
class BedCells
{
public:
    BedCells(int w, int h) : m_w(w), m_h(h), m_taken(size_t(w) * h, 0), m_run(size_t(w) * h, 0)
    {
        for (int r = 0; r < h; ++r)
            update_row(r);
    }

    int width() const { return m_w; }
    int height() const { return m_h; }

    // Takes the cells of `raster` with its origin at (cx, cy); cells off the bed are ignored.
    void take(const Raster &raster, int cx, int cy)
    {
        for (int ry = 0; ry < raster.h; ++ry) {
            const int r = cy + ry;
            if (r < 0 || r >= m_h || raster.rows[ry].empty())
                continue;
            for (const auto &[a, b] : raster.rows[ry])
                for (int c = std::max(0, cx + a); c <= std::min(m_w - 1, cx + b); ++c)
                    m_taken[size_t(r) * m_w + c] = 1;
            update_row(r);
        }
    }

    // Whether `raster` fits with its origin at (cx, cy). If not, *next_cx is the first column
    // on that row where it may fit.
    bool fits(const Raster &raster, int cx, int cy, int *next_cx = nullptr) const
    {
        if (cx < 0 || cy < 0 || cx + raster.w > m_w || cy + raster.h > m_h) {
            if (next_cx)
                *next_cx = m_w;
            return false;
        }
        for (int ry = 0; ry < raster.h; ++ry)
            for (const auto &[a, b] : raster.rows[ry]) {
                const int run = m_run[size_t(cy + ry) * m_w + cx + a];
                if (run < b - a + 1) {
                    // The span covers the taken cell at cx + a + run until the origin passes it.
                    if (next_cx)
                        *next_cx = cx + run + 1;
                    return false;
                }
            }
        return true;
    }

    // The lowest, then leftmost, origin where `raster` fits.
    bool find(const Raster &raster, int &cx, int &cy, const std::function<bool(int, int)> &also_fits = nullptr) const
    {
        for (int y = 0; y + raster.h <= m_h; ++y)
            for (int x = 0; x + raster.w <= m_w;) {
                int next = x + 1;
                if (fits(raster, x, y, &next) && (!also_fits || also_fits(x, y))) {
                    cx = x;
                    cy = y;
                    return true;
                }
                x = std::max(next, x + 1);
            }
        return false;
    }

private:
    void update_row(int r)
    {
        int run = 0;
        for (int c = m_w - 1; c >= 0; --c) {
            const size_t i = size_t(r) * m_w + c;
            run      = m_taken[i] ? 0 : std::min(run + 1, int(UINT16_MAX));
            m_run[i] = uint16_t(run);
        }
    }

    int                   m_w, m_h;
    std::vector<uint8_t>  m_taken;
    std::vector<uint16_t> m_run;
};

bool rasters_overlap(const Raster &a, int ax, int ay, const Raster &b, int bx, int by)
{
    const int y0 = std::max(ay, by), y1 = std::min(ay + a.h, by + b.h);
    for (int y = y0; y < y1; ++y)
        for (const auto &[a0, a1] : a.rows[y - ay])
            for (const auto &[b0, b1] : b.rows[y - by])
                if (ax + a0 <= bx + b1 && bx + b0 <= ax + a1)
                    return true;
    return false;
}

int cell_count(const Raster &raster)
{
    int count = 0;
    for (const auto &spans : raster.rows)
        for (const auto &[a, b] : spans)
            count += b - a + 1;
    return count;
}

// Adds `raster`'s spans to `block` with its origin at (x, y) of the block; normalize() after.
void add_raster(Raster &block, const Raster &raster, int x, int y)
{
    for (int ry = 0; ry < raster.h; ++ry)
        for (const auto &[a, b] : raster.rows[ry])
            block.rows[y + ry].emplace_back(x + a, x + b);
}

// Sorts and merges each row's spans.
void normalize(Raster &raster)
{
    for (auto &spans : raster.rows) {
        std::sort(spans.begin(), spans.end());
        std::vector<std::pair<int, int>> merged;
        for (const auto &span : spans)
            if (!merged.empty() && span.first <= merged.back().second + 1)
                merged.back().second = std::max(merged.back().second, span.second);
            else
                merged.push_back(span);
        spans = std::move(merged);
    }
}

// The shape an item takes, inflated, at its rotation and with no translation.
ExPolygons item_shape(const ArrangePolygon &ap, double rotation, const ArrangeParams &params, coord_t extra_inflation = 0)
{
    // A footprint's holes are offered to other items only when asked for: a part placed
    // inside a frame is not always wanted.
    ExPolygons shape;
    if (ap.footprint.empty())
        shape.emplace_back(ap.poly.contour);
    else if (params.place_in_holes)
        shape = ap.footprint;
    else
        for (const ExPolygon &expoly : ap.footprint)
            shape.emplace_back(expoly.contour);
    for (ExPolygon &expoly : shape)
        expoly.rotate(rotation);
    const coord_t delta = ap.inflation + extra_inflation;
    return delta == 0 ? union_ex(shape) : offset_ex(shape, float(delta));
}

// A fixed item's shape where it is. Touching it is fine, as with the classic arranger.
ExPolygons fixed_shape(const ArrangePolygon &ap, const ArrangeParams &params)
{
    ExPolygons shape = item_shape(ap, ap.rotation, params, -scaled<coord_t>(2. * EPSILON));
    for (ExPolygon &expoly : shape)
        expoly.translate(Point(ap.translation));
    return shape;
}

// A fixed item, rasterized where it is.
struct Obstacle
{
    const ArrangePolygon *ap;
    Raster                raster;
};

struct Bed
{
    explicit Bed(int w, int h) : cells(w, h) {}

    BedCells              cells;
    std::vector<Raster>   fixed;       // the fixed items every item keeps out of, at their places
    std::vector<Obstacle> conditional; // bed exclusions that only some items keep out of
    // For the material rules, what the bed's real objects print with.
    std::set<int> temp_types;
    std::set<int> extruders;
    bool          has_objects = false;
    // The items arranged onto this bed, with their rasters and origins.
    struct Placed
    {
        size_t item;
        Raster raster;
        int    cx, cy;
    };
    std::vector<Placed> placed;
    // The boxes of the bed's objects, fixed and placed, which new items go next to, and the
    // box around them all.
    std::vector<std::tuple<int, int, int, int>> boxes;
    int cluster_x0 = 0, cluster_y0 = 0, cluster_x1 = 0, cluster_y1 = 0;

    void add_box(int x0, int y0, int x1, int y1)
    {
        boxes.emplace_back(x0, y0, x1, y1);
        if (cluster_x0 >= cluster_x1) {
            cluster_x0 = x0;
            cluster_y0 = y0;
            cluster_x1 = x1;
            cluster_y1 = y1;
        } else {
            cluster_x0 = std::min(cluster_x0, x0);
            cluster_y0 = std::min(cluster_y0, y0);
            cluster_x1 = std::max(cluster_x1, x1);
            cluster_y1 = std::max(cluster_y1, y1);
        }
    }
};

bool materials_fit(const Bed &bed, const ArrangePolygon &ap, const ArrangeParams &params)
{
    if (!bed.has_objects)
        return true;
    std::vector<int> types(bed.temp_types.begin(), bed.temp_types.end());
    types.push_back(ap.filament_temp_type);
    if (!Print::is_filaments_compatible(types))
        return false;
    if (!params.allow_multi_materials_on_same_plate) {
        const std::set<int> item(ap.extrude_ids.begin(), ap.extrude_ids.end());
        return std::includes(item.begin(), item.end(), bed.extruders.begin(), bed.extruders.end()) ||
               std::includes(bed.extruders.begin(), bed.extruders.end(), item.begin(), item.end());
    }
    return true;
}

void add_materials(Bed &bed, const ArrangePolygon &ap)
{
    if (ap.is_virt_object)
        return;
    bed.has_objects = true;
    bed.temp_types.insert(ap.filament_temp_type);
    bed.extruders.insert(ap.extrude_ids.begin(), ap.extrude_ids.end());
}

} // namespace

bool grid_arrange_supports(const ArrangeParams &params, const Points &bed)
{
    return !params.is_seq_print && !params.is_belt && bed.size() >= 3;
}

void grid_arrange(ArrangePolygons &items, const ArrangePolygons &fixed, const Points &bedpts, const ArrangeParams &params)
{
    const BoundingBox bedbb(bedpts);
    // About a thousand cells across the bed, at most 1 mm and at least 0.1 mm each.
    // Whole units, so moving a shape by whole cells moves its raster exactly.
    const double cell = std::round(std::clamp(double(std::max(bedbb.size().x(), bedbb.size().y())) / 1000., double(scaled(0.1)), double(scaled(1.))));
    const Grid   grid{bedbb.min.cast<double>(), cell};
    const int    cols = int(std::floor(bedbb.size().x() / cell)), rows = int(std::floor(bedbb.size().y() / cell));
    if (cols <= 0 || rows <= 0)
        return;

    // What a bed starts with: the bed outline's outside, and the regions no item may use.
    ExPolygons blocked = diff_ex(Polygons{bedbb.polygon()}, Polygons{Polygon(bedpts)});
    for (const ArrangePolygons *regions : {&params.excluded_regions, &params.nonprefered_regions})
        for (const ArrangePolygon &region : *regions)
            append(blocked, offset_ex(region.transformed_poly(), float(region.inflation)));
    const Raster blocked_raster = rasterize(union_ex(blocked), grid);

    std::vector<std::unique_ptr<Bed>> beds;
    const auto bed_at = [&](int idx) -> Bed & {
        while (int(beds.size()) <= idx) {
            beds.emplace_back(std::make_unique<Bed>(cols, rows));
            Bed &bed = *beds.back();
            bed.cells.take(blocked_raster, blocked_raster.ox, blocked_raster.oy);
            for (const ArrangePolygon &ap : fixed)
                if (ap.bed_idx == int(beds.size()) - 1) {
                    Raster raster = rasterize(fixed_shape(ap, params), grid);
                    if (raster.empty())
                        continue;
                    if (ap.is_bed_exclusion)
                        bed.conditional.push_back({&ap, std::move(raster)});
                    else {
                        bed.cells.take(raster, raster.ox, raster.oy);
                        // Real objects, as packed before by an arrange mode, are what new items gather by.
                        if (!ap.is_virt_object)
                            bed.add_box(raster.ox, raster.oy, raster.ox + raster.w, raster.oy + raster.h);
                        bed.fixed.push_back(std::move(raster));
                        add_materials(bed, ap);
                    }
                }
        }
        return *beds[idx];
    };

    // Items are placed in units: a single item, or with group_similar, a block of rows and
    // columns of items with about the same footprint and the same materials.
    struct ItemShapes
    {
        std::vector<double> rotations;
        std::vector<Raster> rasters;
    };
    std::vector<ItemShapes> shapes(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        const ArrangePolygon &ap = items[i];
        // The item's own rotation, or with rotations allowed, the four that square its
        // narrowest bounding box with the bed.
        ItemShapes &sh = shapes[i];
        sh.rotations   = {ap.rotation};
        if (params.allow_rotations && !ap.keep_rotation) {
            const double base = MinAreaBoundigBox(ap.poly.contour, MinAreaBoundigBox::pcConvex).angle_to_X();
            sh.rotations      = {base, base + PI / 2, base + PI, base + 3 * PI / 2};
        }
        for (double rotation : sh.rotations)
            sh.rasters.push_back(rasterize(item_shape(ap, rotation, params), grid));
    }

    // An item that matches a unit's first item: with its rotations reordered so they line up.
    const auto similar = [&](size_t a, size_t b) -> std::optional<size_t> {
        const ArrangePolygon &ia = items[a], &ib = items[b];
        if (ia.priority != ib.priority || ia.bed_temp != ib.bed_temp || ia.filament_temp_type != ib.filament_temp_type ||
            ia.extrude_ids != ib.extrude_ids || shapes[a].rotations.size() != shapes[b].rotations.size())
            return std::nullopt;
        const Raster &ra = shapes[a].rasters.front();
        if (ra.empty())
            return std::nullopt;
        const int count_a = cell_count(ra);
        for (size_t k = 0; k < shapes[b].rasters.size(); ++k) {
            const Raster &rb = shapes[b].rasters[k];
            if (!rb.empty() && std::abs(ra.w - rb.w) <= 1 && std::abs(ra.h - rb.h) <= 1 &&
                std::abs(count_a - cell_count(rb)) <= std::max(2, count_a * 3 / 100))
                return k;
        }
        return std::nullopt;
    };

    struct Unit
    {
        std::vector<size_t> items;
        double              area = 0.;
    };
    std::vector<Unit> units;
    {
        std::vector<bool> grouped(items.size(), false);
        for (size_t i = 0; i < items.size(); ++i) {
            if (grouped[i])
                continue;
            Unit unit;
            unit.items.push_back(i);
            if (params.group_similar)
                for (size_t j = i + 1; j < items.size(); ++j)
                    if (!grouped[j])
                        if (const std::optional<size_t> k = similar(i, j)) {
                            grouped[j] = true;
                            std::rotate(shapes[j].rotations.begin(), shapes[j].rotations.begin() + *k, shapes[j].rotations.end());
                            std::rotate(shapes[j].rasters.begin(), shapes[j].rasters.begin() + *k, shapes[j].rasters.end());
                            unit.items.push_back(j);
                        }
            for (size_t j : unit.items)
                unit.area += std::abs(items[j].poly.area());
            units.push_back(std::move(unit));
        }
    }

    // The packing order: priority first, as the arrange modes set it; then bed temperature, so
    // a bed's objects share it; then the largest units, which need room most.
    std::stable_sort(units.begin(), units.end(), [&](const Unit &ua, const Unit &ub) {
        const ArrangePolygon &ia = items[ua.items.front()], &ib = items[ub.items.front()];
        if (ia.priority != ib.priority)
            return ia.priority > ib.priority;
        if (ia.bed_temp != ib.bed_temp)
            return ia.bed_temp > ib.bed_temp;
        return ua.area > ub.area;
    });

    // The best spot on a bed for a raster: where the box around the bed's cluster of objects
    // grows the least, after sliding toward the bed's corner. The cluster
    // starts in the corner, which leaves the most room in one piece, and is moved to the
    // alignment point at the end.
    struct Spot
    {
        int       x = 0, y = 0;
        long long area = 0, dist = 0;
        bool operator<(const Spot &o) const { return std::tie(area, dist) < std::tie(o.area, o.dist); }
    };
    const auto best_spot = [&](const Bed &bed, const Raster &raster, const std::function<bool(int, int)> &clear) -> std::optional<Spot> {
        const auto ok = [&](int x, int y) { return bed.cells.fits(raster, x, y) && (!clear || clear(x, y)); };
        // A slide goes toward the corner.
        const int tx = 0, ty = 0;
        const auto slide = [&](int &x, int &y) {
            for (bool moved = true; moved;) {
                moved = false;
                if (x != tx && ok(x + (tx > x ? 1 : -1), y)) {
                    x += tx > x ? 1 : -1;
                    moved = true;
                }
                if (y != ty && ok(x, y + (ty > y ? 1 : -1))) {
                    y += ty > y ? 1 : -1;
                    moved = true;
                }
            }
        };
        const auto score = [&](int x, int y) {
            Spot s{x, y};
            int x0 = x, y0 = y, x1 = x + raster.w, y1 = y + raster.h;
            if (bed.cluster_x0 < bed.cluster_x1) {
                x0 = std::min(x0, bed.cluster_x0);
                y0 = std::min(y0, bed.cluster_y0);
                x1 = std::max(x1, bed.cluster_x1);
                y1 = std::max(y1, bed.cluster_y1);
            }
            s.area = (long long)(x1 - x0) * (y1 - y0);
            // Then reaching least high, then least far right, which packs a nearly full bed densest.
            s.dist = (long long)(y + raster.h) * (cols + 1) + x + raster.w;
            return s;
        };

        std::vector<std::pair<int, int>> candidates;
        if (bed.cluster_x0 < bed.cluster_x1) {
            for (const auto &[x0, y0, x1, y1] : bed.boxes) {
                const int w = raster.w, h = raster.h;
                for (const auto &c : {std::make_pair(x1, y0), std::make_pair(x1, y1 - h), std::make_pair(x0 - w, y0), std::make_pair(x0 - w, y1 - h),
                                      std::make_pair(x0, y1), std::make_pair(x1 - w, y1), std::make_pair(x0, y0 - h), std::make_pair(x1 - w, y0 - h)})
                    candidates.push_back(c);
            }
        } else
            candidates.emplace_back(tx, ty);

        // And the lowest, leftmost free spot, which finds the gaps away from the boxes' sides.
        {
            int x, y;
            if (!bed.cells.find(raster, x, y, clear))
                return std::nullopt;
            candidates.emplace_back(x, y);
        }

        std::optional<Spot> best;
        for (auto [x, y] : candidates) {
            if (!ok(x, y))
                continue;
            slide(x, y);
            const Spot s = score(x, y);
            if (!best || s < *best)
                best = s;
        }
        return best;
    };

    int packed = 0;
    for (const Unit &unit : units) {
        if (params.stopcondition && params.stopcondition())
            break;
        const ArrangePolygon &first = items[unit.items.front()];
        std::vector<size_t>   left  = unit.items;
        for (int bed_idx = 0; bed_idx < MAX_NUM_PLATES && !left.empty(); ++bed_idx) {
            Bed &bed = bed_at(bed_idx);
            if (!materials_fit(bed, first, params))
                continue;
            std::vector<const Obstacle *> conditional;
            for (const Obstacle &o : bed.conditional)
                if (bed_exclusion_applies(first, *o.ap))
                    conditional.push_back(&o);

            // As many of the unit's items as fit on this bed, in one block, for each rotation;
            // a bed may take more than one block.
            while (!left.empty()) {
                const size_t n = left.size();
                struct Layout
                {
                    size_t k, count;
                    int    cols, pw, ph;
                };
                std::vector<Layout> layouts;
                for (size_t k = 0; k < shapes[left.front()].rasters.size(); ++k) {
                    int pw = 0, ph = 0;
                    for (size_t i : left) {
                        pw = std::max(pw, shapes[i].rasters[k].w);
                        ph = std::max(ph, shapes[i].rasters[k].h);
                    }
                    if (pw == 0 || ph == 0 || pw > cols || ph > rows)
                        continue;
                    // About as many items across as up, by length.
                    const int c = std::clamp(int(std::lround(std::sqrt(double(n) * ph / pw))), 1, std::min(int(n), cols / pw));
                    for (int r = int((n + c - 1) / c); r >= 1; --r)
                        layouts.push_back({k, std::min(n, size_t(c) * r), c, pw, ph});
                    for (int c2 = std::min(c, int(n)) - 1; c2 >= 1; --c2)
                        layouts.push_back({k, size_t(c2), c2, pw, ph});
                }
                std::stable_sort(layouts.begin(), layouts.end(), [](const Layout &a, const Layout &b) { return a.count > b.count; });

                // Each item's raster and its origin in the block.
                struct Slot
                {
                    size_t item;
                    int    x, y;
                };
                std::optional<Spot> best;
                std::vector<Slot>   best_slots;
                Raster              best_block;
                size_t              best_k = 0, best_count = 0;
                for (const Layout &l : layouts) {
                    if (best && l.count < best_count)
                        break;
                    std::vector<Slot> slots;
                    Raster            block;
                    block.w = std::min(int(l.count), l.cols) * l.pw;
                    block.h = int((l.count + l.cols - 1) / l.cols) * l.ph;
                    block.rows.resize(block.h);
                    for (size_t j = 0; j < l.count; ++j) {
                        const Raster &r = shapes[left[j]].rasters[l.k];
                        const int     x = int(j % l.cols) * l.pw + (l.pw - r.w) / 2, y = int(j / l.cols) * l.ph + (l.ph - r.h) / 2;
                        slots.push_back({left[j], x, y});
                        add_raster(block, r, x, y);
                    }
                    normalize(block);
                    const auto clear = conditional.empty() ? std::function<bool(int, int)>() : [&](int cx, int cy) {
                        return std::none_of(conditional.begin(), conditional.end(), [&](const Obstacle *o) {
                            return rasters_overlap(block, cx, cy, o->raster, o->raster.ox, o->raster.oy);
                        });
                    };
                    if (const std::optional<Spot> s = best_spot(bed, block, clear); s && (!best || *s < *best)) {
                        best       = s;
                        best_slots = std::move(slots);
                        best_block = std::move(block);
                        best_k     = l.k;
                        best_count = l.count;
                    }
                }
                if (!best)
                    break;

                bed.cells.take(best_block, best->x, best->y);
                bed.add_box(best->x, best->y, best->x + best_block.w, best->y + best_block.h);
                for (const Slot &slot : best_slots) {
                    ArrangePolygon &ap     = items[slot.item];
                    const Raster   &raster = shapes[slot.item].rasters[best_k];
                    const int       cx = best->x + slot.x, cy = best->y + slot.y;
                    add_materials(bed, ap);
                    bed.placed.push_back({slot.item, raster, cx, cy});
                    ap.rotation    = shapes[slot.item].rotations[best_k];
                    ap.translation = Vec2crd(coord_t(std::llround((cx - raster.ox) * cell)), coord_t(std::llround((cy - raster.oy) * cell)));
                    ap.bed_idx     = bed_idx;
                    ap.itemid      = packed++;
                    if (params.progressind)
                        params.progressind(unsigned(packed), ap.name);
                    if (params.on_packed)
                        params.on_packed(ap);
                }
                left.erase(left.begin(), left.begin() + best_count);
            }
        }
        for (size_t i : left)
            items[i].bed_idx = UNARRANGED;
    }

    if (!params.do_final_align)
        return;

    // Move each bed's new items as a group toward the alignment point, as far as they can go.
    for (std::unique_ptr<Bed> &bed_ptr : beds) {
        Bed &bed = *bed_ptr;
        if (bed.placed.empty())
            continue;
        int x0 = cols, y0 = rows, x1 = 0, y1 = 0;
        for (const Bed::Placed &p : bed.placed) {
            x0 = std::min(x0, p.cx);
            y0 = std::min(y0, p.cy);
            x1 = std::max(x1, p.cx + p.raster.w);
            y1 = std::max(y1, p.cy + p.raster.h);
        }
        const int dx = int(std::lround(cols * params.align_center.x() - (x0 + x1) / 2.));
        const int dy = int(std::lround(rows * params.align_center.y() - (y0 + y1) / 2.));
        if (dx == 0 && dy == 0)
            continue;

        // The bed without the group, to try the group's moves on.
        BedCells without(cols, rows);
        without.take(blocked_raster, blocked_raster.ox, blocked_raster.oy);
        for (const Raster &raster : bed.fixed)
            without.take(raster, raster.ox, raster.oy);

        for (const double f : {1., 0.75, 0.5, 0.25}) {
            const int sx = int(std::lround(dx * f)), sy = int(std::lround(dy * f));
            const bool moves = std::all_of(bed.placed.begin(), bed.placed.end(), [&](const Bed::Placed &p) {
                if (!without.fits(p.raster, p.cx + sx, p.cy + sy))
                    return false;
                const ArrangePolygon &ap = items[p.item];
                return std::none_of(bed.conditional.begin(), bed.conditional.end(), [&](const Obstacle &o) {
                    return bed_exclusion_applies(ap, *o.ap) && rasters_overlap(p.raster, p.cx + sx, p.cy + sy, o.raster, o.raster.ox, o.raster.oy);
                });
            });
            if (moves) {
                for (const Bed::Placed &p : bed.placed)
                    items[p.item].translation += Vec2crd(coord_t(std::llround(sx * cell)), coord_t(std::llround(sy * cell)));
                break;
            }
        }
    }
}

}} // namespace Slic3r::arrangement
