// Wall-mount enclosure for the Sunton ESP32-2432S024C (Cheap Yellow Display 2.4"), two parts.
// Same parameters as make_wallmount.py. Frame: X across the board, Y along it (USB-C at the bottom), Z out of the wall.
// Render with:  part = "shell";  or  part = "bezel";  (F6, export STL)

part = "both";            // "shell" | "bezel" | "both"

board_l = 74.4;  board_w = 42.9;
hole_dx = 68.0;  hole_dy = 36.5;
clear = 0.6;  wall = 2.4;  plate_t = 3.0;
boss_h = 5.0;  boss_d = 6.5;  boss_hole = 2.6;
pcb_t = 1.6;  glass_h = 4.5;  bezel_gap = 0.3;  bezel_t = 2.0;
plug_len = 14.0;  plug_w = 13.0;  plug_dx = -2.5;  rear_hole = 10.0;  corner_r = 3.0;   // USB-C is 2.5 mm left of centre
window_w = 41.0;  window_h = 54.0;  window_dy = 2.0;    // active area sits above the PCB centre
keyhole_y = [18, 58];  keyhole_d = 8.0;  keyhole_slot_w = 4.5;  keyhole_slot_l = 9.0;
head_recess_d = 9.5;  head_recess_t = 2.0;

W = board_w + 2*clear + 2*wall;
L = board_l + 2*clear + plug_len + 2*wall;
inner_h = boss_h + pcb_t + glass_h + bezel_gap;
shell_h = plate_t + inner_h;
cx = W/2;  y0 = wall + clear + plug_len;  cy = y0 + board_l/2;
$fn = 48;


// Boss with a concave fillet at the foot (more material where the tube meets the plate) - rotate_extrude of a 2D profile.
module filleted_boss(d, h, fillet = 1.5) {
    r = d/2;
    rotate_extrude($fn = 48) polygon(concat(
        [[0, 0], [r + fillet, 0]],
        [for (a = [0 : 9 : 90]) [r + fillet - fillet*sin(a), fillet - fillet*cos(a)]],
        [[r, h], [0, h]]));
}
// 90-degree countersunk through hole, cone opening at z = 0 and the hole running towards +z.
module countersunk_hole(d_hole, d_head, length) {
    r = d_hole/2; R = d_head/2;
    rotate_extrude($fn = 48) polygon([[0, -0.5], [R + 0.5, -0.5], [R, 0], [r, R - r], [r, length], [0, length]]);
}

module rrect(w, h, r) { translate([r, r]) offset(r = r) square([w - 2*r, h - 2*r]); }
module holes_at() { for (sx = [-1, 1], sy = [-1, 1]) translate([cx + sx*hole_dy/2, cy + sy*hole_dx/2]) children(); }

module shell() {
    difference() {
        union() {
            difference() {
                linear_extrude(shell_h) rrect(W, L, corner_r);
                translate([wall, wall, plate_t]) linear_extrude(inner_h + 1) rrect(W - 2*wall, L - 2*wall, max(corner_r - wall, 0.5));
            }
            holes_at() translate([0, 0, plate_t - 0.01]) filleted_boss(boss_d, boss_h, 2.0);
        }
        holes_at() translate([0, 0, plate_t - 1]) cylinder(d = boss_hole, h = boss_h + 1.5);
        translate([cx + plug_dx - plug_w/2, -1, plate_t + 2]) cube([plug_w, wall + 2, shell_h]);       // bottom notch
        if (rear_hole > 0) translate([cx + plug_dx, wall + clear + plug_len/2, -1]) cylinder(d = rear_hole, h = plate_t + 2);
        for (ky = keyhole_y) {
            y = L - ky;
            translate([cx, y, -1]) cylinder(d = keyhole_d, h = plate_t + 2);
            translate([cx - keyhole_slot_w/2, y, -1]) cube([keyhole_slot_w, keyhole_slot_l, plate_t + 2]);
            translate([cx - head_recess_d/2, y - keyhole_d/2, plate_t - head_recess_t]) cube([head_recess_d, keyhole_d/2 + keyhole_slot_l + 1, head_recess_t + 1]);
        }
    }
}

module bezel() {
    spacer_h = glass_h + bezel_gap - 0.2;
    difference() {
        union() {
            linear_extrude(bezel_t) rrect(W, L, corner_r);
            holes_at() translate([0, 0, bezel_t - 0.01]) filleted_boss(boss_d, spacer_h, 1.5);
        }
        translate([cx - window_w/2, cy + window_dy - window_h/2, -1]) linear_extrude(bezel_t + spacer_h + 2) rrect(window_w, window_h, 2);
        holes_at() countersunk_hole(3.4, 6.6, bezel_t + spacer_h + 2);        // front face is z = 0
    }
}

if (part == "shell" || part == "both") shell();
if (part == "bezel" || part == "both") translate([W + 10, 0, 0]) bezel();
