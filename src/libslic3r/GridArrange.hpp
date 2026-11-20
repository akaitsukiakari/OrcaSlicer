#pragma once

#include "Arrange.hpp"
#include "Point.hpp"

namespace Slic3r { namespace arrangement {

// The grid arranger, an alternative to the libnest2d one behind arrangement::arrange().
//
// Each item's shape (its footprint when given, else its convex hull) and every bed are turned
// into rasters of square cells, conservatively: a cell is taken when the shape touches it. Items
// go on the first bed with room. On a bed they gather into one cluster: each takes the spot next
// to the others that grows the box around the cluster least, then slides in as far as it can, so a concave footprint leaves its gaps to other items, and with
// place_in_holes its holes too. With group_similar, items of about the same footprint and the
// same materials are placed as one block of rows and columns. The cluster grows from a corner
// of the bed and is then centered on the alignment point when it can move there as a group.
// The price of the cells is up to about a cell of extra gap on each side of an item.
//
// It keeps the classic arranger's material rules (no high and low temperature filaments on one
// bed, and allow_multi_materials_on_same_plate), conditional bed exclusions and the excluded
// and non-preferred regions. It does not do sequential printing or belt printers, nor an
// unbounded bed: see grid_arrange_supports().
bool grid_arrange_supports(const ArrangeParams &params, const Points &bed);

void grid_arrange(ArrangePolygons &items, const ArrangePolygons &fixed, const Points &bed, const ArrangeParams &params);

}} // namespace Slic3r::arrangement
