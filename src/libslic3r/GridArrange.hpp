#pragma once

#include "Arrange.hpp"
#include "Point.hpp"

namespace Slic3r { namespace arrangement {

// The grid arranger, an alternative to the libnest2d one behind arrangement::arrange().
//
// Each item's shape (its footprint when given, else its convex hull) and every bed are turned
// into rasters of square cells, conservatively: a cell is taken when the shape touches it. An
// item goes on the first bed with room, at the lowest and then leftmost free spot, trying each
// allowed rotation; then the items placed on each bed are centered on the alignment point when
// they can move there as a group. So the result is predictable and fills the beds in rows, and
// a concave footprint leaves its gaps to other items, and with place_in_holes its holes too.
// The price is up to about a cell of extra gap on each side of an item.
//
// It keeps the classic arranger's material rules (no high and low temperature filaments on one
// bed, and allow_multi_materials_on_same_plate), conditional bed exclusions and the excluded
// and non-preferred regions. It does not do sequential printing or belt printers, nor an
// unbounded bed: see grid_arrange_supports().
bool grid_arrange_supports(const ArrangeParams &params, const Points &bed);

void grid_arrange(ArrangePolygons &items, const ArrangePolygons &fixed, const Points &bed, const ArrangeParams &params);

}} // namespace Slic3r::arrangement
