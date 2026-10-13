// Desk stand for the Sunton ESP32-2432S024C (Cheap Yellow Display 2.4")
// Board: 74.4 x 42.9 mm PCB, 4 mounting holes Ø3.2 on a 68.0 x 36.5 mm grid (3.2 mm from the edges),
// USB-C on one short edge. Mounted in portrait with the USB-C at the BOTTOM (set "Vending: USB-stik nedad"
// on the remote's web page). Print with the FRONT (inclined) face on the bed: no supports needed.
//
// All dimensions in mm. Edit the parameters, then render with OpenSCAD (F6) and export STL.

tilt        = 65;     // angle of the display face from horizontal
plate_t     = 4;      // front plate thickness
base_t      = 4;      // base plate thickness
wall_t      = 3;      // side wall thickness
width       = 54;     // outer width (board is 42.9)
face_h      = 95;     // length of the inclined face
base_d      = 60;     // total depth of the base (footprint)
boss_d      = 6.5;    // standoff boss diameter
boss_h      = 5;      // standoff height above the face (clears the ESP32 module and connectors)
boss_hole   = 2.6;    // M3 self-tapping; drill to 3.2 for through-bolts
hole_dx     = 68.0;   // board hole grid (along the face)
hole_dy     = 36.5;   // board hole grid (across the face)
board_h     = 74.4;   // board length along the face
board_w     = 42.9;
cable_w     = 13;     // USB-C plug body clearance
cable_h     = 9;
ledge_h     = 3;      // rest for the bottom edge of the board (either side of the cable)
board_lift  = 16;     // face bottom edge -> board bottom edge: room for the USB-C plug body to pass through the slot

r = 1.6;              // small fillet radius for the outer edges


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

module rounded_box(size, rr) {
    hull() for (x = [rr, size[0]-rr], y = [rr, size[1]-rr], z = [rr, size[2]-rr]) translate([x,y,z]) sphere(rr, $fn=24);
}

// The inclined front plate with bosses, defined in its own frame: X across, Y up the face, Z out of the face.
module face_plate() {
    difference() {
        union() {
            rounded_box([width, face_h, plate_t], r);
            // standoff bosses
            for (sx = [-1, 1], sy = [-1, 1])
                translate([width/2 + sx*hole_dy/2, board_lift + board_h/2 + sy*hole_dx/2, plate_t - 0.01])
                    filleted_boss(boss_d, boss_h, 1.5);
            // ledges under the board's bottom edge, left and right of the cable slot
            translate([(width - board_w)/2, board_lift - ledge_h, plate_t - 0.01])
                cube([(board_w - cable_w)/2, ledge_h, boss_h]);
            translate([width/2 + cable_w/2, board_lift - ledge_h, plate_t - 0.01])
                cube([(board_w - cable_w)/2, ledge_h, boss_h]);
        }
        // boss holes
        for (sx = [-1, 1], sy = [-1, 1])
            translate([width/2 + sx*hole_dy/2, board_lift + board_h/2 + sy*hole_dx/2, plate_t - boss_h])
                cylinder(d = boss_hole, h = 3*boss_h, $fn = 24);
        // cable slot through the bottom of the face
        translate([width/2 - cable_w/2, -1, -1]) cube([cable_w, board_lift + 1, plate_t + boss_h + 2]);
    }
}

module stand() {
    // face plate: its back (z=0 side) hinges at the origin and leans back by `tilt` from horizontal.
    // The bosses (+z side) end up facing the front (-y).
    rotate([tilt, 0, 0]) face_plate();
    // base plate, starting just under the plate's front edge. The USB cable runs inside the open shell
    // and out of the back, so no channel is needed.
    front = plate_t * sin(tilt);
    translate([0, -front, 0]) rounded_box([width, base_d + front, base_t], r);
    // triangular side walls following the back of the face plate
    face_depth = face_h * cos(tilt);
    face_top   = face_h * sin(tilt);
    for (x = [0, width - wall_t])
        translate([x, 0, 0])
            rotate([90, 0, 90])
                linear_extrude(wall_t)
                    // bites 1 mm into the plate so the union is a single solid
                    polygon([[-sin(tilt), cos(tilt) - 0.5], [face_depth - sin(tilt), face_top + cos(tilt)], [base_d, 0], [0, -0.5]]);
}

stand();
