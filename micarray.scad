/*
    Four-way INMP441 microphone array components

    Measured directly from the supplied STL: the visible round face is 19 mm,
    while the nominal snap barrel is 15.4 mm.  The apertures below are sized
    so the flexible catches compress during insertion and spring back below
    the 2 mm plate.

    The circular_array component has adjacent microphone centres exactly
    40 mm apart.  The linear_array component places all four microphones in
    one row with 30 mm centre-to-centre spacing.
*/

$fn = 120;

// Select the component to display/export.
component = "linear_array"; // [circular_array, linear_array, both]

// Main dimensions (millimetres)
outer_diameter = 85;
ring_thickness = 2;
outer_wall_thickness = 2;
outer_edge_curve_radius = 2;
outer_edge_curve_segments = 24;
top_edge_curve_radius = 0.6;
top_edge_curve_segments = 20;

mount_count = 4;
mount_center_spacing = 40;
// Four centres at 90 degrees form a square: side = radius * sqrt(2).
mount_pitch_radius = mount_center_spacing / sqrt(2);
mount_hole_diameter = 15.8; // Increase slightly if printed holes run tight.
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

// Board, opening, and internal-support measurements taken from
// grudge-cat-case.scad.  No geometry is imported from that design.
board_pcb_w = 25.50;
board_pcb_l = 51.65;
board_pcb_clearance = 0.40;
board_base_body_w = 29.00;
board_base_body_l = 56.00;
board_base_inside_pocket_w = 25.490;
board_base_internal_l = (49.020 + 4.670) - 2.024;
board_target_internal_w = board_pcb_w + 2 * board_pcb_clearance;
board_target_internal_l = board_pcb_l + 2 * board_pcb_clearance;
board_fit_width_growth =
    board_target_internal_w - board_base_inside_pocket_w;
board_fit_length_growth =
    board_target_internal_l - board_base_internal_l;
board_case_w = board_base_body_w + board_fit_width_growth;
board_case_l = board_base_body_l + board_fit_length_growth;
board_face_h = 4.50;
board_case_join_overlap = 2;
board_wall_growth = 0.50;
board_x_feature_shift = board_fit_width_growth / 2;

board_inside_pocket_x = 1.710;
board_inside_pocket_y = 17.670;
board_inside_pocket_w = board_case_w - 2 * board_inside_pocket_x;
board_inside_pocket_l = 31.350 + board_fit_length_growth;
board_inside_pocket_z = 2.330;

board_screen_face_x = 4.407 + board_x_feature_shift;
board_screen_face_y = 16.725;
board_screen_face_w = 20.096;
board_screen_face_l = 31.25;
board_screen_open_x = 6.66 + board_x_feature_shift;
board_screen_open_y = 18.98;
board_screen_open_w = 15.60;
board_screen_open_l = 26.74;
board_screen_corner_r = 0.20;

board_button_hole_d = 6.0;
board_button_counterbore_d = 7.18;
board_button_counterbore_z = 0.50;
board_button_well_z = 2.50;
board_button_y = 5.68;
board_button_left_x = 5.78 + board_x_feature_shift;
board_button_right_x = 23.13 + board_x_feature_shift;
board_button_well_w = 8.14;
board_button_well_y = 2.024;
board_button_well_l = 7.822;
board_button_well_r = 3.18;
board_button_corner_trim = 2.80;

board_lower_notch_x = 9.885 + board_x_feature_shift;
board_lower_notch_l = 9.846;
board_lower_center_recess_x = 11.135 + board_x_feature_shift;
board_lower_center_recess_y = 0.148;
board_lower_center_recess_w = 6.640;
board_lower_center_recess_l = 9.698;
board_lower_center_recess_z = 1.070;

board_between_recess_x = 1.710;
board_between_recess_y = 9.842;
board_between_recess_w = board_inside_pocket_w;
board_between_recess_l = 7.128;
board_between_recess_z = 1.555;

board_far_support_x = 1.710;
board_far_support_y = 49.020 + board_fit_length_growth;
board_far_support_w = board_inside_pocket_w;
board_far_support_l = 4.670;
board_far_support_z = 2.330;
board_far_support_h = 2.100;

board_usb_face_x = board_lower_notch_x;
board_usb_face_w = 9.14;
board_usb_face_z = 2.00;
board_usb_face_depth = 2.024;
board_usb_mouth_center_x = board_usb_face_x + board_usb_face_w / 2;
board_usb_mouth_left_arc_profile = [
    [9.885, 2.320], [9.885, 2.212], [9.890, 2.212],
    [9.904, 2.105], [9.927, 2.000], [9.997, 1.803],
    [10.099, 1.622], [10.224, 1.464], [10.371, 1.331],
    [10.542, 1.220], [10.732, 1.137], [10.933, 1.087],
    [11.135, 1.070]
];

board_reset_slot_x = 0.000;
board_reset_slot_y = 12.005;
board_reset_slot_w = 2.000;
board_reset_slot_l = 3.231;
board_reset_slot_z = 2.365;
board_reset_slot_r = 0.10;
board_reset_slot_top_extension = 0.65;

// The oriented STL reaches Z = -6.2 mm below the plate's top-side shoulder.
reference_mount_z_offset = -6.2;
wall_below_mount = 5;
outer_wall_bottom = reference_mount_z_offset - wall_below_mount;

// Match the board enclosure's Z extent to the microphone array enclosure.
board_case_h = ring_thickness - outer_wall_bottom;
board_plain_wall_h = board_case_h - board_face_h;
board_far_wall_height = 8.00;
board_slope_drop =
    ring_thickness - (outer_wall_bottom + board_far_wall_height);
board_slope_face_length = board_case_w + 2 * board_wall_growth;
board_slope_projected_run = sqrt(
    board_slope_face_length * board_slope_face_length
    - board_slope_drop * board_slope_drop
);
board_slope_cos = board_slope_projected_run / board_slope_face_length;
board_slope_sin = board_slope_drop / board_slope_face_length;
board_slope_tan = board_slope_drop / board_slope_projected_run;
board_slope_x_max = board_case_w + board_wall_growth;
board_slope_x_min = board_slope_x_max - board_slope_projected_run;

// Display the supplied fittings in OpenSCAD as non-exported reference parts.
show_reference_mounts = true;
mount_stl = "inputs/INMP441+Mic+Mount.STL";

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

module circular_socket_pattern() {
    for (angle = [0 : 360 / mount_count : 360 - 360 / mount_count])
        rotate([0, 0, angle])
            translate([mount_pitch_radius, 0, 0])
                snap_socket();
}

module circular_plate() {
    difference() {
        cylinder(d = outer_diameter, h = ring_thickness);
        circular_socket_pattern();
    }
}

module circular_outer_wall() {
    // Cylindrical face around the plate, extending 5 mm below the fittings.
    translate([0, 0, outer_wall_bottom])
        difference() {
            cylinder(d = outer_diameter,
                     h = ring_thickness - outer_wall_bottom);
            translate([0, 0, -epsilon])
                cylinder(d = outer_diameter - 2 * outer_wall_thickness,
                         h = ring_thickness - outer_wall_bottom
                             + 2 * epsilon);
        }
}

// Quintic Bezier evaluation.  The first three and last three control points
// are collinear and equally spaced, giving zero endpoint curvature.  This
// makes the revolved corner G2-continuous with both adjoining straight faces.
function bezier5(p0, p1, p2, p3, p4, p5, t) =
    p0 * pow(1 - t, 5)
    + p1 * 5 * pow(1 - t, 4) * t
    + p2 * 10 * pow(1 - t, 3) * pow(t, 2)
    + p3 * 10 * pow(1 - t, 2) * pow(t, 3)
    + p4 * 5 * (1 - t) * pow(t, 4)
    + p5 * pow(t, 5);

module circular_g2_outer_edge_cutter() {
    radius = outer_diameter / 2;
    curve = outer_edge_curve_radius;

    p0 = [0, 0];
    p1 = [0, curve / 5];
    p2 = [0, 2 * curve / 5];
    p3 = [3 * curve / 5, curve];
    p4 = [4 * curve / 5, curve];
    p5 = [curve, curve];

    profile = [
        for (i = [0 : outer_edge_curve_segments])
            let(
                t = i / outer_edge_curve_segments,
                point = bezier5(p0, p1, p2, p3, p4, p5, t)
            )
            [radius - point[0],
             ring_thickness - curve + point[1]]
    ];

    rotate_extrude(convexity = 4)
        polygon(points = concat(
            profile,
            [
                [radius + epsilon, ring_thickness + epsilon],
                [radius + epsilon,
                 ring_thickness - curve - epsilon]
            ]
        ));
}

module circular_array_body() {
    difference() {
        union() {
            circular_plate();
            circular_outer_wall();
        }
        circular_g2_outer_edge_cutter();
    }
}

module rounded_rectangle_2d(size, radius) {
    offset(r = radius)
        square([size[0] - 2 * radius, size[1] - 2 * radius],
               center = true);
}

module linear_socket_pattern() {
    for (x = linear_mount_positions)
        translate([x, 0, 0]) snap_socket();
}

module linear_plate() {
    difference() {
        linear_extrude(height = ring_thickness)
            rounded_rectangle_2d(linear_plate_size, linear_corner_radius);
        linear_socket_pattern();
    }
}

module linear_outer_wall() {
    inner_size = [
        linear_plate_size[0] - 2 * outer_wall_thickness,
        linear_plate_size[1] - 2 * outer_wall_thickness
    ];
    inner_radius = max(linear_corner_radius - outer_wall_thickness, 0.01);

    translate([0, 0, outer_wall_bottom])
        linear_extrude(height = ring_thickness - outer_wall_bottom)
            difference() {
                rounded_rectangle_2d(linear_plate_size,
                                     linear_corner_radius);
                rounded_rectangle_2d(inner_size, inner_radius);
            }
}

module board_rounded_rect_2d(w, l, r) {
    if (r <= 0) {
        square([w, l]);
    } else {
        offset(r = r)
            translate([r, r])
                square([w - 2 * r, l - 2 * r]);
    }
}

module board_rounded_prism(w, l, h, r) {
    linear_extrude(height = h)
        board_rounded_rect_2d(w, l, r);
}

module board_outer_profile_2d() {
    // In source coordinates, X=0 becomes the enclosure's outward +Y edge
    // after placement.  Round its two corners with the array's 5 mm profile;
    // retain square corners at the shared wall on X=board_case_w.
    r = linear_corner_radius;
    x0 = -board_wall_growth;
    y0 = -board_wall_growth;
    w = board_case_w + 2 * board_wall_growth;
    l = board_case_l + 2 * board_wall_growth;

    union() {
        translate([x0 + r, y0]) square([w - r, l]);
        translate([x0, y0 + r]) square([r, l - 2 * r]);
        translate([x0 + r, y0 + r]) circle(r = r);
        translate([x0 + r, y0 + l - r]) circle(r = r);
    }
}

module board_tapered_rect_cut(
    bottom_x, bottom_y, bottom_w, bottom_l, bottom_r,
    top_x, top_y, top_w, top_l, top_r,
    z0, z1
) {
    hull() {
        translate([bottom_x, bottom_y, z0])
            linear_extrude(height = epsilon)
                board_rounded_rect_2d(
                    bottom_w, bottom_l, bottom_r
                );
        translate([top_x, top_y, z1 - epsilon])
            linear_extrude(height = epsilon)
                board_rounded_rect_2d(top_w, top_l, top_r);
    }
}

module board_tapered_cylinder_cut(x, y, d0, d1, z0, z1) {
    hull() {
        translate([x, y, z0]) cylinder(d = d0, h = epsilon);
        translate([x, y, z1 - epsilon])
            cylinder(d = d1, h = epsilon);
    }
}

module board_ramp_cut(x, y0, w, y1, z0, z1) {
    multmatrix([
        [0, 0, 1, x],
        [1, 0, 0, 0],
        [0, 1, 0, 0],
        [0, 0, 0, 1]
    ])
        linear_extrude(height = w)
            polygon([
                [y0, z0], [y1, z1],
                [y1, board_face_h + epsilon],
                [y0, board_face_h + epsilon]
            ]);
}

module board_xz_profile_extrude_y(points, y0, depth) {
    multmatrix([
        [1, 0, 0, 0],
        [0, 0, 1, y0],
        [0, 1, 0, 0],
        [0, 0, 0, 1]
    ])
        linear_extrude(height = depth)
            polygon(points);
}

module board_inside_pocket_cut() {
    translate([
        board_inside_pocket_x,
        board_inside_pocket_y,
        board_inside_pocket_z
    ])
        board_rounded_prism(
            board_inside_pocket_w,
            board_inside_pocket_l,
            board_face_h - board_inside_pocket_z + epsilon,
            0.20
        );
}

module board_screen_cut() {
    board_tapered_rect_cut(
        board_screen_face_x,
        board_screen_face_y,
        board_screen_face_w,
        board_screen_face_l,
        0,
        board_screen_open_x,
        board_screen_open_y,
        board_screen_open_w,
        board_screen_open_l,
        board_screen_corner_r,
        -epsilon,
        board_inside_pocket_z
    );

    translate([
        board_screen_open_x,
        board_screen_open_y,
        board_inside_pocket_z - epsilon
    ])
        board_rounded_prism(
            board_screen_open_w,
            board_screen_open_l,
            board_face_h - board_inside_pocket_z + 2 * epsilon,
            board_screen_corner_r
        );
}

module board_button_opening_cut(x) {
    translate([x, board_button_y, -epsilon])
        cylinder(d = board_button_hole_d,
                 h = board_face_h + 2 * epsilon);

    board_tapered_cylinder_cut(
        x, board_button_y,
        board_button_hole_d,
        board_button_counterbore_d,
        -epsilon,
        board_button_counterbore_z
    );

    translate([
        x,
        board_button_y,
        board_button_counterbore_z + 0.005
    ])
        cylinder(
            d = board_button_counterbore_d,
            h = board_button_well_z
                - board_button_counterbore_z + epsilon
        );
}

module board_button_well_cut(x) {
    translate([
        x - board_button_well_w / 2,
        board_button_well_y,
        board_button_well_z
    ])
        board_rounded_prism(
            board_button_well_w,
            board_button_well_l,
            board_face_h - board_button_well_z + epsilon,
            board_button_well_r
        );
}

module board_triangular_cut(points) {
    translate([0, 0, board_button_well_z])
        linear_extrude(
            height = board_face_h - board_button_well_z + epsilon
        )
            polygon(points);
}

module board_button_corner_trim_cuts(x) {
    x0 = x - board_button_well_w / 2;
    x1 = x + board_button_well_w / 2;
    y0 = board_button_well_y;
    y1 = board_button_well_y + board_button_well_l;
    t = board_button_corner_trim;

    if (x0 > board_case_w / 2)
        board_triangular_cut([[x0, y0], [x0 + t, y0], [x0, y0 + t]]);
    if (x1 < board_case_w / 2)
        board_triangular_cut([[x1, y0], [x1 - t, y0], [x1, y0 + t]]);

    board_triangular_cut([[x0, y1], [x0 + t, y1], [x0, y1 - t]]);
    board_triangular_cut([[x1, y1], [x1 - t, y1], [x1, y1 - t]]);
}

module board_button_internal_clearance_cuts() {
    translate([
        board_between_recess_x,
        board_lower_notch_l - 0.06,
        board_button_well_z
    ])
        board_rounded_prism(
            board_between_recess_w,
            0.08,
            board_face_h - board_button_well_z + epsilon,
            0.05
        );

    left_x = board_button_left_x + board_button_well_w / 2 - 0.10;
    right_x = board_lower_center_recess_x
        + board_lower_center_recess_w - 0.10;

    translate([left_x, board_button_well_y, board_button_well_z])
        cube([
            board_lower_center_recess_x - left_x + 0.10,
            board_lower_center_recess_l,
            board_face_h - board_button_well_z + epsilon
        ]);
    translate([right_x, board_button_well_y, board_button_well_z])
        cube([
            board_button_right_x - board_button_well_w / 2
                - right_x + 0.10,
            board_lower_center_recess_l,
            board_face_h - board_button_well_z + epsilon
        ]);

    translate([
        board_between_recess_x,
        board_lower_notch_l - 0.16,
        board_button_well_z
    ])
        cube([
            board_between_recess_w,
            0.34,
            board_face_h - board_button_well_z + epsilon
        ]);
}

module board_lower_internal_recess_cuts() {
    translate([
        board_lower_center_recess_x,
        board_lower_center_recess_y,
        board_lower_center_recess_z
    ])
        cube([
            board_lower_center_recess_w,
            board_lower_center_recess_l,
            board_face_h - board_lower_center_recess_z + epsilon
        ]);

    translate([
        board_between_recess_x,
        board_between_recess_y,
        board_between_recess_z
    ])
        cube([
            board_between_recess_w,
            board_between_recess_l,
            board_face_h - board_between_recess_z + epsilon
        ]);

    board_ramp_cut(
        board_between_recess_x,
        board_between_recess_y + board_between_recess_l,
        board_between_recess_w,
        board_inside_pocket_y + epsilon,
        board_between_recess_z + 0.075,
        board_inside_pocket_z
    );
}

module board_far_support_top_cut() {
    translate([
        board_far_support_x,
        board_far_support_y,
        board_far_support_z + board_far_support_h
    ])
        cube([
            board_far_support_w,
            board_far_support_l,
            board_face_h
                - board_far_support_z - board_far_support_h + epsilon
        ]);
}

module board_far_support() {
    translate([
        board_far_support_x,
        board_far_support_y,
        board_far_support_z
    ])
        cube([
            board_far_support_w,
            board_far_support_l,
            board_far_support_h
        ]);
}

module board_usb_mouth_cut() {
    outer_left_x = board_usb_face_x;
    outer_right_x = board_usb_face_x + board_usb_face_w;
    profile = concat(
        [
            [outer_left_x, board_face_h + 0.65 + epsilon],
            [outer_right_x, board_face_h + 0.65 + epsilon]
        ],
        [
            for (i = [0 : len(board_usb_mouth_left_arc_profile) - 1])
                [
                    2 * board_usb_mouth_center_x
                        - (board_usb_mouth_left_arc_profile[i][0]
                           + board_x_feature_shift),
                    board_usb_mouth_left_arc_profile[i][1]
                ]
        ],
        [
            for (i = [len(board_usb_mouth_left_arc_profile) - 1 : -1 : 0])
                [
                    board_usb_mouth_left_arc_profile[i][0]
                        + board_x_feature_shift,
                    board_usb_mouth_left_arc_profile[i][1]
                ]
        ]
    );

    board_xz_profile_extrude_y(
        profile,
        -2.00 - epsilon,
        board_lower_notch_l + 4.00 + 2 * epsilon
    );
}

module board_reset_slot_cut() {
    translate([
        board_reset_slot_x - board_wall_growth - epsilon,
        board_reset_slot_y,
        board_reset_slot_z
    ])
        board_rounded_prism(
            board_reset_slot_w + board_wall_growth + epsilon,
            board_reset_slot_l,
            board_face_h + board_reset_slot_top_extension
                - board_reset_slot_z + epsilon,
            board_reset_slot_r
        );
}

module board_feature_face() {
    union() {
        difference() {
            linear_extrude(height = board_face_h)
                board_outer_profile_2d();

            board_inside_pocket_cut();
            board_screen_cut();
            board_lower_internal_recess_cuts();
            board_button_internal_clearance_cuts();
            board_far_support_top_cut();
            board_usb_mouth_cut();

            for (x = [board_button_left_x, board_button_right_x]) {
                board_button_well_cut(x);
                board_button_corner_trim_cuts(x);
                board_button_opening_cut(x);
            }
        }

        board_far_support();
    }
}

module board_wall_profile_2d() {
    inner_x = (board_case_w - board_target_internal_w) / 2;
    inner_y = (board_case_l - board_target_internal_l) / 2;

    difference() {
        board_outer_profile_2d();
        translate([inner_x, inner_y])
            square([
                board_target_internal_w,
                board_target_internal_l
            ]);
    }
}

module board_face_slope_transform() {
    multmatrix([
        [board_slope_cos, 0, 0,
         board_slope_x_max * (1 - board_slope_cos)],
        [0, 1, 0, 0],
        [-board_slope_sin, 0, 1,
         board_slope_sin * board_slope_x_max],
        [0, 0, 0, 1]
    ]) children();
}

module board_wall_xy_compress() {
    multmatrix([
        [board_slope_cos, 0, 0,
         board_slope_x_max * (1 - board_slope_cos)],
        [0, 1, 0, 0],
        [0, 0, 1, 0],
        [0, 0, 0, 1]
    ]) children();
}

module board_sloped_wall_volume() {
    board_xz_profile_extrude_y(
        [
            [board_slope_x_min, board_face_h + board_slope_drop],
            [board_slope_x_max, board_face_h - epsilon],
            [board_slope_x_max, board_case_h + epsilon],
            [board_slope_x_min, board_case_h + epsilon]
        ],
        -board_wall_growth - epsilon,
        board_case_l + 2 * board_wall_growth + 2 * epsilon
    );
}

module plain_board_box_walls() {
    intersection() {
        board_wall_xy_compress()
            translate([0, 0, board_face_h - epsilon])
                linear_extrude(height = board_plain_wall_h + epsilon)
                    board_wall_profile_2d();
        board_sloped_wall_volume();
    }
}

module subtle_board_top_edge_cutter_local() {
    curve = top_edge_curve_radius;
    p0 = [0, 0];
    p1 = [0, curve / 5];
    p2 = [0, 2 * curve / 5];
    p3 = [3 * curve / 5, curve];
    p4 = [4 * curve / 5, curve];
    p5 = [curve, curve];

    for (i = [0 : top_edge_curve_segments - 1])
        let(
            t0 = i / top_edge_curve_segments,
            t1 = (i + 1) / top_edge_curve_segments,
            tm = (t0 + t1) / 2,
            q0 = bezier5(p0, p1, p2, p3, p4, p5, t0),
            q1 = bezier5(p0, p1, p2, p3, p4, p5, t1),
            qm = bezier5(p0, p1, p2, p3, p4, p5, tm)
        )
            translate([0, 0, curve - q1[1] - epsilon])
                linear_extrude(height = q1[1] - q0[1] + 2 * epsilon)
                    difference() {
                        board_top_surface_local_2d();
                        offset(delta = -qm[0])
                            board_top_surface_local_2d();
                        // This edge is joined to the microphone enclosure,
                        // so it is no longer an exposed top edge.
                        translate([
                            board_slope_x_max - curve - epsilon,
                            -board_wall_growth - epsilon
                        ])
                            square([
                                curve + 2 * epsilon,
                                board_case_l + 2 * board_wall_growth
                                    + 2 * epsilon
                            ]);
                    }
}

module board_feature_face_rounded() {
    difference() {
        board_feature_face();
        subtle_board_top_edge_cutter_local();
    }
}

module plain_board_case() {
    difference() {
        union() {
            board_face_slope_transform()
                board_feature_face_rounded();
            plain_board_box_walls();
        }
        board_face_slope_transform()
            board_reset_slot_cut();
    }
}

module linear_array_board_case() {
    // Mirror the measured -Z features upward and rotate the enclosure -90
    // degrees around Z.  Its reset-access side faces outward on +Y, its USB
    // mouth faces -X, and its opposite long side overlaps the microphone body.
    translate([
        -board_case_l / 2,
        linear_plate_size[1] / 2 + board_case_w
            - board_case_join_overlap,
        ring_thickness
    ])
        rotate([0, 0, -90])
            mirror([0, 0, 1])
                plain_board_case();
}

module array_board_shared_wall_cut() {
    join_y = linear_plate_size[1] / 2 - board_case_join_overlap;

    // Open the two internal cavities through their overlapping walls while
    // retaining the Z=0..2 top plate and the exterior walls at each end.
    translate([
        -board_target_internal_l / 2,
        join_y - board_wall_growth - epsilon,
        outer_wall_bottom - epsilon
    ])
        cube([
            board_target_internal_l,
            board_case_join_overlap + board_wall_growth + 2 * epsilon,
            -outer_wall_bottom + 2 * epsilon
        ]);
}

module linear_top_surface_2d() {
    difference() {
        rounded_rectangle_2d(linear_plate_size, linear_corner_radius);
        for (x = linear_mount_positions)
            translate([x, 0])
                circle(d = mount_hole_diameter + 2 * lead_in);
    }
}

module board_top_surface_local_2d() {
    difference() {
        board_outer_profile_2d();
        translate([board_screen_face_x, board_screen_face_y])
            square([board_screen_face_w, board_screen_face_l]);
        for (x = [board_button_left_x, board_button_right_x])
            translate([x, board_button_y])
                circle(d = board_button_hole_d);
    }
}

module subtle_top_edge_cutter() {
    curve = top_edge_curve_radius;
    p0 = [0, 0];
    p1 = [0, curve / 5];
    p2 = [0, 2 * curve / 5];
    p3 = [3 * curve / 5, curve];
    p4 = [4 * curve / 5, curve];
    p5 = [curve, curve];

    for (i = [0 : top_edge_curve_segments - 1])
        let(
            t0 = i / top_edge_curve_segments,
            t1 = (i + 1) / top_edge_curve_segments,
            tm = (t0 + t1) / 2,
            q0 = bezier5(p0, p1, p2, p3, p4, p5, t0),
            q1 = bezier5(p0, p1, p2, p3, p4, p5, t1),
            qm = bezier5(p0, p1, p2, p3, p4, p5, tm)
        )
            translate([
                0,
                0,
                ring_thickness - curve + q0[1] - epsilon
            ])
                linear_extrude(height = q1[1] - q0[1] + 2 * epsilon)
                    difference() {
                        linear_top_surface_2d();
                        offset(delta = -qm[0])
                            linear_top_surface_2d();
                        // Do not round the internal seam beneath the attached
                        // board face; that junction must remain continuous.
                        translate([
                            -board_case_l / 2 - board_wall_growth - epsilon,
                            linear_plate_size[1] / 2 - curve - epsilon
                        ])
                            square([
                                board_case_l + 2 * board_wall_growth
                                    + 2 * epsilon,
                                curve + 2 * epsilon
                            ]);
                    }
}

module array_board_overlap_slope_cut() {
    // The board overlaps the array by 2 mm.  Remove the flat array skin above
    // the board's inclined face so the joined top remains a continuous plane.
    near_y = linear_plate_size[1] / 2
        - board_case_join_overlap - board_wall_growth;
    far_y = linear_plate_size[1] / 2 + epsilon;
    far_z = ring_thickness - (far_y - near_y) * board_slope_tan;
    cut_x0 = -board_case_l / 2 - board_wall_growth - epsilon;
    cut_width = board_case_l + 2 * board_wall_growth + 2 * epsilon;

    multmatrix([
        [0, 0, 1, cut_x0],
        [1, 0, 0, 0],
        [0, 1, 0, 0],
        [0, 0, 0, 1]
    ])
        linear_extrude(height = cut_width)
            polygon([
                [near_y, ring_thickness],
                [near_y, ring_thickness + epsilon],
                [far_y, ring_thickness + epsilon],
                [far_y, far_z]
            ]);
}

module linear_array_body_raw() {
    difference() {
        union() {
            linear_plate();
            linear_outer_wall();
            linear_array_board_case();
        }
        array_board_shared_wall_cut();
        array_board_overlap_slope_cut();
    }
}

module linear_array_body() {
    difference() {
        linear_array_body_raw();
        subtle_top_edge_cutter();
    }
}

module reference_mount() {
    // Source axis is +Y. Rotate it so the perforated microphone face is +Z,
    // then place the flange underside on the top surface of the ring.
    translate([-11.5, 11.5, reference_mount_z_offset])
        rotate([90, 0, 0])
            import(mount_stl, convexity = 10);
}

module circular_array() {
    circular_array_body();

    if (show_reference_mounts)
        for (angle = [0 : 360 / mount_count
                      : 360 - 360 / mount_count])
            %rotate([0, 0, angle])
                translate([mount_pitch_radius, 0, 0])
                    reference_mount();
}

module linear_array() {
    linear_array_body();

    if (show_reference_mounts)
        for (x = linear_mount_positions)
            %translate([x, 0, 0]) reference_mount();
}

if (component == "circular_array") {
    circular_array();
} else if (component == "linear_array") {
    linear_array();
} else if (component == "both") {
    gap = 10;
    translate([-(linear_plate_size[0] / 2 + gap / 2), 0, 0])
        circular_array();
    translate([outer_diameter / 2 + gap / 2, 0, 0])
        linear_array();
} else {
    assert(false, str("Unknown component: ", component));
}
