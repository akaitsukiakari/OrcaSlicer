#ifndef ARRANGEJOB_HPP
#define ARRANGEJOB_HPP


#include <map>
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include <exception>
#include <functional>
#include <string>
#include <optional>
#include <utility>
#include <vector>
#include <cstddef>

#include "Job.hpp"
#include "libslic3r/Arrange.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/IMEXArrange.hpp"

namespace Slic3r {

class ModelInstance;
class ModelObject;

namespace GUI {

class Plater;
class PartPlate;

void set_arrange_polygon_bed_exclusion_extruders(arrangement::ArrangePolygon &polygon,
                                                  const ModelObject &object,
                                                  const PartPlate &plate,
                                                  const DynamicPrintConfig &config);

class ArrangeJob : public Job
{
    using ArrangePolygon = arrangement::ArrangePolygon;
    using ArrangePolygons = arrangement::ArrangePolygons;

    //BBS: add locked logic
    ArrangePolygons m_selected, m_unselected, m_unprintable, m_locked;
    std::vector<ModelInstance*> m_unarranged;
    std::map<int, ArrangePolygons> m_selected_groups;   // groups of selected items for sequential printing
    std::vector<int> m_uncompatible_plates;  // plate indices with different printing sequence than global

    // IMEX zone snapshot, taken on the main thread in prepare().
    //
    // process() runs on the worker thread (see Job::process). PartPlate::imex_primary_zone() is
    // non-const and calls ensure_imex_zones(), which reads wxGetApp().preset_bundle -- main-thread
    // GUI state -- on every call, and on a cache miss calls calc_imex_zones(), which destroys and
    // rebuilds std::vector<GLModel> members. ~GLModel issues glDeleteBuffers/glDeleteVertexArrays,
    // and no GL context is current on the worker, while the GUI thread may be painting those same
    // vectors from GLCanvas3D::on_paint. (imex_collision_zones() is itself const and merely reads
    // the member -- but it is only valid once imex_primary_zone() has warmed the cache, so it
    // cannot be moved off the main thread on its own.) Snapshotting plain geometry here keeps
    // every one of those touches on the main thread.
    //
    // The zones are stored already converted to plate-local coordinates, which is the space the
    // arranger works in.
    ImexArrangeInput m_imex;

    arrangement::ArrangeParams params;
    int current_plate_index = 0;
    Polygon bed_poly;
    Plater *m_plater;

    // BBS: add flag for whether on current part plate
    bool only_on_partplate{false};

    // Arrange settings read on the main thread in prepare(); see GLCanvas3D::ArrangeSettings.
    bool m_keep_on_plate{false};
    int  m_mode{0};
    // Per m_selected item, by itemid, when arranging all plates: its object's index and its
    // print time for ranking the items, from slicing or else estimated.
    std::vector<int>    m_selected_object;
    std::vector<double> m_selected_print_time;
    // Names of the items left where they were because they did not fit on the plate.
    std::vector<std::string> m_left_in_place;
    // Prime towers already on a plate: the plate and the tower's item in m_unselected.
    std::vector<std::pair<int, size_t>> m_wipe_towers;
    // Towers prepare() moved to a corner: the plate and the new position, applied in finalize().
    std::vector<std::pair<int, Vec2d>> m_moved_wipe_towers;

    // The arrange modes, run in process() on the items prepared for them.
    using Arranger = std::function<void(ArrangePolygons& items, const ArrangePolygons& fixed, const arrangement::ArrangeParams& params)>;
    void arrange_keeping_on_plate(const Arranger& arrange);
    void arrange_fewest_plates(const Arranger& arrange);
    void arrange_one_object_per_plate(const Arranger& arrange);

    // clear m_selected and m_unselected, reserve space for next usage
    void clear_input();

    // Prepare the selected and unselected items separately. If nothing is
    // selected, behaves as if everything would be selected.
    void prepare_selected();

    void prepare_all();

    //BBS:prepare the items from current selected partplate
    void prepare_partplate();
    void prepare_wipe_tower();
    void move_wipe_towers_to_corners();
    void prepare_belt_regions(int num_plates);
    void prepare_imex_zones();

    ArrangePolygon prepare_arrange_polygon(void *instance, const PartPlate *mapping_plate = nullptr);

protected:

    void check_unprintable();

public:

    void prepare();

    void process(Ctl &ctl) override;

    ArrangeJob();

    int status_range() const
    {
        // ensure finalize() is called after all operations in process() is finished.
        return int(m_selected.size() + m_unprintable.size() + 1);
    }

    void finalize(bool canceled, std::exception_ptr &e) override;
};

std::optional<arrangement::ArrangePolygon> get_wipe_tower_arrangepoly(const Plater &);

// Objects get packed around the prime tower, so a tower left mid-plate breaks up their rows.
// Moves `tower`, as the arranger sees it on the plate-local `bed`, to the corner of the bed
// nearest to it where it is clear of the items of `fixed` on its bed. Returns false and leaves
// it in place when no corner is clear.
bool move_wipe_tower_to_corner(arrangement::ArrangePolygon &tower, const arrangement::ArrangePolygons &fixed, const Points &bed);
// Saves the prime tower position of a plate, in the plate's coordinates.
void set_wipe_tower_position(int plate_idx, const Vec2d &pos);

// The gap between logical beds in the x axis expressed in ratio of
// the current bed width.
static const constexpr double LOGICAL_BED_GAP = 1. / 5.;

//BBS: add sudoku-style strides for x and y
// Stride between logical beds
double bed_stride_x(const Plater* plater);
double bed_stride_y(const Plater* plater);

arrangement::ArrangeParams init_arrange_params(Plater *p);

// Print times of object instances from the plates' last valid slices, for arranging by print time.
// They are kept when an arrangement moves the instances and dropped when an instance's shape changes.
// Main thread only.
void update_sliced_print_times(Plater &plater);
std::optional<double> sliced_print_time(const ModelInstance &instance);

}} // namespace Slic3r::GUI

#endif // ARRANGEJOB_HPP
