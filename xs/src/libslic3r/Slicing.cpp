#include "Slicing.hpp"
#include "SlicingAdaptive.hpp"
#include "PrintConfig.hpp"
#include "Model.hpp"

// #define SLIC3R_DEBUG

// Make assert active if SLIC3R_DEBUG
#ifdef SLIC3R_DEBUG
    #undef NDEBUG
    #define DEBUG
    #define _DEBUG
    #include "SVG.hpp"
    #undef assert 
    #include <cassert>
#endif

namespace Slic3r
{

SlicingParameters SlicingParameters::create_from_config(
	const PrintConfig 		&print_config, 
	const PrintObjectConfig &object_config,
	coordf_t				 object_height,
	const std::set<size_t>  &object_extruders)
{
    coordf_t first_layer_height                      = (object_config.first_layer_height.value <= 0) ? 
        object_config.layer_height.value : 
        object_config.first_layer_height.get_abs_value(object_config.layer_height.value);
    coordf_t support_material_extruder_dmr           = print_config.nozzle_diameter.get_at(object_config.support_material_extruder.value - 1);
    coordf_t support_material_interface_extruder_dmr = print_config.nozzle_diameter.get_at(object_config.support_material_interface_extruder.value - 1);
    bool     soluble_interface                       = object_config.support_material_contact_distance.value == 0.;

    SlicingParameters params;
    params.layer_height = object_config.layer_height.value;
    params.first_object_layer_height = first_layer_height;
    params.object_print_z_min = 0.;
    params.object_print_z_max = object_height;
    params.base_raft_layers = object_config.raft_layers.value;

    if (params.base_raft_layers > 0) {
		params.interface_raft_layers = (params.base_raft_layers + 1) / 2;
        params.base_raft_layers -= params.interface_raft_layers;
        // Use as large as possible layer height for the intermediate raft layers.
        params.base_raft_layer_height       = std::max(params.layer_height, 0.75 * support_material_extruder_dmr);
        params.interface_raft_layer_height  = std::max(params.layer_height, 0.75 * support_material_interface_extruder_dmr);
        params.contact_raft_layer_height_bridging = false;
        params.first_object_layer_bridging  = false;
        #if 1
        params.contact_raft_layer_height    = std::max(params.layer_height, 0.75 * support_material_interface_extruder_dmr);
        if (! soluble_interface) {
            // Compute the average of all nozzles used for printing the object over a raft.
            //FIXME It is expected, that the 1st layer of the object is printed with a bridging flow over a full raft. Shall it not be vice versa?
            coordf_t average_object_extruder_dmr = 0.;
            if (! object_extruders.empty()) {
                for (std::set<size_t>::const_iterator it_extruder = object_extruders.begin(); it_extruder != object_extruders.end(); ++ it_extruder)
                    average_object_extruder_dmr += print_config.nozzle_diameter.get_at(*it_extruder);
                average_object_extruder_dmr /= coordf_t(object_extruders.size());
            }
            params.first_object_layer_height   = average_object_extruder_dmr;
            params.first_object_layer_bridging = true;
        }
        #else
        params.contact_raft_layer_height    = soluble_interface ? support_material_interface_extruder_dmr : 0.75 * support_material_interface_extruder_dmr;
        params.contact_raft_layer_height_bridging = ! soluble_interface;
        ...
        #endif
    }

    if (params.has_raft()) {
        // Raise first object layer Z by the thickness of the raft itself plus the extra distance required by the support material logic.
        //FIXME The last raft layer is the contact layer, which shall be printed with a bridging flow for ease of separation. Currently it is not the case.
		coordf_t print_z = first_layer_height + object_config.support_material_contact_distance.value;
		if (params.raft_layers() == 1) {
			params.contact_raft_layer_height = first_layer_height;
		} else {
			print_z +=
				// Number of the base raft layers is decreased by the first layer, which has already been added to print_z.
				coordf_t(params.base_raft_layers - 1) * params.base_raft_layer_height +
				// Number of the interface raft layers is decreased by the contact layer.
				coordf_t(params.interface_raft_layers - 1) * params.interface_raft_layer_height +
				params.contact_raft_layer_height;
		}
        params.object_print_z_min = print_z;
        params.object_print_z_max += print_z;
    }

    params.min_layer_height = std::min(params.layer_height, first_layer_height);
    params.max_layer_height = std::max(params.layer_height, first_layer_height);

    //FIXME add it to the print configuration
    params.min_layer_height = 0.05;

    // Calculate the maximum layer height as 0.75 from the minimum nozzle diameter.
    if (! object_extruders.empty()) {
        coordf_t min_object_extruder_dmr = 1000000.;
        for (std::set<size_t>::const_iterator it_extruder = object_extruders.begin(); it_extruder != object_extruders.end(); ++ it_extruder)
            min_object_extruder_dmr = std::min(min_object_extruder_dmr, print_config.nozzle_diameter.get_at(*it_extruder));
        // Allow excessive maximum layer height higher than 0.75 * min_object_extruder_dmr 
        params.max_layer_height = std::max(std::max(params.layer_height, first_layer_height), 0.75 * min_object_extruder_dmr);
    }

    return params;
}

// Convert layer_height_ranges to layer_height_profile. Both are referenced to z=0, meaning the raft layers are not accounted for
// in the height profile and the printed object may be lifted by the raft thickness at the time of the G-code generation.
std::vector<coordf_t> layer_height_profile_from_ranges(
	const SlicingParameters 	&slicing_params,
	const t_layer_height_ranges &layer_height_ranges)
{
    // 1) If there are any height ranges, trim one by the other to make them non-overlapping. Insert the 1st layer if fixed.
    std::vector<std::pair<t_layer_height_range,coordf_t>> ranges_non_overlapping;
    ranges_non_overlapping.reserve(layer_height_ranges.size() * 4);
    if (slicing_params.first_object_layer_height_fixed())
        ranges_non_overlapping.push_back(std::pair<t_layer_height_range,coordf_t>(
            t_layer_height_range(0., slicing_params.first_object_layer_height), 
            slicing_params.first_object_layer_height));
    // The height ranges are sorted lexicographically by low / high layer boundaries.
    for (t_layer_height_ranges::const_iterator it_range = layer_height_ranges.begin(); it_range != layer_height_ranges.end(); ++ it_range) {
        coordf_t lo = it_range->first.first;
        coordf_t hi = std::min(it_range->first.second, slicing_params.object_print_z_height());
        coordf_t height = it_range->second;
        if (! ranges_non_overlapping.empty())
            // Trim current low with the last high.
            lo = std::max(lo, ranges_non_overlapping.back().first.second);
        if (lo + EPSILON < hi)
            // Ignore too narrow ranges.
            ranges_non_overlapping.push_back(std::pair<t_layer_height_range,coordf_t>(t_layer_height_range(lo, hi), height));
    }

    // 2) Convert the trimmed ranges to a height profile, fill in the undefined intervals between z=0 and z=slicing_params.object_print_z_max()
    // with slicing_params.layer_height
    std::vector<coordf_t> layer_height_profile;
    for (std::vector<std::pair<t_layer_height_range,coordf_t>>::const_iterator it_range = ranges_non_overlapping.begin(); it_range != ranges_non_overlapping.end(); ++ it_range) {
        coordf_t lo = it_range->first.first;
        coordf_t hi = it_range->first.second;
        coordf_t height = it_range->second;
        coordf_t last_z      = layer_height_profile.empty() ? 0. : layer_height_profile[layer_height_profile.size() - 2];
        coordf_t last_height = layer_height_profile.empty() ? 0. : layer_height_profile[layer_height_profile.size() - 1];
        if (lo > last_z + EPSILON) {
            // Insert a step of normal layer height.
            layer_height_profile.push_back(last_z);
            layer_height_profile.push_back(slicing_params.layer_height);
            layer_height_profile.push_back(lo);
            layer_height_profile.push_back(slicing_params.layer_height);
        }
        // Insert a step of the overriden layer height.
        layer_height_profile.push_back(lo);
        layer_height_profile.push_back(height);
        layer_height_profile.push_back(hi);
        layer_height_profile.push_back(height);
    }

    coordf_t last_z      = layer_height_profile.empty() ? 0. : layer_height_profile[layer_height_profile.size() - 2];
    coordf_t last_height = layer_height_profile.empty() ? 0. : layer_height_profile[layer_height_profile.size() - 1];
    if (last_z < slicing_params.object_print_z_height()) {
        // Insert a step of normal layer height up to the object top.
        layer_height_profile.push_back(last_z);
        layer_height_profile.push_back(slicing_params.layer_height);
        layer_height_profile.push_back(slicing_params.object_print_z_height());
        layer_height_profile.push_back(slicing_params.layer_height);
    }

   	return layer_height_profile;
}

// Based on the work of @platsch
// Fill layer_height_profile by heights ensuring a prescribed maximum cusp height.
std::vector<coordf_t> layer_height_profile_adaptive(
    const SlicingParameters     &slicing_params,
    const t_layer_height_ranges &layer_height_ranges,
    const ModelVolumePtrs		&volumes)
{
    // 1) Initialize the SlicingAdaptive class with the object meshes.
    SlicingAdaptive as;
    as.set_slicing_parameters(slicing_params);
    for (ModelVolumePtrs::const_iterator it = volumes.begin(); it != volumes.end(); ++ it)
        if (! (*it)->modifier)
            as.add_mesh(&(*it)->mesh);
    as.prepare();

    // 2) Generate layers using the algorithm of @platsch 
    // loop until we have at least one layer and the max slice_z reaches the object height
    //FIXME make it configurable
    // Cusp value: A maximum allowed distance from a corner of a rectangular extrusion to a chrodal line, in mm.
    const coordf_t cusp_value = 0.2; // $self->config->get_value('cusp_value');

    std::vector<coordf_t> layer_height_profile;
    layer_height_profile.push_back(0.);
    layer_height_profile.push_back(slicing_params.first_object_layer_height);
    if (slicing_params.first_object_layer_height_fixed()) {
        layer_height_profile.push_back(slicing_params.first_object_layer_height);
        layer_height_profile.push_back(slicing_params.first_object_layer_height);
    }
    coordf_t slice_z = slicing_params.first_object_layer_height;
    coordf_t height  = slicing_params.first_object_layer_height;
    coordf_t cusp_height = 0.;
    int current_facet = 0;
    while ((slice_z - height) <= slicing_params.object_print_z_height()) {
        height = 999;
        // Slic3r::debugf "\n Slice layer: %d\n", $id;
        // determine next layer height
        coordf_t cusp_height = as.cusp_height(slice_z, cusp_value, current_facet);
        // check for horizontal features and object size
        /*
        if($self->config->get_value('match_horizontal_surfaces')) {
            my $horizontal_dist = $adaptive_slicing[$region_id]->horizontal_facet_distance(scale $slice_z+$cusp_height, $min_height);
            if(($horizontal_dist < $min_height) && ($horizontal_dist > 0)) {
                Slic3r::debugf "Horizontal feature ahead, distance: %f\n", $horizontal_dist;
                # can we shrink the current layer a bit?
                if($cusp_height-($min_height-$horizontal_dist) > $min_height) {
                    # yes we can
                    $cusp_height = $cusp_height-($min_height-$horizontal_dist);
                    Slic3r::debugf "Shrink layer height to %f\n", $cusp_height;
                }else{
                    # no, current layer would become too thin
                    $cusp_height = $cusp_height+$horizontal_dist;
                    Slic3r::debugf "Widen layer height to %f\n", $cusp_height;
                }
            }
        }
        */
        height = std::min(cusp_height, height);

        // apply z-gradation
        /*
        my $gradation = $self->config->get_value('adaptive_slicing_z_gradation');
        if($gradation > 0) {
            $height = $height - unscale((scale($height)) % (scale($gradation)));
        }
        */
    
        // look for an applicable custom range
        /*
        if (my $range = first { $_->[0] <= $slice_z && $_->[1] > $slice_z } @{$self->layer_height_ranges}) {
            $height = $range->[2];
    
            # if user set custom height to zero we should just skip the range and resume slicing over it
            if ($height == 0) {
                $slice_z += $range->[1] - $range->[0];
                next;
            }
        }
        */
        
        layer_height_profile.push_back(slice_z);
        layer_height_profile.push_back(height);
        slice_z += height;
        layer_height_profile.push_back(slice_z);
        layer_height_profile.push_back(height);
    }

    coordf_t last = std::max(slicing_params.first_object_layer_height, layer_height_profile[layer_height_profile.size() - 2]);
    layer_height_profile.push_back(last);
    layer_height_profile.push_back(slicing_params.first_object_layer_height);
    layer_height_profile.push_back(slicing_params.object_print_z_height());
    layer_height_profile.push_back(slicing_params.first_object_layer_height);

    return layer_height_profile;
}

template <typename T>
static inline T clamp(const T low, const T high, const T value)
{
    return std::max(low, std::min(high, value));
}

template <typename T>
static inline T lerp(const T a, const T b, const T t)
{
    assert(t >= T(-EPSILON) && t <= T(1.+EPSILON));
    return (1. - t) * a + t * b;
}

void adjust_layer_height_profile(
    const SlicingParameters     &slicing_params,
    std::vector<coordf_t> 		&layer_height_profile,
    coordf_t 					 z,
    coordf_t 					 layer_thickness_delta,
    coordf_t 					 band_width,
    int 						 action)
{
     // Constrain the profile variability by the 1st layer height.
    std::pair<coordf_t, coordf_t> z_span_variable = 
        std::pair<coordf_t, coordf_t>(
            slicing_params.first_object_layer_height_fixed() ? slicing_params.first_object_layer_height : 0.,
            slicing_params.object_print_z_height());
    if (z < z_span_variable.first || z > z_span_variable.second)
        return;

	assert(layer_height_profile.size() >= 2);

    // 1) Get the current layer thickness at z.
    coordf_t current_layer_height = slicing_params.layer_height;
    for (size_t i = 0; i < layer_height_profile.size(); i += 2) {
        if (i + 2 == layer_height_profile.size()) {
            current_layer_height = layer_height_profile[i + 1];
            break;
        } else if (layer_height_profile[i + 2] > z) {
            coordf_t z1 = layer_height_profile[i];
            coordf_t h1 = layer_height_profile[i + 1];
            coordf_t z2 = layer_height_profile[i + 2];
            coordf_t h2 = layer_height_profile[i + 3];
            current_layer_height = lerp(h1, h2, (z - z1) / (z2 - z1));
			break;
        }
    }

    // 2) Is it possible to apply the delta?
    switch (action) {
        case 0:
        default:
            if (layer_thickness_delta > 0) {
                if (current_layer_height >= slicing_params.max_layer_height - EPSILON)
                    return;
                layer_thickness_delta = std::min(layer_thickness_delta, slicing_params.max_layer_height - current_layer_height);
            } else {
                if (current_layer_height <= slicing_params.min_layer_height + EPSILON)
                    return;
                layer_thickness_delta = std::max(layer_thickness_delta, slicing_params.min_layer_height - current_layer_height);
            }
            break;
        case 1:
            layer_thickness_delta = std::abs(layer_thickness_delta);
            layer_thickness_delta = std::min(layer_thickness_delta, std::abs(slicing_params.layer_height - current_layer_height));
            if (layer_thickness_delta < EPSILON)
                return;
            break;
    }

    // 3) Densify the profile inside z +- band_width/2, remove duplicate Zs from the height profile inside the band.
	coordf_t lo = std::max(z_span_variable.first,  z - 0.5 * band_width);
	coordf_t hi = std::min(z_span_variable.second, z + 0.5 * band_width);
    coordf_t z_step = 0.1;
    size_t i = 0;
    while (i < layer_height_profile.size() && layer_height_profile[i] < lo)
        i += 2;
    i -= 2;

    std::vector<double> profile_new;
    profile_new.reserve(layer_height_profile.size());
	assert(i >= 0 && i + 1 < layer_height_profile.size());
	profile_new.insert(profile_new.end(), layer_height_profile.begin(), layer_height_profile.begin() + i + 2);
    coordf_t zz = lo;
    while (zz < hi) {
        size_t next = i + 2;
        coordf_t z1 = layer_height_profile[i];
        coordf_t h1 = layer_height_profile[i + 1];
        coordf_t height = h1;
        if (next < layer_height_profile.size()) {
            coordf_t z2 = layer_height_profile[next];
            coordf_t h2 = layer_height_profile[next + 1];
            height = lerp(h1, h2, (zz - z1) / (z2 - z1));
        }
        // Adjust height by layer_thickness_delta.
        coordf_t weight = std::abs(zz - z) < 0.5 * band_width ? (0.5 + 0.5 * cos(2. * M_PI * (zz - z) / band_width)) : 0.;
        coordf_t height_new = height;
        switch (action) {
            case 0:
            default:
                height += weight * layer_thickness_delta;
                break;
            case 1:
            {
                coordf_t delta = height - slicing_params.layer_height;
                coordf_t step  = weight * layer_thickness_delta;
                step = (std::abs(delta) > step) ?
                    (delta > 0) ? -step : step :
                    -delta;
                height += step;
                break;
            }
        }
        // Avoid entering a too short segment.
        if (profile_new[profile_new.size() - 2] + EPSILON < zz) {
            profile_new.push_back(zz);
            profile_new.push_back(clamp(slicing_params.min_layer_height, slicing_params.max_layer_height, height));
        }
        zz += z_step;
        i = next;
        while (i < layer_height_profile.size() && layer_height_profile[i] < zz)
            i += 2;
        i -= 2;
    }

    i += 2;
	if (i < layer_height_profile.size()) {
        if (profile_new[profile_new.size() - 2] + z_step < layer_height_profile[i]) {
            profile_new.push_back(profile_new[profile_new.size() - 2] + z_step);
            profile_new.push_back(layer_height_profile[i + 1]);
        }
		profile_new.insert(profile_new.end(), layer_height_profile.begin() + i, layer_height_profile.end());
    }
    layer_height_profile = std::move(profile_new);

	assert(layer_height_profile.size() > 2);
	assert(layer_height_profile.size() % 2 == 0);
	assert(layer_height_profile[0] == 0.);
#ifdef _DEBUG
	for (size_t i = 2; i < layer_height_profile.size(); i += 2)
		assert(layer_height_profile[i - 2] <= layer_height_profile[i]);
	for (size_t i = 1; i < layer_height_profile.size(); i += 2) {
		assert(layer_height_profile[i] > slicing_params.min_layer_height - EPSILON);
		assert(layer_height_profile[i] < slicing_params.max_layer_height + EPSILON);
	}
#endif /* _DEBUG */
}

// Produce object layers as pairs of low / high layer boundaries, stored into a linear vector.
std::vector<coordf_t> generate_object_layers(
	const SlicingParameters 	&slicing_params,
	const std::vector<coordf_t> &layer_height_profile)
{
    coordf_t print_z = 0;
    coordf_t height  = 0;

    std::vector<coordf_t> out;

    if (slicing_params.first_object_layer_height_fixed()) {
        out.push_back(0);
        print_z = slicing_params.first_object_layer_height;
        out.push_back(print_z);
    }

    size_t idx_layer_height_profile = 0;
    // loop until we have at least one layer and the max slice_z reaches the object height
    coordf_t slice_z = print_z + 0.5 * slicing_params.min_layer_height;
    while (slice_z < slicing_params.object_print_z_height()) {
        height = slicing_params.min_layer_height;
        if (idx_layer_height_profile < layer_height_profile.size()) {
            size_t next = idx_layer_height_profile + 2;
            for (;;) {
                if (next >= layer_height_profile.size() || slice_z < layer_height_profile[next])
                    break;
                idx_layer_height_profile = next;
                next += 2;
            }
            coordf_t z1 = layer_height_profile[idx_layer_height_profile];
            coordf_t h1 = layer_height_profile[idx_layer_height_profile + 1];
            height = h1;
            if (next < layer_height_profile.size()) {
                coordf_t z2 = layer_height_profile[next];
                coordf_t h2 = layer_height_profile[next + 1];
                height = lerp(h1, h2, (slice_z - z1) / (z2 - z1));
                assert(height >= slicing_params.min_layer_height - EPSILON && height <= slicing_params.max_layer_height + EPSILON);
            }
        }
        slice_z = print_z + 0.5 * height;
        if (slice_z >= slicing_params.object_print_z_height())
            break;
        assert(height > slicing_params.min_layer_height - EPSILON);
        assert(height < slicing_params.max_layer_height + EPSILON);
        out.push_back(print_z);
        print_z += height;
        slice_z = print_z + 0.5 * slicing_params.min_layer_height;
        out.push_back(print_z);
    }

    //FIXME Adjust the last layer to align with the top object layer exactly?
    return out;
}

int generate_layer_height_texture(
	const SlicingParameters 	&slicing_params,
	const std::vector<coordf_t> &layers,
	void *data, int rows, int cols, bool level_of_detail_2nd_level)
{
// https://github.com/aschn/gnuplot-colorbrewer
    std::vector<Point3> palette_raw;
    palette_raw.push_back(Point3(0x0B2, 0x018, 0x02B));
    palette_raw.push_back(Point3(0x0D6, 0x060, 0x04D));
    palette_raw.push_back(Point3(0x0F4, 0x0A5, 0x082));
    palette_raw.push_back(Point3(0x0FD, 0x0DB, 0x0C7));
    palette_raw.push_back(Point3(0x0D1, 0x0E5, 0x0F0));
    palette_raw.push_back(Point3(0x092, 0x0C5, 0x0DE));
    palette_raw.push_back(Point3(0x043, 0x093, 0x0C3));
    palette_raw.push_back(Point3(0x021, 0x066, 0x0AC));

    // Clear the main texture and the 2nd LOD level.
    memset(data, 0, rows * cols * 5);
    // 2nd LOD level data start
    unsigned char *data1 = reinterpret_cast<unsigned char*>(data) + rows * cols * 4;
    int ncells  = std::min((cols-1) * rows, int(ceil(16. * (slicing_params.object_print_z_height() / slicing_params.min_layer_height))));
    int ncells1 = ncells / 2;
    int cols1   = cols / 2;
    coordf_t z_to_cell = coordf_t(ncells-1) / slicing_params.object_print_z_height();
    coordf_t cell_to_z = slicing_params.object_print_z_height() / coordf_t(ncells-1);
    coordf_t z_to_cell1 = coordf_t(ncells1-1) / slicing_params.object_print_z_height();
    coordf_t cell_to_z1 = slicing_params.object_print_z_height() / coordf_t(ncells1-1);
    // for color scaling
	coordf_t hscale = 2.f * std::max(slicing_params.max_layer_height - slicing_params.layer_height, slicing_params.layer_height - slicing_params.min_layer_height);
	if (hscale == 0)
		// All layers have the same height. Provide some height scale to avoid division by zero.
		hscale = slicing_params.layer_height;
    for (size_t idx_layer = 0; idx_layer < layers.size(); idx_layer += 2) {
        coordf_t lo  = layers[idx_layer];
		coordf_t hi  = layers[idx_layer + 1];
        coordf_t mid = 0.5f * (lo + hi);
		assert(mid <= slicing_params.object_print_z_height());
		coordf_t h = hi - lo;
		hi = std::min(hi, slicing_params.object_print_z_height());
        int cell_first = clamp(0, ncells-1, int(ceil(lo * z_to_cell)));
        int cell_last  = clamp(0, ncells-1, int(floor(hi * z_to_cell)));
        for (int cell = cell_first; cell <= cell_last; ++ cell) {
            coordf_t idxf = (0.5 * hscale + (h - slicing_params.layer_height)) * coordf_t(palette_raw.size()) / hscale;
            int idx1 = clamp(0, int(palette_raw.size() - 1), int(floor(idxf)));
            int idx2 = std::min(int(palette_raw.size() - 1), idx1 + 1);
			coordf_t t = idxf - coordf_t(idx1);
            const Point3 &color1 = palette_raw[idx1];
            const Point3 &color2 = palette_raw[idx2];

            coordf_t z = cell_to_z * coordf_t(cell);
			assert(z >= lo && z <= hi);
            // Intensity profile to visualize the layers.
            coordf_t intensity = cos(M_PI * 0.7 * (mid - z) / h);

            // Color mapping from layer height to RGB.
            Pointf3 color(
                intensity * lerp(coordf_t(color1.x), coordf_t(color2.x), t), 
                intensity * lerp(coordf_t(color1.y), coordf_t(color2.y), t),
                intensity * lerp(coordf_t(color1.z), coordf_t(color2.z), t));

            int row = cell / (cols - 1);
            int col = cell - row * (cols - 1);
			assert(row >= 0 && row < rows);
			assert(col >= 0 && col < cols);
            unsigned char *ptr = (unsigned char*)data + (row * cols + col) * 4;
            ptr[0] = clamp<int>(0, 255, int(floor(color.x + 0.5)));
            ptr[1] = clamp<int>(0, 255, int(floor(color.y + 0.5)));
            ptr[2] = clamp<int>(0, 255, int(floor(color.z + 0.5)));
            ptr[3] = 255;
            if (col == 0 && row > 0) {
                // Duplicate the first value in a row as a last value of the preceding row.
                ptr[-4] = ptr[0];
                ptr[-3] = ptr[1];
                ptr[-2] = ptr[2];
                ptr[-1] = ptr[3];
            }
        }
        if (level_of_detail_2nd_level) {
            cell_first = clamp(0, ncells1-1, int(ceil(lo * z_to_cell1))); 
            cell_last  = clamp(0, ncells1-1, int(floor(hi * z_to_cell1)));
            for (int cell = cell_first; cell <= cell_last; ++ cell) {
                coordf_t idxf = (0.5 * hscale + (h - slicing_params.layer_height)) * coordf_t(palette_raw.size()) / hscale;
                int idx1 = clamp(0, int(palette_raw.size() - 1), int(floor(idxf)));
                int idx2 = std::min(int(palette_raw.size() - 1), idx1 + 1);
    			coordf_t t = idxf - coordf_t(idx1);
                const Point3 &color1 = palette_raw[idx1];
                const Point3 &color2 = palette_raw[idx2];

                coordf_t z = cell_to_z1 * coordf_t(cell);
                assert(z >= lo && z <= hi);

                // Color mapping from layer height to RGB.
                Pointf3 color(
                    lerp(coordf_t(color1.x), coordf_t(color2.x), t), 
                    lerp(coordf_t(color1.y), coordf_t(color2.y), t),
                    lerp(coordf_t(color1.z), coordf_t(color2.z), t));

                int row = cell / (cols1 - 1);
                int col = cell - row * (cols1 - 1);
    			assert(row >= 0 && row < rows/2);
    			assert(col >= 0 && col < cols/2);
                unsigned char *ptr = data1 + (row * cols1 + col) * 4;
                ptr[0] = clamp<int>(0, 255, int(floor(color.x + 0.5)));
                ptr[1] = clamp<int>(0, 255, int(floor(color.y + 0.5)));
                ptr[2] = clamp<int>(0, 255, int(floor(color.z + 0.5)));
                ptr[3] = 255;
                if (col == 0 && row > 0) {
                    // Duplicate the first value in a row as a last value of the preceding row.
                    ptr[-4] = ptr[0];
                    ptr[-3] = ptr[1];
                    ptr[-2] = ptr[2];
                    ptr[-1] = ptr[3];
                }
            }
        }
    }

    // Returns number of cells of the 0th LOD level.
    return ncells;
}

}; // namespace Slic3r
