/*
    Four-way INMP441 microphone array components

    Measured directly from the supplied STL: the visible round face is 19 mm,
    while the nominal snap barrel is 15.4 mm.  The apertures below are sized
    so the flexible catches compress during insertion and spring back below
    the 2 mm plate.

    The four microphones are placed in one row with 30 mm centre-to-centre
    spacing.  The microphone enclosure is joined to an ESP32/display case.
*/

$fn = 120;

// Main dimensions (millimetres)
ring_thickness = 2;
outer_wall_thickness = 2;
top_edge_curve_radius = 5;
top_edge_curve_segments = 40;

mount_count = 4;
mount_hole_diameter = 17.4; // Increase slightly if printed holes run tight.
lead_in = 0.3;              // Radial enlargement at the top opening.
lead_in_height = 0.6;

// Linear-array component: four fittings in one horizontal row.
linear_center_spacing = 30;
linear_plate_size = [120, 30];
linear_corner_radius = 5;
linear_mount_positions = [
    for (i = [0 : mount_count - 1])
        (i - (mount_count - 1) / 2) * linear_center_spacing
];

// Square ESP32/display enclosure attached to linear_array_large.  These
// board, opening, and spacing dimensions come from drumbocase/drumbo.scad.
large_box_wall = 3;
large_face_skin = 1.2;
large_board_size = [50, 88];
large_board_hole_pitch = [42, 78.5];
large_board_mount_depth = 5.5;
large_board_post_diameter = 6.2;
large_board_pilot_diameter = 2.1;
large_board_pilot_skin = 0.8;
large_screen_size = [42.8, 57.2];
// The Drumbo layout is turned end-for-end before the complete enclosure is
// rotated below.
large_screen_y = -(5 - 2.75);
large_screen_radius = 0.8;
large_screen_bevel = 1.2;
large_screen_bevel_depth = 0.8;
large_usb_size = [12, 7];
large_usb_depth = 8.5;
large_usb_radius = 2;
large_box_inner_size = [
    large_board_size[0] + 4,
    large_board_size[1] + 3
];
large_box_size = [
    large_box_inner_size[0] + 2 * large_box_wall,
    large_box_inner_size[1] + 2 * large_box_wall
];
large_box_rotation = 90;
large_box_rotated_size = [large_box_size[1], large_box_size[0]];
large_box_rotated_inner_size = [
    large_box_inner_size[1],
    large_box_inner_size[0]
];
large_box_corner_radius = linear_corner_radius;
large_box_corner_segments = 24;
large_box_top_edge_radius = top_edge_curve_radius;
large_box_join_overlap = outer_wall_thickness;
large_box_center_y = linear_plate_size[1] / 2
    + large_box_rotated_size[1] / 2 - large_box_join_overlap;

// The oriented STL reaches Z = -6.2 mm below the plate's top-side shoulder.
reference_mount_z_offset = -6.2;
wall_below_mount = 10;
outer_wall_bottom = reference_mount_z_offset - wall_below_mount;
microphone_array_height = 25;
microphone_array_top = outer_wall_bottom + microphone_array_height;
microphone_array_top_offset = microphone_array_top - ring_thickness;

// Keep the ESP enclosure's local +X top at its original Z position while
// extending the common bottom downward. Rotate its top face and mounted
// features up to the taller microphone enclosure at the local -X edge.
large_box_slope_angle = asin(
    microphone_array_top_offset / large_box_rotated_size[1]
);
// Start on the flat microphone face, before its perimeter roll begins.
large_box_join_blend_length = top_edge_curve_radius + 6;
large_box_join_y = linear_plate_size[1] / 2 - top_edge_curve_radius;

// Snap-in floor. The plate fits inside the two joined cavities and sits
// flush with the enclosure bottom. Four flexible tabs engage blind pockets
// in the walls, leaving the exterior faces uninterrupted.
floor_thickness = 2;
floor_clearance = 0.3;
floor_tab_width = 10;
floor_tab_thickness = 1.2;
floor_tab_height = 6;
floor_hook_depth = 0.8;
floor_hook_height = 1;
floor_slot_clearance = 0.2;
floor_microphone_tab_x = [-30, 30];
floor_esp_tab_x = [-25, 25];
floor_print_offset = [0, -105, 0];
floor_assembly_offset = [0, 0, -20];

// Select "assembly", "body", "floor", or "both". "assembly" is an
// exploded view; "both" lays out the printable parts on the same Z plane.
output_part = "assembly";

// Display the supplied fittings in OpenSCAD as non-exported reference parts.
show_reference_mounts = true;
mount_stl = "../inputs/INMP441+Mic+Mount.STL";

epsilon = 0.02;

module snap_socket() {
    // Straight bore retains the flexible hooks below the plate.
    translate([0, 0, -epsilon])
        cylinder(d = mount_hole_diameter,
                 h = ring_thickness + 2 * epsilon);

    // Small top chamfer guides the barrel into the bore.
    translate([0, 0, ring_thickness - lead_in_height])
        cylinder(d1 = mount_hole_diameter,
                 d2 = mount_hole_diameter + 2 * lead_in,
                 h = lead_in_height + epsilon);
}

// Quintic Bezier evaluation.  The first three and last three control points
// are collinear and equally spaced, giving zero endpoint curvature.  This
// makes the rounded corners G2-continuous with adjoining straight faces.
function bezier5(p0, p1, p2, p3, p4, p5, t) =
    p0 * pow(1 - t, 5)
    + p1 * 5 * pow(1 - t, 4) * t
    + p2 * 10 * pow(1 - t, 3) * pow(t, 2)
    + p3 * 10 * pow(1 - t, 2) * pow(t, 3)
    + p4 * 5 * (1 - t) * pow(t, 4)
    + p5 * pow(t, 5);

module rounded_rectangle_2d(size, radius) {
    offset(r = radius)
        square([size[0] - 2 * radius, size[1] - 2 * radius],
               center = true);
}

module linear_socket_pattern() {
    for (x = linear_mount_positions)
        translate([x, 0, 0]) snap_socket();
}

module linear_array_large_sloped_esp_transform() {
    // In ESP-local coordinates the unchanged edge is +X. Rotating about
    // that edge preserves all feature dimensions on the sloped face.
    translate([0, large_box_center_y, 0])
        rotate([0, 0, large_box_rotation])
            translate([large_box_size[0] / 2, 0, ring_thickness])
                rotate([0, large_box_slope_angle, 0])
                    translate([
                        -large_box_size[0] / 2,
                        0,
                        -ring_thickness
                    ])
                        children();
}

module linear_array_large_global_slope_transform() {
    far_edge_y = large_box_center_y + large_box_rotated_size[1] / 2;

    translate([0, far_edge_y, ring_thickness])
        rotate([-large_box_slope_angle, 0, 0])
            translate([0, -far_edge_y, -ring_thickness])
                children();
}

module linear_array_large_box_profile_2d(
    size,
    corner_radius,
    edge_inset = 0
) {
    min_x = -size[0] / 2 + edge_inset;
    max_x = size[0] / 2 - edge_inset;
    min_y = -size[1] / 2;
    max_y = size[1] / 2 - edge_inset;
    r = min(
        max(corner_radius - edge_inset, 0),
        min(max_x - min_x, max_y - min_y) / 2
    );

    // The two +Y corners use the same quintic construction as the other G2
    // edges: three collinear controls at each end give zero join curvature.
    positive_x_controls = [
        [max_x, max_y - r],
        [max_x, max_y - 4 * r / 5],
        [max_x, max_y - 3 * r / 5],
        [max_x - 3 * r / 5, max_y],
        [max_x - 4 * r / 5, max_y],
        [max_x - r, max_y]
    ];
    negative_x_controls = [
        [min_x + r, max_y],
        [min_x + 4 * r / 5, max_y],
        [min_x + 3 * r / 5, max_y],
        [min_x, max_y - 3 * r / 5],
        [min_x, max_y - 4 * r / 5],
        [min_x, max_y - r]
    ];

    if (r <= epsilon) {
        polygon([
            [min_x, min_y], [max_x, min_y],
            [max_x, max_y], [min_x, max_y]
        ]);
    } else {
        polygon(points = concat(
            [[min_x, min_y], [max_x, min_y]],
            [
                for (i = [0 : large_box_corner_segments])
                    bezier5(
                        positive_x_controls[0],
                        positive_x_controls[1],
                        positive_x_controls[2],
                        positive_x_controls[3],
                        positive_x_controls[4],
                        positive_x_controls[5],
                        i / large_box_corner_segments
                    )
            ],
            [
                for (i = [0 : large_box_corner_segments])
                    bezier5(
                        negative_x_controls[0],
                        negative_x_controls[1],
                        negative_x_controls[2],
                        negative_x_controls[3],
                        negative_x_controls[4],
                        negative_x_controls[5],
                        i / large_box_corner_segments
                    )
            ]
        ));
    }
}

module snap_in_floor_outline_2d() {
    microphone_inner_size = [
        linear_plate_size[0] - 2 * outer_wall_thickness,
        linear_plate_size[1] - 2 * outer_wall_thickness
    ];
    microphone_inner_radius = max(
        linear_corner_radius - outer_wall_thickness,
        0.01
    );
    esp_inner_radius = max(
        large_box_corner_radius - large_box_wall,
        0.01
    );
    microphone_inner_top = microphone_inner_size[1] / 2;
    esp_inner_bottom = large_box_center_y
        - large_box_rotated_inner_size[1] / 2;
    bridge_width = large_box_rotated_inner_size[0]
        - 2 * floor_clearance;

    union() {
        offset(delta = -floor_clearance)
            rounded_rectangle_2d(
                microphone_inner_size,
                microphone_inner_radius
            );

        translate([0, large_box_center_y])
            offset(delta = -floor_clearance)
                linear_array_large_box_profile_2d(
                    large_box_rotated_inner_size,
                    esp_inner_radius
                );

        // Bridge the original shared-wall gap so the floor is one component.
        translate([
            -bridge_width / 2,
            microphone_inner_top - floor_clearance - epsilon
        ])
            square([
                bridge_width,
                esp_inner_bottom - microphone_inner_top
                    + 2 * floor_clearance + 2 * epsilon
            ]);
    }
}

module snap_in_floor_hook(x, y, direction) {
    plate_top = outer_wall_bottom + floor_thickness;
    hook_bottom = plate_top + floor_tab_height - floor_hook_height;
    hook_tip_y = y + direction * floor_hook_depth;

    hull() {
        translate([
            x - floor_tab_width / 2,
            hook_tip_y - epsilon / 2,
            hook_bottom
        ])
            cube([floor_tab_width, epsilon, epsilon]);
        translate([
            x - floor_tab_width / 2,
            y - epsilon / 2,
            plate_top + floor_tab_height
        ])
            cube([floor_tab_width, epsilon, epsilon]);
    }
}

module snap_in_floor_tab(x, y, direction) {
    plate_top = outer_wall_bottom + floor_thickness;
    beam_y = direction < 0 ? y : y - floor_tab_thickness;

    translate([
        x - floor_tab_width / 2,
        beam_y,
        plate_top - epsilon
    ])
        cube([
            floor_tab_width,
            floor_tab_thickness,
            floor_tab_height + epsilon
        ]);

    snap_in_floor_hook(x, y, direction);
}

module snap_in_floor() {
    microphone_tab_y = -(
        linear_plate_size[1] / 2
        - outer_wall_thickness
        - floor_clearance
    );
    esp_tab_y = large_box_center_y
        + large_box_rotated_inner_size[1] / 2
        - floor_clearance;

    union() {
        translate([0, 0, outer_wall_bottom])
            linear_extrude(height = floor_thickness)
                snap_in_floor_outline_2d();

        for (x = floor_microphone_tab_x)
            snap_in_floor_tab(x, microphone_tab_y, -1);

        for (x = floor_esp_tab_x)
            snap_in_floor_tab(x, esp_tab_y, 1);
    }
}

module snap_in_floor_retention_slots() {
    slot_bottom = outer_wall_bottom + floor_thickness
        + floor_tab_height - floor_hook_height
        - floor_slot_clearance;
    slot_height = floor_hook_height + 2 * floor_slot_clearance;
    slot_width = floor_tab_width + 2 * floor_slot_clearance;
    microphone_inner_y = -(
        linear_plate_size[1] / 2 - outer_wall_thickness
    );
    esp_inner_y = large_box_center_y
        + large_box_rotated_inner_size[1] / 2;

    for (x = floor_microphone_tab_x)
        translate([
            x - slot_width / 2,
            microphone_inner_y - floor_hook_depth - floor_slot_clearance,
            slot_bottom
        ])
            cube([
                slot_width,
                floor_hook_depth + 2 * floor_slot_clearance,
                slot_height
            ]);

    for (x = floor_esp_tab_x)
        translate([
            x - slot_width / 2,
            esp_inner_y - floor_slot_clearance,
            slot_bottom
        ])
            cube([
                slot_width,
                floor_hook_depth + 2 * floor_slot_clearance,
                slot_height
            ]);
}

function large_box_g2_edge_inset(t) =
    large_box_top_edge_radius
        * bezier5(0, 0, 0, 0.6, 0.8, 1, t);

function large_box_g2_edge_rise(t) =
    large_box_top_edge_radius
        * bezier5(0, 0.2, 0.4, 1, 1, 1, t);

function large_box_sloped_face_z(y) =
    ring_thickness
        + tan(large_box_slope_angle)
            * (large_box_center_y
               + large_box_rotated_size[1] / 2 - y);

function large_box_join_blend_point(t) =
    let(
        y0 = large_box_join_y,
        y1 = large_box_join_y + large_box_join_blend_length,
        z0 = microphone_array_top,
        z1 = large_box_sloped_face_z(y1),
        slope = -tan(large_box_slope_angle),
        p0 = [y0, z0],
        p1 = [y0 + large_box_join_blend_length / 5, z0],
        p2 = [y0 + 2 * large_box_join_blend_length / 5, z0],
        p5 = [y1, z1],
        p4 = [
            y1 - large_box_join_blend_length / 5,
            z1 - slope * large_box_join_blend_length / 5
        ],
        p3 = 2 * p4 - p5
    )
        bezier5(p0, p1, p2, p3, p4, p5, t);

module linear_array_large_box_profile_slice(
    size,
    corner_radius,
    edge_inset,
    z,
    sloped = false
) {
    if (sloped)
        linear_array_large_global_slope_transform()
            translate([0, large_box_center_y, z])
                linear_extrude(height = epsilon, center = true)
                    linear_array_large_box_profile_2d(
                        size,
                        corner_radius,
                        edge_inset
                    );
    else
        translate([0, large_box_center_y, z])
            linear_extrude(height = epsilon, center = true)
                linear_array_large_box_profile_2d(
                    size,
                    corner_radius,
                    edge_inset
                );
}

module linear_array_large_esp_cavity() {
    box_bottom = outer_wall_bottom;
    cavity_top = ring_thickness - large_face_skin;
    // Loft the cavity to the rotated face underside. This preserves the
    // face skin and side walls while leaving the bottom plane unchanged.
    hull() {
        linear_array_large_box_profile_slice(
            large_box_rotated_inner_size,
            max(large_box_corner_radius - large_box_wall, 0.01),
            0,
            box_bottom - epsilon
        );
        linear_array_large_box_profile_slice(
            large_box_rotated_inner_size,
            max(large_box_corner_radius - large_box_wall, 0.01),
            0,
            cavity_top,
            true
        );
    }
}

module linear_array_large_screen_cutout() {
    linear_array_large_sloped_esp_transform()
        translate([0, large_screen_y, ring_thickness]) {
            translate([0, 0, -large_face_skin - epsilon])
                linear_extrude(height = large_face_skin + 2 * epsilon)
                    rounded_rectangle_2d(
                        large_screen_size,
                        large_screen_radius
                    );

            hull() {
                translate([0, 0, -large_screen_bevel_depth])
                    linear_extrude(height = epsilon)
                        rounded_rectangle_2d(
                            large_screen_size,
                            large_screen_radius
                        );
                linear_extrude(height = epsilon)
                    rounded_rectangle_2d(
                        large_screen_size
                            + [2 * large_screen_bevel,
                               2 * large_screen_bevel],
                        large_screen_radius + large_screen_bevel
                    );
            }
        }
}

module linear_array_large_board_posts() {
    linear_array_large_sloped_esp_transform()
        for (x_sign = [-1, 1], y_sign = [-1, 1])
            translate([
                x_sign * large_board_hole_pitch[0] / 2,
                y_sign * large_board_hole_pitch[1] / 2,
                ring_thickness
            ])
                difference() {
                    translate([0, 0, -large_board_mount_depth])
                        cylinder(
                            d = large_board_post_diameter,
                            h = large_board_mount_depth
                                - large_face_skin + epsilon
                        );
                    translate([
                        0,
                        0,
                        -large_board_mount_depth - epsilon
                    ])
                        cylinder(
                            d = large_board_pilot_diameter,
                            h = large_board_mount_depth
                                - large_board_pilot_skin + epsilon
                        );
                }
}

module linear_array_large_usb_cutout() {
    box_outer_y = large_box_size[1] / 2;

    linear_array_large_sloped_esp_transform()
        translate([
            0,
            box_outer_y + epsilon,
            ring_thickness - large_usb_depth
        ])
            rotate([90, 0, 0])
                linear_extrude(height = large_box_wall + 2 * epsilon)
                    rounded_rectangle_2d(
                        large_usb_size,
                        large_usb_radius
                    );
}

module linear_array_large_shared_wall_cut() {
    box_inner_near_y = large_box_center_y
        - large_box_rotated_inner_size[1] / 2;
    passage_start_y = linear_plate_size[1] / 2
        - large_box_join_overlap - outer_wall_thickness;
    passage_end_y = box_inner_near_y + outer_wall_thickness;

    // Join the microphone and ESP cavities below their uninterrupted top.
    // Overlap both cavities so the rotated ESP shell cannot leave a membrane
    // on either side of the passage.
    translate([
        -large_box_rotated_inner_size[0] / 2,
        passage_start_y - epsilon,
        outer_wall_bottom - epsilon
    ])
        cube([
            large_box_rotated_inner_size[0],
            passage_end_y - passage_start_y + 2 * epsilon,
            microphone_array_top - ring_thickness
                - outer_wall_bottom + epsilon
        ]);
}

// One perimeter loft rounds the joined enclosure, including its concave
// shoulders. Separate fillets and an additive strip leave steps at the join.
// Every ring has matching vertices; the top is capped in transverse strips
// so it follows the blend instead of triangulating across the sloping face.
function joined_outline_right(inset) =
    let(
        r = max(top_edge_curve_radius - inset, 0.001),
        cr = top_edge_curve_radius + inset,
        mx = linear_plate_size[0] / 2,
        my = linear_plate_size[1] / 2,
        ex = large_box_rotated_size[0] / 2,
        ey = large_box_center_y + large_box_rotated_size[1] / 2,
        n = top_edge_curve_segments
    ) concat(
        [[0, -my + inset]],
        [for (i = [0:n]) let(a = -90 + 90*i/n)
            [mx - top_edge_curve_radius + r*cos(a),
             -my + top_edge_curve_radius + r*sin(a)]],
        [for (i = [1:n])
            [mx - inset, -my + top_edge_curve_radius
                + (2*my - 2*top_edge_curve_radius)*i/n]],
        [for (i = [1:n]) let(a = 90*i/n)
            [mx - top_edge_curve_radius + r*cos(a),
             my - top_edge_curve_radius + r*sin(a)]],
        [for (i = [0:n]) let(a = -90 - 90*i/n)
            [ex + top_edge_curve_radius + cr*cos(a),
             my + top_edge_curve_radius + cr*sin(a)]],
        [for (i = [1:4*n])
            [ex - inset, my + top_edge_curve_radius
                + (ey - my - 2*top_edge_curve_radius)*i/(4*n)]],
        [for (i = [1:n]) let(a = 90*i/n)
            [ex - top_edge_curve_radius + r*cos(a),
             ey - top_edge_curve_radius + r*sin(a)]],
        [[0, ey - inset]]
    );

function joined_outline(inset) =
    let(right = joined_outline_right(inset))
    concat(right, [for (i = [len(right)-2:-1:1])
        [-right[i][0], right[i][1]]]);

function joined_face_z(y) =
    y <= large_box_join_y ? microphone_array_top :
    y >= large_box_join_y + large_box_join_blend_length
        ? large_box_sloped_face_z(y)
        : large_box_join_blend_point(
            (y - large_box_join_y) / large_box_join_blend_length)[1];

module joined_outer_shell_solid() {
    n = top_edge_curve_segments;
    count = len(joined_outline(0));
    half_count = len(joined_outline_right(0));
    points = concat(
        [for (p = joined_outline(0)) [p[0], p[1], outer_wall_bottom]],
        [for (j = [0:n])
            let(t = j/n,
                inset = large_box_g2_edge_inset(t),
                drop = top_edge_curve_radius - large_box_g2_edge_rise(t))
            for (p = joined_outline(inset))
                [p[0], p[1], joined_face_z(p[1]) - drop]]
    );
    faces = concat(
        // Side quads are split consistently, including on the curved join.
        [for (j = [0:n], i = [0:count-1])
            let(a=j*count+i, b=j*count+(i+1)%count,
                c=b+count, d=a+count)
            each [[a,d,c], [a,c,b]]],
        [for (i = [0:half_count-2])
            let(a=i, b=(count-i)%count,
                c=(count-i-1)%count, d=i+1)
            each [[a,c,b], [a,d,c]]],
        [for (i = [0:half_count-2])
            let(k=(n+1)*count,
                a=k+i, b=k+(count-i)%count,
                c=k+(count-i-1)%count, d=k+i+1)
            each [[a,b,c], [a,c,d]]]
    );
    polyhedron(points=points, faces=faces, convexity=10);
}

module linear_array_large_body() {
    difference() {
        union() {
            difference() {
                joined_outer_shell_solid();
                // Preserve the original cavities and mounting clearances.
                translate([0, 0, outer_wall_bottom - epsilon])
                    linear_extrude(height = microphone_array_top
                        - ring_thickness - outer_wall_bottom + epsilon)
                        rounded_rectangle_2d(
                            linear_plate_size - [2*outer_wall_thickness,
                                                 2*outer_wall_thickness],
                            linear_corner_radius - outer_wall_thickness);
                linear_array_large_esp_cavity();
            }
            linear_array_large_board_posts();
        }
        translate([0, 0, microphone_array_top_offset])
            linear_socket_pattern();
        linear_array_large_screen_cutout();
        linear_array_large_usb_cutout();
        linear_array_large_shared_wall_cut();
        snap_in_floor_retention_slots();
    }
}

module reference_mount() {
    // Source axis is +Y. Rotate it so the perforated microphone face is +Z,
    // then place the flange underside on the top surface of the ring.
    translate([
        -11.5,
        11.5,
        reference_mount_z_offset + microphone_array_top_offset
    ])
        rotate([90, 0, 0])
            import(mount_stl, convexity = 10);
}

module linear_array_large(show_mounts = true) {
    linear_array_large_body();

    if (show_mounts && show_reference_mounts)
        for (x = linear_mount_positions)
            %translate([x, 0, 0]) reference_mount();
}

if (output_part == "assembly") {
    linear_array_large();
    translate(floor_assembly_offset) snap_in_floor();
} else if (output_part == "body") {
    linear_array_large();
} else if (output_part == "floor") {
    snap_in_floor();
} else if (output_part == "both") {
    linear_array_large();
    translate(floor_print_offset) snap_in_floor();
} else {
    assert(false, str("Unknown output_part: ", output_part));
}
