// WiFi Source Locator - 3D-printable holder
//
// Holds six PCB Yagi antennas as spokes (standing on edge, 60 degrees apart),
// six XIAO boards, an M5Stack Tab5 tilted towards you, and a GPS antenna,
// all on one pistol-grip handle.
//
// Printed parts:  hub x1, clamp x6, cradle x1, latch x4, handle x1
// Open this file in OpenSCAD, set `part`, press F6, then export as STL.
// Or run hardware/build.sh to export everything.
//
// MEASURE YOUR ANTENNA BOARDS before printing and set the four ant_* values:
// the defaults are estimates from a product photo.

/* [What to make] */
part = "assembly"; // [assembly, exploded, top, hub, clamp, cradle, latch, handle]

/* [Antenna boards: measure yours] */
ant_len     = 105;  // back edge (connector end) to front tip
ant_w_back  = 60;   // width of the wide end, where the connector is
ant_w_front = 36;   // width of the narrow end
ant_t       = 1.6;  // board thickness
sma_clear   = 9;    // keep-out radius around the connector, measured from the board's centre line
n_ant       = 6;    // number of antennas

/* [Tablet] */
tab_w    = 128;  // Tab5 width (landscape)
tab_d    = 80;   // Tab5 height (landscape)
tab_t    = 12;   // Tab5 body thickness, without battery
tab_lift = 16;   // gap behind the tablet for the battery pack and header wires
tab_tilt = 20;   // degrees the screen leans back towards you
fit      = 0.5;  // total play around the tablet
corner   = 16;   // how far the corner brackets run along each edge (shorten to free a port)

/* [GPS shelf] */
gps_shelf = true;
gps_w     = 60;  // shelf size, left-right
gps_d     = 56;  // shelf size, front-back

/* [Layout] */
r_back     = 62;   // distance from the centre to each antenna's back edge
deck_t     = 3.6;  // hub floor thickness
handle_d   = 34;
handle_len = 112;

/* [Hidden] */
$fn = 72;
eps = 0.01;
m3_clear = 3.4;   // M3 screw passes through
m3_tap   = 2.8;   // M3 screw cuts its own thread
nut_af   = 5.9;   // M3 nut across flats, with clearance
nut_h    = 2.7;

// ---- derived dimensions ----
deck_r   = r_back + 18;
z_axis   = deck_t + ant_w_back / 2;          // height of the antenna centre line
clamp_h  = ant_w_back / 2 - sma_clear;       // clamp grips the board below its connector
ant_top  = deck_t + ant_w_back;
grip_in  = 24;    // clamp length behind the board's back edge
grip_out = 18;    // fixed wall length alongside the board
toe      = 16;    // clamp plate overlap on the board
wall_t   = 5;
plate_t  = 4.5;
bolt_y   = [r_back - 8, r_back - 17];
bolt_z   = deck_t + clamp_h * 0.55;

xiao_w = 17.8; xiao_l = 21; xiao_r = 58;      // XIAO size and its distance from the centre
col_od = 40; col_id = 24;                     // centre column: carries the cradle, wires run inside
hb_r   = 27;                                  // handle bolt circle radius
cb     = 11.3;                                // cradle bolt offset (x and y) from the column axis

cr_t   = 5;                                   // cradle frame thickness
hw     = tab_w / 2 + fit / 2;                 // tablet pocket half-sizes
hd     = tab_d / 2 + fit / 2;
post_h = cr_t + tab_lift + tab_t + 0.2;
cr_reach = hd + 9;                            // cradle's furthest point front-back
z_pivot  = ceil(ant_top + 4 + cr_reach * sin(tab_tilt));   // lowest cradle edge still clears the antennas

c_print  = "#E8833A";
c_print2 = "#F2B26B";
c_handle = "#3B4048";

// ---------------------------------------------------------------- helpers

module at_antenna(i) { rotate([0, 0, -i * 360 / n_ant]) children(); }          // +Y = out along antenna i
module at_xiao(i)    { rotate([0, 0, -(i + 0.5) * 360 / n_ant]) children(); }  // between antennas i and i+1
module hexnut(h)     { cylinder(d = nut_af / cos(30), h = h, $fn = 6); }
module slot(l, w, h) { hull() for (s = [-1, 1]) translate([0, s * (l - w) / 2, 0]) cylinder(d = w, h = h, $fn = 24); }

// ---------------------------------------------------------------- hub

// Fixed half of an antenna clamp: a wall beside the board plus a ledge behind
// it. The ledge is the stop the board's back edge sits against, and the heel
// the clamp plate rocks on.
module clamp_wall() {
  xw = -ant_t / 2;
  difference() {
    union() {
      translate([xw - wall_t, r_back - grip_in, 0]) cube([wall_t, grip_in + grip_out, deck_t + clamp_h]);
      translate([xw - eps, r_back - grip_in, 0]) cube([ant_t - 0.3, grip_in, deck_t + clamp_h]);
      hull() {   // buttress
        translate([xw - wall_t - 12, r_back + 5, 0]) cube([12, grip_out - 5, deck_t]);
        translate([xw - wall_t - eps, r_back + 5, 0]) cube([eps, grip_out - 5, deck_t + clamp_h]);
      }
    }
    for (y = bolt_y) translate([0, y, bolt_z]) rotate([0, 90, 0]) {
      cylinder(d = m3_clear, h = 40, center = true, $fn = 24);
      translate([0, 0, xw - wall_t - eps]) rotate([0, 0, 30]) hexnut(nut_h);   // nut pocket on the outside
    }
  }
}

// Loose half of the clamp (print 6).
module clamp_plate() {
  difference() {
    translate([ant_t / 2, r_back - grip_in, deck_t + 0.6]) cube([plate_t, grip_in + toe, clamp_h - 0.6]);
    for (y = bolt_y) translate([0, y, bolt_z]) rotate([0, 90, 0]) cylinder(d = m3_clear, h = 40, center = true, $fn = 24);
  }
}

module xiao_bay() {   // raised pad and side rails; +Y = USB end, pointing outwards
  translate([0, xiao_r, deck_t - eps]) {
    translate([-3.5, -xiao_l / 2, 0]) cube([7, xiao_l, 1.2]);
    for (s = [-1, 1]) translate([s * (xiao_w / 2 + 0.25) - (s < 0 ? 1.6 : 0), -xiao_l / 2 - 1.6, 0]) cube([1.6, xiao_l + 1.6, 4.4]);
    translate([-xiao_w / 2 - 0.25, -xiao_l / 2 - 1.6, 0]) cube([xiao_w + 0.5, 1.6, 4.4]);
  }
}

module xiao_bay_cuts() {   // slots for header pins / wires, and for a cable tie over the board
  translate([0, xiao_r, -1]) {
    for (s = [-1, 1]) translate([s * 7.62, 0, 0]) slot(xiao_l - 2, 2.8, deck_t + 2);
    for (s = [-1, 1]) translate([s * (xiao_w / 2 + 3.6), 0, 0]) slot(6, 2.2, deck_t + 2);
  }
}

module column() {
  difference() {
    cylinder(d = col_od, h = z_pivot + 30);
    translate([0, 0, -1]) cylinder(d = col_id, h = z_pivot + 40);
    // top cut at the tablet's tilt
    translate([0, 0, z_pivot]) rotate([tab_tilt, 0, 0]) translate([-50, -50, 0]) cube([100, 100, 60]);
    // cradle screws and their slide-in nuts
    translate([0, 0, z_pivot]) rotate([tab_tilt, 0, 0]) for (sx = [-1, 1], sy = [-1, 1]) {
      translate([sx * cb, sy * cb, -16]) cylinder(d = m3_clear, h = 17, $fn = 24);
      translate([sx * cb, sy * cb, -6.5]) rotate([0, 0, atan2(sy, sx)]) hull() {
        rotate([0, 0, 30]) hexnut(nut_h + 0.2);
        translate([12, -nut_af / 2, 0]) cube([1, nut_af, nut_h + 0.2]);
      }
    }
    // wire windows at the base, pointed tops so they print without support
    for (k = [0 : 2]) rotate([0, 0, -(k * 120 + 180 / n_ant)]) translate([0, col_od / 2, deck_t]) hull() {
      translate([-6.5, -11, 0]) cube([13, 22, 10]);
      translate([-0.5, -11, 0]) cube([1, 22, 16.5]);
    }
  }
}

module hub() {
  difference() {
    intersection() {
      union() {
        cylinder(r = deck_r, h = deck_t);
        for (i = [0 : n_ant - 1]) {
          at_antenna(i) clamp_wall();
          at_xiao(i) xiao_bay();
        }
        column();
      }
      cylinder(r = deck_r, h = 400);
    }
    for (i = [0 : n_ant - 1]) at_xiao(i) xiao_bay_cuts();
    for (k = [0 : 3]) rotate([0, 0, 45 + k * 90]) translate([hb_r, 0, -1]) cylinder(d = m3_clear, h = deck_t + 2, $fn = 24);
    // antenna numbers, sunk into the top of each clamp wall
    for (i = [0 : n_ant - 1]) at_antenna(i)
      translate([-ant_t / 2 - wall_t / 2, r_back + grip_out / 2 - 2, deck_t + clamp_h - 0.6])
        rotate([0, 0, 90]) linear_extrude(1) text(str(i), size = 4, halign = "center", valign = "center");
  }
}

// ---------------------------------------------------------------- cradle (tablet tray)

module corner_post(sx, sy) {
  // L-shaped walls around the tablet corner
  translate([sx > 0 ? hw - corner : -hw - 3, sy > 0 ? hd : -hd - 3, 0]) cube([corner + 3, 3, post_h]);
  translate([sx > 0 ? hw : -hw - 3, sy > 0 ? hd - corner : -hd - 3, 0]) cube([3, corner + 3, post_h]);
  // the tablet's corner rests here
  translate([sx > 0 ? hw - 10 : -hw, sy > 0 ? hd - 10 : -hd, 0]) cube([10, 10, cr_t + tab_lift]);
  // block that takes the latch screw
  difference() {
    translate([sx > 0 ? hw : -hw - 9, sy > 0 ? hd : -hd - 9, 0]) cube([9, 9, post_h]);
    translate([sx * (hw + 4.5), sy * (hd + 4.5), post_h - 14]) cylinder(d = m3_tap, h = 15, $fn = 24);
  }
}

module cradle() {
  difference() {
    union() {
      difference() {   // outer frame
        translate([-hw - 3, -hd - 3, 0]) cube([2 * hw + 6, 2 * hd + 6, cr_t]);
        translate([-hw + 5, -hd + 5, -1]) cube([2 * hw - 10, 2 * hd - 10, cr_t + 2]);
      }
      cylinder(d = 56, h = cr_t);   // pad that bolts to the column
      for (sx = [-1, 1], sy = [-1, 1]) hull() {   // arms to the corners
        cylinder(d = 12, h = cr_t);
        translate([sx * (hw - 4), sy * (hd - 4), 0]) cylinder(d = 12, h = cr_t);
      }
      for (sx = [-1, 1], sy = [-1, 1]) corner_post(sx, sy);
      if (gps_shelf) translate([-hw - 3 - gps_w, -gps_d / 2, 0]) difference() {
        cube([gps_w + eps, gps_d, cr_t + 4]);
        translate([2, 2, cr_t]) cube([gps_w, gps_d - 4, 10]);               // low rim on three sides
        for (x = [12, gps_w - 14], y = [10, gps_d - 10]) translate([x, y, -1]) rotate([0, 0, 90]) slot(14, 3, cr_t + 2);
      }
    }
    translate([0, 0, -1]) cylinder(d = 20, h = cr_t + 2);   // wires come up through the column
    for (sx = [-1, 1], sy = [-1, 1]) translate([sx * cb, sy * cb, 0]) {
      translate([0, 0, -1]) cylinder(d = m3_clear, h = cr_t + 2, $fn = 24);
      translate([0, 0, cr_t - 1.8]) cylinder(d1 = m3_clear, d2 = 6.8, h = 1.8 + eps, $fn = 32);   // countersink
    }
  }
}

// Turn-button that swings over the tablet's corner (print 4).
module latch() {
  difference() {
    hull() { cylinder(d = 9, h = 3); translate([11, 0, 0]) cylinder(d = 9, h = 3); }
    translate([0, 0, -1]) cylinder(d = m3_clear, h = 5, $fn = 24);
  }
}

// ---------------------------------------------------------------- handle

// Drawn hanging down from z = 0 (the underside of the hub).
module handle() {
  flange_t = 7;
  difference() {
    union() {
      translate([0, 0, -flange_t]) cylinder(d = 2 * hb_r + 18, h = flange_t);
      translate([0, 0, -flange_t - 6]) cylinder(d1 = handle_d, d2 = 44, h = 6 + eps);
      translate([0, 0, -handle_len + 4]) cylinder(d = handle_d, h = handle_len - flange_t - 6 - 4 + eps);
      translate([0, 0, -handle_len]) cylinder(d1 = handle_d - 8, d2 = handle_d, h = 4 + eps);
    }
    for (k = [0 : 3]) rotate([0, 0, 45 + k * 90]) translate([hb_r, 0, 0]) {
      translate([0, 0, -flange_t - 1]) cylinder(d = m3_clear, h = flange_t + 2, $fn = 24);
      translate([0, 0, -flange_t - 8]) rotate([0, 0, 30]) hexnut(8 + nut_h);   // nut pocket in the underside of the flange
    }
    for (z = [-40, -58, -76, -94]) translate([0, 0, z]) rotate_extrude() translate([handle_d / 2 + 1.2, 0]) circle(r = 2.4, $fn = 24);
    translate([0, 0, -handle_len + 12]) rotate([90, 0, 0]) cylinder(d = 6, h = 60, center = true, $fn = 32);   // lanyard hole
  }
}

// ---------------------------------------------------------------- stand-ins for the bought parts (pictures only)

module ant_board() {   // standing on edge, back edge at y = 0, centre line at z = 0
  color("#1E7D45") rotate([90, 0, 90]) linear_extrude(ant_t, center = true)
    polygon([[0, -ant_w_back / 2], [0, ant_w_back / 2], [ant_len, ant_w_front / 2], [ant_len, -ant_w_front / 2]]);
  for (e = [[6, 0.86, 6], [19, 0.74, 4], [0.36 * ant_len, 0.62, 6], [0.52 * ant_len, 0.56, 6], [0.68 * ant_len, 0.50, 6], [0.84 * ant_len, 0.44, 6]])
    color("#D8DCE0") translate([0, e[0], 0]) cube([ant_t + 0.3, e[2], e[1] * ant_w_back], center = true);
  color("#D4A537") translate([0, -5, 0]) rotate([90, 0, 0]) cylinder(d = 6.2, h = 10, center = true, $fn = 24);
  color("#D4A537") translate([0, 1.5, 0]) cube([4.5, 5, 6.4], center = true);
  color("#B9BDC2") translate([0, -16, 0]) rotate([90, 0, 0]) cylinder(d = 8, h = 12, center = true, $fn = 6);   // pigtail plug
}

module xiao_mock() {   // sitting in a bay
  translate([0, xiao_r, deck_t + 1.2]) {
    color("#20242A") translate([-xiao_w / 2, -xiao_l / 2, 0]) cube([xiao_w, xiao_l, 1.2]);
    color("#C9CDD2") translate([-5.5, -xiao_l / 2 + 1.5, 1.2]) cube([11, 10.5, 2]);
    color("#C9CDD2") translate([-4.5, xiao_l / 2 - 6, 1.2]) cube([9, 7.4, 3.2]);
  }
}

module cable(pts, d = 1.6) {
  color("#15171A") for (i = [0 : len(pts) - 2]) hull() {
    translate(pts[i]) sphere(d = d, $fn = 10);
    translate(pts[i + 1]) sphere(d = d, $fn = 10);
  }
}

module tab5_mock() {
  translate([0, 0, cr_t + tab_lift]) {
    color("#2A2D33") hull() for (sx = [-1, 1], sy = [-1, 1]) translate([sx * (tab_w / 2 - 6), sy * (tab_d / 2 - 6), 0]) cylinder(r = 6, h = tab_t);
    translate([0, 0, tab_t]) {
      color("#0E1116") translate([-55.3, -31.1, 0]) cube([110.6, 62.2, 0.3]);
      // a hint of the Hunt screen
      color("#1A202A") translate([-27, 0, 0.3]) cylinder(r = 26, h = 0.2);
      color("#35C2FF") translate([-27, 0, 0.5]) rotate([0, 0, -35]) hull() { translate([0, 20, 0]) cylinder(r = 0.6, h = 0.2); translate([-7, 6, 0]) cylinder(r = 0.6, h = 0.2); translate([7, 6, 0]) cylinder(r = 0.6, h = 0.2); }
      color("#35C2FF") translate([-27, 0, 0.5]) rotate([0, 0, -35]) translate([-2, -8, 0]) cube([4, 16, 0.2]);
      color("#FFD23C") translate([8, 14, 0.3]) cube([20, 9, 0.2]);
      color("#232B38") translate([8, 6, 0.3]) cube([44, 4, 0.2]);
      color("#FFD23C") translate([8, 6, 0.4]) cube([29, 4, 0.2]);
      color("#1A202A") translate([8, -12, 0.3]) cube([44, 14, 0.2]);
      for (k = [0 : 2]) color("#232B38") translate([8 + k * 15, -27, 0.3]) cube([13.5, 8, 0.2]);
      color("#35C2FF") translate([-52, 25, 0.3]) cube([16, 5, 0.2]);
    }
  }
}

module gps_mock() {
  translate([-hw - 3 - gps_w / 2, 0, cr_t]) {
    color("#F4F4F2") cylinder(d = 44, h = 12);
    color("#2A2D33") cylinder(d = 46, h = 2);
  }
}

// ---------------------------------------------------------------- views

module placed_cradle() { translate([0, 0, z_pivot]) rotate([tab_tilt, 0, 0]) children(); }

module assembly(x = 0) {   // x = how far to pull the parts apart
  color(c_print) render(convexity = 8) hub();
  for (i = [0 : n_ant - 1]) {
    at_antenna(i) {
      translate([x * 0.5, 0, 0]) color(c_print2) render() clamp_plate();
      translate([0, r_back + x * 1.2, z_axis]) ant_board();
    }
    at_xiao(i) translate([0, 0, x * 0.5]) xiao_mock();
    if (x == 0) cable([   // pigtail from each XIAO to its antenna
      [xiao_r * sin((i + 0.5) * 360 / n_ant), xiao_r * cos((i + 0.5) * 360 / n_ant), deck_t + 4] * 0.84,
      [(xiao_r - 22) * sin((i + 0.35) * 360 / n_ant), (xiao_r - 22) * cos((i + 0.35) * 360 / n_ant), deck_t + 16],
      [(r_back - 32) * sin((i + 0.12) * 360 / n_ant), (r_back - 32) * cos((i + 0.12) * 360 / n_ant), z_axis - 4],
      [(r_back - 22) * sin(i * 360 / n_ant), (r_back - 22) * cos(i * 360 / n_ant), z_axis]]);
  }
  translate([0, 0, -x * 0.8]) color(c_handle) render(convexity = 6) handle();
  translate([0, 0, x * 0.9]) placed_cradle() {
    color(c_print) render(convexity = 6) cradle();
    translate([0, 0, x * 0.8]) {
      tab5_mock();
      if (gps_shelf) gps_mock();
      for (sx = [-1, 1], sy = [-1, 1]) translate([sx * (hw + 4.5), sy * (hd + 4.5), post_h + x * 0.4])
        rotate([0, 0, atan2(-sy, -sx)]) color(c_print2) latch();
    }
  }
}

module labels() {   // for the top-view picture
  color("#20242A") {
    for (i = [0 : n_ant - 1]) at_antenna(i) translate([14, r_back + ant_len - 14, ant_top + 1]) rotate([0, 0, i * 360 / n_ant])
      linear_extrude(0.5) text(str(i), size = 13, halign = "center", valign = "center", font = "DejaVu Sans:style=Bold");
    translate([0, r_back + ant_len + 22, 0]) linear_extrude(0.5) text("FRONT", size = 12, halign = "center", font = "DejaVu Sans:style=Bold");
    translate([0, r_back + ant_len + 6, 0]) linear_extrude(0.5) polygon([[-9, 0], [9, 0], [0, 12]]);
    translate([0, -r_back - ant_len - 30, 0]) linear_extrude(0.5) text("you stand here", size = 11, halign = "center", font = "DejaVu Sans");
  }
}

if (part == "assembly") assembly();
else if (part == "exploded") assembly(60);
else if (part == "top") { assembly(); labels(); }
else if (part == "hub") hub();
else if (part == "clamp") translate([0, 0, ant_t / 2 + plate_t]) rotate([0, 90, 0]) translate([0, -r_back, 0]) clamp_plate();   // flat on its face
else if (part == "cradle") cradle();
else if (part == "latch") latch();
else if (part == "handle") translate([0, 0, 0]) rotate([180, 0, 0]) handle();   // flange on the bed
