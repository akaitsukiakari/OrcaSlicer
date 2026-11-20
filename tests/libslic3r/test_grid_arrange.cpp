#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/Arrange.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/GridArrange.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;
using namespace Slic3r::arrangement;
using Catch::Matchers::WithinAbs;

namespace {

Polygon rectangle(double w_mm, double h_mm)
{
    return Polygon({Point(0, 0), Point(scaled(w_mm), 0), Point(scaled(w_mm), scaled(h_mm)), Point(0, scaled(h_mm))});
}

ArrangePolygon item(const Polygon &outline)
{
    ArrangePolygon ap;
    ap.poly        = ExPolygon(outline);
    ap.extrude_ids = {1};
    return ap;
}

// A U open toward +X: w_mm wide and tall, with walls wall_mm thick. Its convex hull is the
// whole w_mm square.
ArrangePolygon u_shape(double w_mm, double wall_mm)
{
    const coord_t w = scaled(w_mm), t = scaled(wall_mm);
    const Polygon outline({Point(0, 0), Point(w, 0), Point(w, t), Point(t, t), Point(t, w - t), Point(w, w - t), Point(w, w), Point(0, w)});
    ArrangePolygon ap = item(rectangle(w_mm, w_mm));
    ap.footprint      = {ExPolygon(outline)};
    return ap;
}

Points bed(double w_mm, double h_mm) { return rectangle(w_mm, h_mm).points; }

ArrangeParams grid_params()
{
    ArrangeParams p;
    p.grid_engine = true;
    p.progressind = nullptr;
    return p;
}

ExPolygons placed(const ArrangePolygon &ap)
{
    ExPolygons shape = ap.footprint.empty() ? ExPolygons{ap.poly} : ap.footprint;
    for (ExPolygon &e : shape) {
        e.rotate(ap.rotation);
        e.translate(Point(ap.translation));
    }
    return shape;
}

double overlap(const ArrangePolygon &a, const ArrangePolygon &b) { return area(intersection_ex(placed(a), placed(b))); }

// Overlap below a thousandth of a square millimetre is clipping noise.
constexpr double no_area = 1e9;

} // namespace

TEST_CASE("The grid arranger keeps items on the bed and apart", "[GridArrange]")
{
    ArrangePolygons items;
    for (int i = 0; i < 12; ++i) {
        items.push_back(item(rectangle(20. + 7. * (i % 4), 15. + 9. * (i % 3))));
        items.back().inflation = scaled(1.);
    }
    arrange(items, {}, bed(200, 200), grid_params());

    const BoundingBox bedbb(bed(200, 200));
    for (size_t i = 0; i < items.size(); ++i) {
        REQUIRE(items[i].bed_idx == 0);
        CHECK(bedbb.contains(get_extents(placed(items[i]))));
        for (size_t j = i + 1; j < items.size(); ++j)
            CHECK_THAT(overlap(items[i], items[j]), WithinAbs(0., no_area));
    }
}

TEST_CASE("The grid arranger keeps the inflation between items", "[GridArrange]")
{
    ArrangePolygons items;
    for (int i = 0; i < 2; ++i) {
        items.push_back(item(rectangle(40, 40)));
        items.back().inflation = scaled(2.);
    }
    arrange(items, {}, bed(200, 200), grid_params());

    REQUIRE(items[0].bed_idx == 0);
    REQUIRE(items[1].bed_idx == 0);
    // 2 mm of inflation each, so at least 4 mm apart.
    const ExPolygons a = offset_ex(placed(items[0]), float(scaled(2.) - 10)), b = offset_ex(placed(items[1]), float(scaled(2.) - 10));
    CHECK_THAT(area(intersection_ex(a, b)), WithinAbs(0., no_area));
}

TEST_CASE("The grid arranger lays identical items out in rows", "[GridArrange]")
{
    // Nine 80 mm squares fill a 256 mm bed three by three.
    ArrangePolygons items;
    for (int i = 0; i < 9; ++i) {
        items.push_back(item(rectangle(80, 80)));
        items.back().inflation = scaled(1.);
    }
    arrange(items, {}, bed(256, 256), grid_params());

    std::set<coord_t> xs, ys;
    for (const ArrangePolygon &ap : items) {
        REQUIRE(ap.bed_idx == 0);
        xs.insert(ap.translation.x());
        ys.insert(ap.translation.y());
    }
    CHECK(xs.size() == 3);
    CHECK(ys.size() == 3);
}

TEST_CASE("The grid arranger fills the first bed before the next", "[GridArrange]")
{
    // Four 90 mm squares fit a 200 mm bed; the fifth goes to the next bed.
    ArrangePolygons items;
    for (int i = 0; i < 5; ++i)
        items.push_back(item(rectangle(90, 90)));
    arrange(items, {}, bed(200, 200), grid_params());

    CHECK(std::count_if(items.begin(), items.end(), [](const ArrangePolygon &ap) { return ap.bed_idx == 0; }) == 4);
    CHECK(std::count_if(items.begin(), items.end(), [](const ArrangePolygon &ap) { return ap.bed_idx == 1; }) == 1);
}

TEST_CASE("The grid arranger puts items in the concave part of a footprint", "[GridArrange]")
{
    // The U's hull fills nearly the whole bed, but the square fits in the U's opening.
    ArrangePolygons items{u_shape(120, 20), item(rectangle(60, 60))};
    arrange(items, {}, bed(125, 125), grid_params());

    REQUIRE(items[0].bed_idx == 0);
    CHECK(items[1].bed_idx == 0);
    CHECK_THAT(overlap(items[0], items[1]), WithinAbs(0., no_area));
}

TEST_CASE("The grid arranger uses the convex hull when there is no footprint", "[GridArrange]")
{
    ArrangePolygons items{u_shape(120, 20), item(rectangle(60, 60))};
    items[0].footprint.clear();
    arrange(items, {}, bed(125, 125), grid_params());

    CHECK(items[0].bed_idx != items[1].bed_idx);
}

TEST_CASE("The grid arranger places items in holes only when asked to", "[GridArrange]")
{
    // A 100 mm frame around a 70 mm hole, and a 40 mm square that fits only in the hole.
    ExPolygon frame(rectangle(100, 100));
    Polygon   hole = rectangle(70, 70);
    hole.translate(scaled(15.), scaled(15.));
    hole.reverse();
    frame.holes.push_back(hole);

    const auto arranged = [&](bool place_in_holes) {
        ArrangePolygons items{item(rectangle(100, 100)), item(rectangle(40, 40))};
        items[0].footprint = {frame};
        ArrangeParams params  = grid_params();
        params.place_in_holes = place_in_holes;
        arrange(items, {}, bed(105, 105), params);
        return items;
    };

    const ArrangePolygons inside = arranged(true);
    CHECK(inside[1].bed_idx == 0);
    CHECK_THAT(overlap(inside[0], inside[1]), WithinAbs(0., no_area));
    CHECK(arranged(false)[1].bed_idx == 1);
}

TEST_CASE("The grid arranger keeps items clear of fixed items", "[GridArrange]")
{
    ArrangePolygon tower = item(rectangle(60, 60));
    tower.translation    = Vec2crd(scaled(70.), scaled(70.));
    tower.bed_idx        = 0;
    tower.is_virt_object = true;
    tower.is_wipe_tower  = true;

    ArrangePolygons items;
    for (int i = 0; i < 6; ++i)
        items.push_back(item(rectangle(40, 40)));
    arrange(items, {tower}, bed(200, 200), grid_params());

    for (const ArrangePolygon &ap : items) {
        REQUIRE(ap.bed_idx == 0);
        CHECK_THAT(overlap(ap, tower), WithinAbs(0., no_area));
    }
}

TEST_CASE("The grid arranger keeps high and low temperature filaments on separate beds", "[GridArrange]")
{
    ArrangePolygons items;
    for (int i = 0; i < 4; ++i) {
        items.push_back(item(rectangle(30, 30)));
        items.back().filament_temp_type = i % 2 ? FilamentTempType::HighTemp : FilamentTempType::LowTemp;
    }
    arrange(items, {}, bed(200, 200), grid_params());

    for (const ArrangePolygon &a : items)
        for (const ArrangePolygon &b : items)
            if (a.filament_temp_type != b.filament_temp_type)
                CHECK(a.bed_idx != b.bed_idx);
}

TEST_CASE("The grid arranger leaves an item larger than the bed unarranged", "[GridArrange]")
{
    ArrangePolygons items{item(rectangle(250, 10))};
    arrange(items, {}, bed(200, 200), grid_params());

    CHECK(items[0].bed_idx == UNARRANGED);
}

TEST_CASE("The grid arranger centers a bed's items on the alignment point", "[GridArrange]")
{
    ArrangePolygons items{item(rectangle(40, 20))};
    arrange(items, {}, bed(200, 200), grid_params());

    REQUIRE(items[0].bed_idx == 0);
    const Vec2d center = unscaled(get_extents(placed(items[0])).center());
    // Within a cell or two of the bed's center.
    CHECK_THAT(center.x(), WithinAbs(100., 1.));
    CHECK_THAT(center.y(), WithinAbs(100., 1.));
}

TEST_CASE("The grid arranger leaves sequential printing and belt printers to the classic arranger", "[GridArrange]")
{
    ArrangeParams params = grid_params();
    CHECK(grid_arrange_supports(params, bed(200, 200)));
    params.is_seq_print = true;
    CHECK_FALSE(grid_arrange_supports(params, bed(200, 200)));
    params.is_seq_print = false;
    params.is_belt      = true;
    CHECK_FALSE(grid_arrange_supports(params, bed(200, 200)));
}

TEST_CASE("The grid arranger keeps an item's rotation when told to, even with rotations allowed", "[GridArrange]")
{
    // A long bar along Y on a bed that is wide but short: it fits only turned a quarter.
    const auto arranged = [](bool keep_rotation) {
        ArrangePolygons items{item(rectangle(10, 150))};
        items[0].keep_rotation = keep_rotation;
        ArrangeParams params   = grid_params();
        params.allow_rotations = true;
        arrange(items, {}, bed(200, 100), params);
        return items[0];
    };

    const ArrangePolygon turned = arranged(false);
    REQUIRE(turned.bed_idx == 0);
    CHECK_THAT(std::abs(std::sin(turned.rotation)), WithinAbs(1., 1e-6));

    const ArrangePolygon kept = arranged(true);
    CHECK(kept.bed_idx == UNARRANGED);
    CHECK_THAT(kept.rotation, WithinAbs(0., 1e-9));
}

TEST_CASE("The grid arranger groups similar items into one block of rows and columns", "[GridArrange]")
{
    // Twenty items of 25 x 12 mm, half of them a few tenths larger, and five larger ones.
    ArrangePolygons items;
    for (int i = 0; i < 20; ++i)
        items.push_back(item(rectangle(i % 2 ? 25.3 : 25., i % 2 ? 12.2 : 12.)));
    for (int i = 0; i < 5; ++i)
        items.push_back(item(rectangle(60, 40)));
    for (ArrangePolygon &ap : items)
        ap.inflation = scaled(1.);
    ArrangeParams params = grid_params();
    params.group_similar = true;
    arrange(items, {}, bed(270, 270), params);

    // The small items sit on a grid: few distinct columns and rows, all filled but the last row.
    // Centers within a millimetre of each other are in the same column or row.
    const auto distinct = [](std::vector<coord_t> v) {
        std::sort(v.begin(), v.end());
        size_t n = 0;
        for (size_t i = 0; i < v.size(); ++i)
            if (i == 0 || v[i] - v[i - 1] > scaled(1.))
                ++n;
        return n;
    };
    std::vector<coord_t> cx, cy;
    for (int i = 0; i < 20; ++i) {
        REQUIRE(items[i].bed_idx == 0);
        const BoundingBox bb = get_extents(placed(items[i]));
        cx.push_back(bb.center().x());
        cy.push_back(bb.center().y());
    }
    const size_t columns = distinct(cx), rows = distinct(cy);
    CHECK(columns * rows >= 20);
    CHECK(columns * rows < 20 + columns);
    for (size_t i = 0; i < items.size(); ++i)
        for (size_t j = i + 1; j < items.size(); ++j)
            CHECK_THAT(overlap(items[i], items[j]), WithinAbs(0., no_area));
}
