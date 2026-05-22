"""
gen_fonts.py  —  generate missing 16×24 font glyphs for font_data.h

Format: 16 cols × 24 rows = 384 uint16_t values, PROGMEM.
  0xFFFF = ink pixel, 0x0000 = transparent.
  Strokes are 3 pixels wide.
  Rows 0-5 are always blank padding.
  Uppercase letters fill roughly rows 6-23, cols 1-14.
  Tall lowercase (d, h) start at row 6 (ascender).
  Short lowercase (x) start at row 12 (x-height only).
"""

W = 16  # columns
H = 24  # rows
INK = 0xFFFF
BLK = 0x0000

# Helper to turn a 16x24 list-of-strings grid into flat uint16_t array.
# Each string is exactly 16 chars: '1' = ink, '0' = blank.
def grid_to_array(rows):
    assert len(rows) == H, f"Need {H} rows, got {len(rows)}"
    result = []
    for r in rows:
        assert len(r) == W, f"Row '{r}' has len {len(r)}, need {W}"
        for c in r:
            result.append(INK if c == '1' else BLK)
    assert len(result) == W * H == 384
    return result

def fmt_array(arr):
    """Return the C array body as a single long line, matching font_data.h style."""
    return ", ".join(f"0x{v:04X}" for v in arr)

def emit_char(char_name, var_name, grid):
    arr = grid_to_array(grid)
    body = fmt_array(arr)
    print(f"// '{char_name}' 16x24")
    print(f"const uint16_t {var_name}[] PROGMEM = {{")
    print(body)
    print("};")
    print()

# ─────────────────────────────────────────────────────────────────────────────
# 'M'  (uppercase, tall, rows 6-23, wide V strokes inside two vertical pillars)
# ─────────────────────────────────────────────────────────────────────────────
# Shape: two outer verticals (cols 1-3 and cols 12-14), inner diagonals meeting
# at centre top (V-like).  Row 6: full horizontal span for the initial down-diags.
# Left pillar: cols 1-3.  Right pillar: cols 12-14.
# Left inner diagonal: descends from cols 4-6 at row 6 to cols 5-7 at row ~10.
# Right inner diagonal: mirror.
# Both diagonals meet at centre (cols 6-8 or 7-9) around row 12.
M_grid = [
    "0000000000000000",  # row 0  (blank)
    "0000000000000000",  # row 1
    "0000000000000000",  # row 2
    "0000000000000000",  # row 3
    "0000000000000000",  # row 4
    "0000000000000000",  # row 5
    "0111000000011100",  # row 6   left||| + right|||
    "0111000000011100",  # row 7
    "0111000000011100",  # row 8
    "0111100000111100",  # row 9   diagonals start inward
    "0111110001111100",  # row 10
    "0111111011111100",  # row 11
    "0111001110011100",  # row 12  peak of V
    "0111000100011100",  # row 13
    "0111000000011100",  # row 14  back to plain verticals
    "0111000000011100",  # row 15
    "0111000000011100",  # row 16
    "0111000000011100",  # row 17
    "0111000000011100",  # row 18
    "0111000000011100",  # row 19
    "0111000000011100",  # row 20
    "0111000000011100",  # row 21
    "0111000000011100",  # row 22
    "0111000000011100",  # row 23
]

# ─────────────────────────────────────────────────────────────────────────────
# 'S'  (uppercase S — mirror of lowercase s but taller, rows 6-23)
# Matches font_5 / font_s style: top arc, middle break, bottom arc.
# ─────────────────────────────────────────────────────────────────────────────
S_grid = [
    "0000000000000000",  # row 0
    "0000000000000000",  # row 1
    "0000000000000000",  # row 2
    "0000000000000000",  # row 3
    "0000000000000000",  # row 4
    "0000000000000000",  # row 5
    "0011111111110000",  # row 6   top arc
    "0011111111110000",  # row 7
    "0011111111110000",  # row 8
    "0011100000000000",  # row 9   left side only
    "0011100000000000",  # row 10
    "0011100000000000",  # row 11
    "0011111111110000",  # row 12  middle bar
    "0011111111110000",  # row 13
    "0011111111110000",  # row 14
    "0000000001110000",  # row 15  right side only
    "0000000001110000",  # row 16
    "0000000001110000",  # row 17
    "0011111111110000",  # row 18  bottom arc
    "0011111111110000",  # row 19
    "0011111111110000",  # row 20
    "0000000000000000",  # row 21
    "0000000000000000",  # row 22
    "0000000000000000",  # row 23
]

# ─────────────────────────────────────────────────────────────────────────────
# 'd'  (lowercase d — ascender: tall left stroke from row 6, bowl from row 12)
# Mirror of 'b': right vertical pillar (cols 12-14) + left bowl.
# ─────────────────────────────────────────────────────────────────────────────
d_grid = [
    "0000000000000000",  # row 0
    "0000000000000000",  # row 1
    "0000000000000000",  # row 2
    "0000000000000000",  # row 3
    "0000000000000000",  # row 4
    "0000000000000000",  # row 5
    "0000000001110000",  # row 6   right ascender only (cols 9-11)
    "0000000001110000",  # row 7
    "0000000001110000",  # row 8
    "0000000001110000",  # row 9
    "0000000001110000",  # row 10
    "0000000001110000",  # row 11
    "0001111111110000",  # row 12  bowl top arc starts
    "0001111111110000",  # row 13
    "0001111111110000",  # row 14
    "0011100001110000",  # row 15  sides
    "0011100001110000",  # row 16
    "0011100001110000",  # row 17
    "0001111111110000",  # row 18  bowl bottom arc
    "0001111111110000",  # row 19
    "0001111111110000",  # row 20
    "0000000000000000",  # row 21
    "0000000000000000",  # row 22
    "0000000000000000",  # row 23
]

# ─────────────────────────────────────────────────────────────────────────────
# 'h'  (lowercase h — tall left stroke from row 6, right arch from row 12)
# Similar to 'n' but with ascender.  Left pillar cols 1-3, right pillar cols 10-12.
# ─────────────────────────────────────────────────────────────────────────────
h_grid = [
    "0000000000000000",  # row 0
    "0000000000000000",  # row 1
    "0000000000000000",  # row 2
    "0000000000000000",  # row 3
    "0000000000000000",  # row 4
    "0000000000000000",  # row 5
    "0111000000000000",  # row 6   left ascender
    "0111000000000000",  # row 7
    "0111000000000000",  # row 8
    "0111000000000000",  # row 9
    "0111000000000000",  # row 10
    "0111000000000000",  # row 11
    "0111111111110000",  # row 12  arch top
    "0111111111110000",  # row 13
    "0111111111110000",  # row 14
    "0111000000011100",  # row 15  two verticals (cols 1-3, cols 12-14)  — wait, that's too wide
    "0111000001110000",  # row 16  left pillar + right pillar (cols 1-3, cols 9-11)
    "0111000001110000",  # row 17
    "0111000001110000",  # row 18
    "0111000001110000",  # row 19
    "0111000001110000",  # row 20
    "0111000001110000",  # row 21
    "0111000001110000",  # row 22
    "0111000001110000",  # row 23
]
# Fix rows 15-16 to be consistent:
h_grid[15] = "0111000001110000"

# ─────────────────────────────────────────────────────────────────────────────
# 'x'  (lowercase x — x-height only, rows 12-23, two crossing diagonals)
# ─────────────────────────────────────────────────────────────────────────────
x_grid = [
    "0000000000000000",  # row 0
    "0000000000000000",  # row 1
    "0000000000000000",  # row 2
    "0000000000000000",  # row 3
    "0000000000000000",  # row 4
    "0000000000000000",  # row 5
    "0000000000000000",  # row 6
    "0000000000000000",  # row 7
    "0000000000000000",  # row 8
    "0000000000000000",  # row 9
    "0000000000000000",  # row 10
    "0000000000000000",  # row 11
    "0111000001110000",  # row 12  top: left arm + right arm
    "0011100011100000",  # row 13
    "0001110111000000",  # row 14
    "0000111110000000",  # row 15  centre overlap
    "0000011100000000",  # row 16  crossing centre (3-wide)
    "0000111110000000",  # row 17
    "0001110111000000",  # row 18
    "0011100011100000",  # row 19
    "0111000001110000",  # row 20  bottom: left arm + right arm
    "0000000000000000",  # row 21
    "0000000000000000",  # row 22
    "0000000000000000",  # row 23
]

# ─────────────────────────────────────────────────────────────────────────────
# '/'  (forward slash — diagonal line, rows 6-23)
# ─────────────────────────────────────────────────────────────────────────────
slash_grid = [
    "0000000000000000",  # row 0
    "0000000000000000",  # row 1
    "0000000000000000",  # row 2
    "0000000000000000",  # row 3
    "0000000000000000",  # row 4
    "0000000000000000",  # row 5
    "0000000000011100",  # row 6   bottom-right  (cols 12-14)
    "0000000000011100",  # row 7
    "0000000000011100",  # row 8
    "0000000001110000",  # row 9
    "0000000001110000",  # row 10
    "0000000001110000",  # row 11
    "0000000111000000",  # row 12
    "0000000111000000",  # row 13
    "0000000111000000",  # row 14
    "0000011100000000",  # row 15
    "0000011100000000",  # row 16
    "0000011100000000",  # row 17
    "0001110000000000",  # row 18
    "0001110000000000",  # row 19
    "0001110000000000",  # row 20   top-left (cols 3-5)
    "0000000000000000",  # row 21
    "0000000000000000",  # row 22
    "0000000000000000",  # row 23
]

# ─────────────────────────────────────────────────────────────────────────────
# Validate all grids (W=16 cols, H=24 rows) before emitting
# ─────────────────────────────────────────────────────────────────────────────
def validate(name, grid):
    assert len(grid) == H, f"{name}: {len(grid)} rows (need {H})"
    for i, row in enumerate(grid):
        assert len(row) == W, f"{name} row {i}: len={len(row)} (need {W})"
        assert all(c in '01' for c in row), f"{name} row {i}: bad chars"
        # rows 0-5 must be blank
        if i < 6:
            assert row == '0' * W, f"{name} row {i} must be blank, got '{row}'"

for label, g in [('M', M_grid), ('S', S_grid), ('d', d_grid),
                  ('h', h_grid), ('x', x_grid), ('/', slash_grid)]:
    validate(label, g)

# ─────────────────────────────────────────────────────────────────────────────
# Emit C declarations
# ─────────────────────────────────────────────────────────────────────────────
emit_char('M', 'font_M', M_grid)
emit_char('S', 'font_S', S_grid)
emit_char('d', 'font_d', d_grid)
emit_char('h', 'font_h', h_grid)
emit_char('x', 'font_x', x_grid)
emit_char('/', 'font_slash', slash_grid)
