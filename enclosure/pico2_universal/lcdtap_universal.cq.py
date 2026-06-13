import cadquery as cq

# 壁の厚み
wall_t = 2

# 基板サイズ
board_radius = 5
board_w = 50
board_h = 70
board_t = 1.6
board_top_z = wall_t + 4

# ケースサイズ
case_expand = wall_t + 0.5
case_radius = case_radius = board_radius + case_expand
case_w = board_w + case_expand * 2
case_h = board_h + case_expand * 2
case_t = board_top_z + 7 + wall_t
case_margin = 0.1
case_notch_t = 2
case_back_t = board_top_z + 2.5
case_front_t = case_t - case_back_t

# USB コネクタの位置
usb_hole_w = 9 + 1
usb_hole_h = 3.3 + 1
usb_x = -13
usb_z = board_top_z + 3.3 / 2

# ビデオコネクタの位置
video_hole_w = 15 + 1
video_hole_h = 5.6 + 1
video_x = 10
video_z = board_top_z + 3.4

# ピンヘッダの位置
header_hole_w = 2.54 * 8 + 2
header_hole_t = 2.54 * 2 + 2
header_x = 0
header_z = board_top_z + 2.54

# 十字キーの中心位置
dpad_x = 0
dpad_y = -board_h / 2 + 20
dpad_rx = 7
dpad_ry = 6
sw_top_z = board_top_z + 2.4
sw_w = 5
sw_h = 4

# リセットスイッチの位置
reset_x = board_w / 2 - 10
reset_y = -board_h / 2 + 30

# 上面スイッチの位置
sw_poses = [
    (dpad_x, dpad_y, 180),
    (dpad_x - dpad_rx, dpad_y, 0),
    (dpad_x + dpad_rx, dpad_y, 0),
    (dpad_x, dpad_y - dpad_ry, 0),
    (dpad_x, dpad_y + dpad_ry, 0),
    # (reset_x, reset_y, 0),
    # (-reset_x, reset_y, 0),
]

# サイドスイッチの位置
side_hole_h = 8
side_hole_t = 4
side_y = -board_h / 2 + 20
side_z = board_top_z + 0.75

# LEDの位置
led_x = 0
led_y = board_h / 2 - 13
led_top_z = board_top_z + 1

# ネジの位置
screw_poses = [
    (board_w / 2 - 5, board_h / 2 - 15),
    (-board_w / 2 + 5, board_h / 2 - 15),
    (board_w / 2 - 5, -board_h / 2 + 5),
    (-board_w / 2 + 5, -board_h / 2 + 5),
]


def rounded_box(w, h, t, radius):
    return (
        cq.Workplane("XY")
        .box(w, h, t, centered=(True, True, False))
        .edges("|Z")
        .fillet(radius)
    )


# バックパネル
notch_cut_size = wall_t / 2 + case_margin
back_panel = (
    rounded_box(case_w, case_h, case_back_t - case_notch_t - case_margin, case_radius)
    .union(
        rounded_box(
            case_w - notch_cut_size * 2,
            case_h - notch_cut_size * 2,
            case_back_t - case_margin,
            case_radius - notch_cut_size,
        )
    )
    .cut(
        rounded_box(
            case_w - wall_t * 2, case_h - wall_t * 2, 999, case_radius - wall_t
        ).translate((0, 0, wall_t))
    )
    .edges("<Z")
    .chamfer(1)
)

# フロントパネル
notch_cut_size = wall_t / 2 - case_margin
front_panel = (
    rounded_box(case_w, case_h, case_front_t + case_notch_t - case_margin, case_radius)
    .cut(
        rounded_box(
            case_w - notch_cut_size * 2,
            case_h - notch_cut_size * 2,
            case_notch_t + case_margin,
            case_radius - notch_cut_size,
        )
    )
    .cut(
        rounded_box(
            case_w - wall_t * 2,
            case_h - wall_t * 2,
            case_front_t + case_notch_t - wall_t,
            case_radius - wall_t,
        )
    )
    .translate((0, 0, case_back_t - case_notch_t + case_margin))
)

if True:
    # LEDの穴
    hole_w = 20
    hole_h = 2
    front_panel = front_panel.union(
        rounded_box(
            hole_w + 2, hole_h + 2, case_t - led_top_z - 1, (hole_h + 2) / 2 - 0.1
        ).translate((led_x, led_y, case_t - led_top_z))
    )
    front_panel = front_panel.cut(
        rounded_box(hole_w, hole_h, 999, hole_h / 2 - 0.1).translate((led_x, led_y, 0))
    )

front_panel = front_panel.edges(">Z").chamfer(1 - 0.1)

# USBコネクタの穴
cutter = (
    rounded_box(usb_hole_w, usb_hole_h, 999, usb_hole_h / 2 - 0.1)
    .rotate((1, 0, 0), (0, 0, 0), 90)
    .translate((usb_x, board_h / 2, usb_z))
)
back_panel = back_panel.cut(cutter)
front_panel = front_panel.cut(cutter)

# ビデオコネクタの穴
chamfer = 2.5
verts = [
    (-video_hole_w / 2, video_hole_h / 2),
    (video_hole_w / 2, video_hole_h / 2),
    (video_hole_w / 2, -video_hole_h / 2 + chamfer),
    (video_hole_w / 2 - chamfer, -video_hole_h / 2),
    (-video_hole_w / 2 + chamfer, -video_hole_h / 2),
    (-video_hole_w / 2, -video_hole_h / 2 + chamfer),
]
cutter = (
    cq.Workplane("XY")
    .polyline(verts)
    .close()
    .extrude(10)
    .rotate((1, 0, 0), (0, 0, 0), -90)
    .translate((video_x, case_h / 2 + 5, video_z))
    .edges("|Y")
    .fillet(1)
)
back_panel = back_panel.cut(cutter)
front_panel = front_panel.cut(cutter)

# ピンヘッダの穴
cutter = (
    cq.Workplane("XY")
    .box(header_hole_w, 20, header_hole_t, centered=(True, True, True))
    .translate((header_x, -case_h / 2, header_z))
    .edges("|Y")
    .fillet(1)
)
back_panel = back_panel.cut(cutter)
front_panel = front_panel.cut(cutter)

# 上面スイッチ
sw_notch_t = 1
sw_notch = (
    cq.Workplane("XY")
    .box(2.5, 1.75, sw_notch_t, centered=(True, False, False))
    .translate((sw_w / 2, -sw_h / 2, 0))
)
sw = (
    rounded_box(sw_w, sw_h, case_t - sw_top_z + 0.5, 1)
    .edges(">Z")
    .chamfer(0.3)
    .union(sw_notch)
    .union(sw_notch.mirror("YZ"))
)

# 上面スイッチの穴
guide = rounded_box(sw_w + 4, sw_h + 4, case_t - sw_top_z - 2, 2).translate(
    (0, 0, sw_top_z + sw_notch_t + 0.5)
)
cutter = rounded_box(sw_w + 0.5, sw_h + 0.5, 999, 1)
for sw_x, sw_y, _ in sw_poses:
    front_panel = front_panel.union(guide.translate((sw_x, sw_y, 0)))
for sw_x, sw_y, _ in sw_poses:
    front_panel = front_panel.cut(cutter.translate((sw_x, sw_y, 0)))

# 上面スイッチの矢印
verts = [
    (0, 2),
    (-2, -2),
    (2, -2),
]
cutter = (
    cq.Workplane("XY").polyline(verts).close().extrude(10).translate((0, 0, case_t - wall_t + 0.5))
)
front_panel = front_panel.cut(cutter.translate((dpad_x, dpad_y + dpad_ry + 7, 0)))
front_panel = front_panel.cut(
    cutter.rotate((0, 0, 1), (0, 0, 0), 180).translate(
        (dpad_x, dpad_y - dpad_ry - 7, 0)
    )
)
front_panel = front_panel.cut(
    cutter.rotate((0, 0, 1), (0, 0, 0), 90).translate((dpad_x + dpad_rx + 7, dpad_y, 0))
)
front_panel = front_panel.cut(
    cutter.rotate((0, 0, 1), (0, 0, 0), 270).translate(
        (dpad_x - dpad_rx - 7, dpad_y, 0)
    )
)

if True:
    # リセット/ブートスイッチの穴
    guide = (
        cq.Workplane("XY")
        .cylinder(case_t - wall_t - sw_top_z, 6 / 2, centered=(True, True, False))
        .translate((0, 0, sw_top_z + 1.5))
    )
    front_panel = front_panel.union(guide.translate((reset_x, reset_y, 0)))
    front_panel = front_panel.union(guide.translate((-reset_x, reset_y, 0)))
    front_panel = (
        front_panel.faces(">Z")
        .workplane()
        .pushPoints([(reset_x, reset_y), (-reset_x, reset_y)])
        .hole(3.5)
    )

if False:
    # 上面スイッチの文字
    front_panel = front_panel.cut(
        cq.Workplane("XY")
        .text("BOOT", 3, 1, font="Arial", kind="bold", halign="center", valign="center")
        .translate((-reset_x, reset_y + 5, case_t - 1))
    )
    front_panel = front_panel.cut(
        cq.Workplane("XY")
        .text(
            "RESET", 3, 1, font="Arial", kind="bold", halign="center", valign="center"
        )
        .translate((reset_x, reset_y + 5, case_t - 1))
    )

# サイドスイッチの穴
verts = [
    (0, -side_hole_t / 2),
    (0, side_hole_t / 2),
    (-20, side_hole_t / 2 + 20),
    (-20, -side_hole_t / 2 - 20),
]
cutter = (
    cq.Workplane("XZ")
    .polyline(verts)
    .close()
    .extrude(side_hole_h / 2, both=True)
    .translate((-case_w / 2 + wall_t, side_y, side_z))
)
back_panel = back_panel.cut(cutter)
front_panel = front_panel.cut(cutter)

# ケース前面のネジ穴
guide = (
    cq.Workplane("XY")
    .cylinder(case_t - wall_t - board_top_z, 6 / 2, centered=(True, True, False))
    .translate((0, 0, board_top_z + 0.1))
    .faces("<Z")
    .workplane()
    .hole(2.5)
)
for screw_x, screw_y in screw_poses:
    front_panel = front_panel.union(guide.translate((screw_x, screw_y, 0)))
    support_t = case_front_t - case_notch_t
    support = (
        cq.Workplane("XY")
        .box(4, 3, support_t, centered=(False, True, False))
        .translate((0, 0, case_t - wall_t - support_t + 0.5))
    )
    if screw_x > 0:
        front_panel = front_panel.union(support.translate((screw_x + 2, screw_y, 0)))
    else:
        front_panel = front_panel.union(
            support.rotate((0, 0, 1), (0, 0, 0), 180).translate(
                (screw_x - 2, screw_y, 0)
            )
        )
    if screw_y < 0:
        front_panel = front_panel.union(
            support.rotate((0, 0, 1), (0, 0, 0), 90).translate(
                (screw_x, screw_y - 2, 0)
            )
        )

# ケース背面のネジ穴
screw_head_depth = 1
verts = [
    (0, 0),
    (7 / 2 + 0.5, 0),
    (7 / 2, 0.5),
    (7 / 2, screw_head_depth),
    (3.5 / 2, screw_head_depth + 3.5 / 2),
    (3.5 / 2, 999),
    (0, 999),
]
screw_hole = (
    cq.Workplane("XZ").polyline(verts).close().revolve(360, (0, 0, 0), (0, 1, 0))
)
for screw_x, screw_y in screw_poses:
    support_t = board_top_z - board_t - 0.1
    back_panel = back_panel.union(
        cq.Workplane("XY")
        .cylinder(support_t, 8 / 2, centered=(True, True, False))
        .translate((screw_x, screw_y, 0))
    )
    support = cq.Workplane("XY").box(4, 3, support_t, centered=(False, True, False))
    if screw_x > 0:
        back_panel = back_panel.union(support.translate((screw_x + 2, screw_y, 0)))
    else:
        back_panel = back_panel.union(
            support.rotate((0, 0, 1), (0, 0, 0), 180).translate(
                (screw_x - 2, screw_y, 0)
            )
        )
    if screw_y < 0:
        back_panel = back_panel.union(
            support.rotate((0, 0, 1), (0, 0, 0), 90).translate(
                (screw_x, screw_y - 2, 0)
            )
        )
    back_panel = back_panel.cut(screw_hole.translate((screw_x, screw_y, 0)))

if False:
    # ロゴ
    logo_dot_poses = [
        "##         ###### ######  ",
        "##        ####### ####### ",
        "##       ###      ##   ###",
        "##       ##       ##    ##",
        "##       ##       ##    ##",
        "##       ###      ##   ###",
        "########  ####### ####### ",
        " #######   ###### ######  ",
        "                          ",
        "########   ####   ####### ",
        "########  ######  ########",
        "   ##    ###  ### ##    ##",
        "   ##    ##    ## ##    ##",
        "   ##    ######## ########",
        "   ##    ######## ####### ",
        "   ##    ##    ## ##      ",
        "   ##    ##    ## ##      ",
    ]
    hole = cq.Workplane("XY").box(0.75, 0.75, wall_t, centered=(True, True, False))
    holes = []
    step = 1.25
    for y, line in enumerate(logo_dot_poses):
        for x, c in enumerate(line):
            if c != " ":
                holes.append(hole.translate((x * step, -y * step, 0)))

    def union_holes(holes):
        if len(holes) == 1:
            return holes[0]
        mid = len(holes) // 2
        left = union_holes(holes[:mid])
        right = union_holes(holes[mid:])
        return left.union(right)

    cutter = union_holes(holes).translate(
        (-12.5 * step, 17 + 8 * step, case_t - wall_t - 0.5)
    )
    front_panel = front_panel.cut(cutter)

if True:
    show_object(back_panel, options={"color": (0.5, 0.5, 0.5), "alpha": 0.5})
    show_object(front_panel, options={"color": (0.5, 0.5, 0.5), "alpha": 0.5})
    for sw_x, sw_y, a in sw_poses:
        show_object(
            sw.rotate((0, 0, 1), (0, 0, 0), a).translate((sw_x, sw_y, sw_top_z)),
            options={"color": (0, 0, 0)},
        )

back_panel = back_panel.translate((case_w / 2 + 1, 0, 0))
front_panel = front_panel.rotate(
    (-case_w / 2, 0, case_t / 2), (-case_w / 2, 1, case_t / 2), 180
).translate((case_w / 2 - 1, 0, 0))


sw_rot = sw.rotate((0, -sw_h / 2, 0), (1, -sw_h / 2, 0), 90)
sw_set = sw_rot
for i in range(1, len(sw_poses)):
    sw_set = sw_set.union(
        sw_rot.translate((i * (sw_w + 3),0,0))
    )
    
if False:
    show_object(back_panel, options={"color": (0.5, 0.5, 0.5), "alpha": 0.5})
    show_object(front_panel, options={"color": (0.5, 0.5, 0.5), "alpha": 0.5})
    show_object(sw_set.translate((0, -case_h / 2, 0)), options={"color": (0, 0, 0)})

front_panel.export("front_panel.stl")
front_panel.export("front_panel.step")
back_panel.export("back_panel.stl")
back_panel.export("back_panel.step")
sw_rot.export("switch.stl")
sw_rot.export("switch.step")
sw_set.export("switch_set.stl")
sw_set.export("switch_set.step")
