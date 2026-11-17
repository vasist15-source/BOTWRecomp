#!/usr/bin/env python3
"""The controller pictures of Settings > Controls (runtime/data/controllers).

usage: tools/controller_images.py [--marks DIR]

The pictures are vector drawings, all CC0 (see runtime/data/controllers/README.md):
  xbox, dualshock4   TheHoodieGuy02's "8th Gen Console Vector Gamepad Collection" (opengameart.org), as it is
  switchpro          a Switch Pro Controller in that set's style, put together here from its parts
  dualsense          a DualSense in that set's style, derived here from its DualShock 4

This downloads the set, writes switchpro.svg and dualsense.svg, renders the four drawings (tools/svg2png.swift,
so macOS with Xcode's swift), crops each to the controller, scales it to 1000 pixels wide and writes
<name>.heic (with alpha; sips). It then prints the table of button positions for
runtime/src/overlay/controls_view.cpp (picture_def): the positions are known in the drawings' own
coordinates (SPOTS below) and follow the crop. --marks DIR also writes <name>_marks.png there, each picture
with the positions of controls_view.cpp's table drawn on it, to check them by eye.

Needs Pillow.
"""
import io
import os
import re
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'runtime', 'data', 'controllers')
SET = 'https://opengameart.org/sites/default/files/VectorGamepad.zip'
WIDTH = 1000
ORDER = ['south', 'east', 'west', 'north', 'lb', 'rb', 'lt', 'rt', 'menu', 'options', 'home', 'ls', 'rs',
         'dup', 'ddown', 'dleft', 'dright']


def mirrored(spots):
    """right-hand inputs from the left-hand ones of a symmetric drawing (2048 wide)"""
    for left, right in (('lb', 'rb'), ('lt', 'rt')):
        x, y, r = spots[left]
        spots[right] = (2048 - x, y, r)
    return spots


# where each input is on the drawings, in their own coordinates (x, y, radius)
PLAYSTATION = mirrored({
    'south': (1555, 1029, 50), 'east': (1681, 904, 50), 'west': (1431, 904, 50), 'north': (1554, 779, 50),
    'lb': (507, 590, 34), 'lt': (507, 536, 26),
    'menu': (1361.5, 742, 34), 'options': (688.5, 742, 34), 'home': (1019, 1155, 40),
    'ls': (754.6, 1138, 107), 'rs': (1293.4, 1138, 107),
    'dup': (494, 808, 46), 'ddown': (494, 1000, 46), 'dleft': (398, 904, 46), 'dright': (590, 904, 46),
})
SPOTS = {
    'xbox': mirrored({
        'south': (1408.5, 940.5, 50), 'east': (1512.5, 837.5, 50), 'west': (1304.5, 837.5, 50), 'north': (1408.5, 738.5, 50),
        'lb': (640, 602, 38), 'lt': (572, 558, 24),
        'menu': (1137.5, 837.5, 37), 'options': (923.5, 835.5, 37), 'home': (1024, 664, 46),  # the panel the Xbox button is on
        'ls': (653.5, 837.5, 106), 'rs': (1210.5, 1125.5, 106),
        'dup': (834.5, 1043.5, 34), 'ddown': (834.5, 1185.5, 34), 'dleft': (763.5, 1114.5, 34), 'dright': (905.5, 1114.5, 34),
    }),
    'dualshock4': PLAYSTATION,
    'dualsense': PLAYSTATION,
    'switchpro': mirrored({
        'south': (1420, 942.8, 51), 'east': (1512.8, 850, 51), 'west': (1327.2, 850, 51), 'north': (1420, 757.2, 51),
        'lb': (640, 606, 36), 'lt': (600, 546, 24),
        'menu': (1168, 735, 31), 'options': (880, 735, 31), 'home': (1118, 850, 35),
        'ls': (640, 850, 106), 'rs': (1216, 1085, 106),
        'dup': (834.5, 1013.5, 34), 'ddown': (834.5, 1155.5, 34), 'dleft': (763.5, 1084.5, 34), 'dright': (905.5, 1084.5, 34),
    }),
}
NOTES = {
    'xbox': 'A, B, X, Y; LB, RB, LT, RT; Menu, View, Xbox',
    'dualshock4': 'cross, circle, square, triangle; L1, R1, L2, R2; Options, Share, PS',
    'dualsense': 'cross, circle, square, triangle; L1, R1, L2, R2; Options, Create, PS',
    'switchpro': 'B, A, Y, X; L, R, ZL, ZR; +, −, Home',
}


def switchpro_svg(joycon):
    """A Switch Pro Controller in the set's style: a body and shoulders drawn here, the sticks and D-pad as
    on the set's Xbox One drawing, the letters and the Home icon of its Joy-Con drawing."""
    letters = re.findall(r'<(?:path|polygon) class="fil10"[^>]*/>', joycon)
    assert len(letters) == 4
    home = re.search(r'<path class="fil9" d="M1347\.5 1266\.5[^>]*/>', joycon).group(0)
    body = ('M 600 628 C 760 598, 1288 598, 1448 628 C 1560 650, 1640 700, 1690 800 '
            'C 1760 950, 1806 1180, 1800 1370 C 1796 1500, 1716 1580, 1622 1556 '
            'C 1530 1532, 1450 1400, 1352 1318 C 1300 1276, 1250 1262, 1180 1262 L 868 1262 '
            'C 798 1262, 748 1276, 696 1318 C 598 1400, 518 1532, 426 1556 '
            'C 332 1580, 252 1500, 248 1370 C 242 1180, 288 950, 358 800 C 408 700, 488 650, 600 628 Z')

    def shoulder(sign):
        def X(x):
            return 1024 + sign * (x - 1024)
        front = ('M %g 668 C %g 610, %g 572, %g 566 L %g 572 L %g 640 L %g 648 C %g 660, %g 700, %g 736 Z' %
                 (X(402), X(450), X(540), X(660), X(900), X(900), X(640), X(540), X(470), X(430)))
        back = ('M %g 606 C %g 548, %g 520, %g 520 L %g 530 L %g 572 L %g 566 C %g 572, %g 590, %g 622 Z' %
                (X(470), X(520), X(600), X(700), X(860), X(860), X(660), X(580), X(520), X(486)))
        return '<path class="fil1 str0" d="%s"/>\n  <path class="fil1 str0" d="%s"/>' % (back, front)

    return '''<?xml version="1.0" encoding="UTF-8"?>
<!-- A Switch Pro Controller in the style of TheHoodieGuy02's "8th Gen Console Vector Gamepad Collection"
     (CC0, opengameart.org), made for BOTWRecomp by tools/controller_images.py from that set's parts: the
     letters and the Home icon of its Joy-Con drawing, the sticks and D-pad of its Xbox One drawing. CC0. -->
<svg xmlns="http://www.w3.org/2000/svg" width="2048px" height="2048px" viewBox="0 0 2048 2048" style="fill-rule:evenodd; clip-rule:evenodd">
 <defs><style type="text/css"><![CDATA[
    .str0 {stroke:white;stroke-width:4;stroke-miterlimit:22.9256}
    .str0s {stroke:white;stroke-width:2.8;stroke-miterlimit:22.9256}
    .str1 {stroke:black;stroke-width:8;stroke-miterlimit:22.9256}
    .fil3 {fill:black}
    .fil1 {fill:#1A1A1A}
    .fil2 {fill:#4D4D4D}
    .fil9 {fill:white}
    .fil10 {fill:white;fill-rule:nonzero}
 ]]></style></defs>
 <g id="Pro_Controller">
  %(left)s
  %(right)s
  <path id="Body" class="fil2 str1" d="%(body)s"/>
  <g id="Left_Stick"><circle class="fil3 str0" cx="640" cy="850" r="106"/><circle class="fil1 str0" cx="640" cy="850" r="76"/></g>
  <g id="Right_Stick"><circle class="fil3 str0" cx="1216" cy="1085" r="106"/><circle class="fil1 str0" cx="1216" cy="1085" r="76"/></g>
  <polygon id="D-pad" class="fil3 str0" transform="translate(0,-30)" points="724.5,1146.5 802.5,1146.5 802.5,1224.5 866.5,1224.5 866.5,1146.5 944.5,1146.5 944.5,1082.5 866.5,1082.5 866.5,1004.5 802.5,1004.5 802.5,1082.5 724.5,1082.5 "/>
  <g id="Face" transform="translate(1420,850) scale(1.45) translate(-1401.5,-848.5)">
   <circle class="fil3 str0s" cx="1337.505" cy="848.495" r="35"/>
   <circle class="fil3 str0s" cx="1401.5" cy="784.5" r="35"/>
   <circle class="fil3 str0s" cx="1465.494" cy="848.495" r="35"/>
   <circle class="fil3 str0s" cx="1401.5" cy="912.489" r="35"/>
   %(letters)s
  </g>
  <g id="Minus"><circle class="fil3 str0" cx="880" cy="735" r="31"/><rect class="fil9" x="864" y="731" width="32" height="8"/></g>
  <g id="Plus"><circle class="fil3 str0" cx="1168" cy="735" r="31"/><path class="fil9" d="M1152 731h12v-12h8v12h12v8h-12v12h-8v-12h-12z"/></g>
  <g id="Capture"><rect class="fil1 str0" x="901" y="821" width="58" height="58" rx="6" ry="6"/><circle class="fil3" cx="930" cy="850" r="18"/></g>
  <g id="Home" transform="translate(1118,850) scale(1.06) translate(-1354.5,-1271.5)"><circle class="fil3 str0" cx="1354.5" cy="1271.5" r="33"/>%(home)s</g>
 </g>
</svg>
''' % dict(left=shoulder(1), right=shoulder(-1), body=body, letters='\n   '.join(letters), home=home)


def dualsense_svg(s):
    """A DualSense in the set's style, from its DualShock 4: a wider body with longer grips, a dark centre
    under the touchpad and the sticks, plain symbols, no lettering, no light bar, the mute button."""
    def sub(pattern, repl, text, count):
        text, k = re.subn(pattern, repl, text)
        assert k >= count if count < 0 else k == count, (pattern, k)
        return text
    s = sub(r'\s*<(?:path|polygon)(?: id="\d")? class="fil7"[^>]*/>', '', s, -10)   # "SHARE", "OPTIONS"
    s = sub(r'\s*<rect class="fil6"[^>]*/>', '', s, 1)                                # the light bar
    s = sub(r'\s*<path class="fil4" d="M763\.206 696\.044[^>]*/>', '', s, 1)          # the touchpad's dots
    s = sub(r'\.str([3456]) \{stroke:#[0-9A-F]{6};', r'.str\1 {stroke:#D9D9D9;', s, 4)  # the symbols: not coloured
    assert '.fil1 {fill:#4D4D4D}' in s and '.fil2 {fill:#333333}' in s
    s = s.replace('.fil1 {fill:#4D4D4D}', '.fil1 {fill:#5C5C5C}\n    .filc {fill:#262626}')
    s = s.replace('.fil2 {fill:#333333}', '.fil2 {fill:#4A4A4A}\n    .filp {fill:#262626}')
    s = sub(r'<path class="fil2 str1" d="M(1293\.423|754\.577) 985\.257', r'<path class="filp str1" d="M\1 985.257', s, 2)
    body = ('M 1024 588 L 1400 588 C 1500 560, 1600 566, 1668 640 C 1790 780, 1850 1010, 1872 1290 '
            'C 1886 1470, 1840 1600, 1740 1624 C 1650 1644, 1570 1560, 1508 1420 C 1470 1330, 1440 1270, 1380 1250 '
            'L 668 1250 C 608 1270, 578 1330, 540 1420 C 478 1560, 398 1644, 308 1624 C 208 1600, 162 1470, 176 1290 '
            'C 198 1010, 258 780, 380 640 C 448 566, 548 560, 648 588 Z')
    s = sub(r'(<path id="Stick" class="fil1 str0" d=")[^"]*(")', lambda m: m.group(1) + body + m.group(2), s, 1)
    centre = ('M 716 600 L 1332 600 L 1340 940 C 1420 990, 1462 1070, 1456 1160 C 1448 1270, 1360 1320, 1280 1296 '
              'L 768 1296 C 688 1320, 600 1270, 592 1160 C 586 1070, 628 990, 708 940 Z')
    assert '<g id="Right_x0020_Stick">' in s and '<g id="Options">' in s
    s = s.replace('<g id="Right_x0020_Stick">', '<path id="Centre" class="filc str1" d="%s"/>\n  <g id="Right_x0020_Stick">' % centre, 1)
    s = sub(r'(<path class="fil3 str2" d=")M741\.153 616[^"]*(")',
            r'\1M 738 612 L 1310 612 L 1292 925 C 1290 950, 1272 964, 1250 964 L 798 964 C 776 964, 758 950, 756 925 Z\2', s, 1)
    s = s.replace('<g id="Options">', '<rect id="Mute" class="fil3 str2" x="994" y="1218" width="50" height="18" rx="9" ry="9"/>\n  <g id="Options">', 1)
    return s.replace('<!-- Creator: CorelDRAW 2017 -->',
                     '<!-- A DualSense in the style of TheHoodieGuy02\'s "8th Gen Console Vector Gamepad Collection" (CC0,\n'
                     '     opengameart.org), made for BOTWRecomp by tools/controller_images.py from that set\'s DualShock 4\n'
                     '     drawing. CC0. -->')


def table_spots():
    """name -> {input: (x, y, r)} from controls_view.cpp's picture_def table"""
    text = open(os.path.join(ROOT, 'runtime', 'src', 'overlay', 'controls_view.cpp')).read()
    out = {}
    for m in re.finditer(r'// picture: (\w+)\n(.*?)\n\s*\};', text, re.S):
        nums = [float(x) for x in re.findall(r'(?<![A-Za-z_\d.])-?\d+(?:\.\d+)?', re.sub(r'//.*', '', m.group(2)))][1:]
        out[m.group(1)] = {n: tuple(nums[i * 3:i * 3 + 3]) for i, n in enumerate(ORDER)}
    return out


def main():
    marks = sys.argv[2] if len(sys.argv) > 2 and sys.argv[1] == '--marks' else None
    os.makedirs(OUT, exist_ok=True)
    req = urllib.request.Request(SET, headers={'User-Agent': 'Mozilla/5.0 (controller-images)'})
    with urllib.request.urlopen(req, timeout=120) as r:
        z = zipfile.ZipFile(io.BytesIO(r.read()))
    svg = {n: z.read(n).decode('utf-8') for n in ('Xbox One.svg', 'Dualshock 4.svg', 'Switch Joycon.svg')}
    drawings = {
        'xbox': svg['Xbox One.svg'],
        'dualshock4': svg['Dualshock 4.svg'],
        'switchpro': switchpro_svg(svg['Switch Joycon.svg']),
        'dualsense': dualsense_svg(svg['Dualshock 4.svg']),
    }
    for name in ('switchpro', 'dualsense'):  # the two made here: kept as vectors too
        with open(os.path.join(OUT, name + '.svg'), 'w') as f:
            f.write(drawings[name])
    table = table_spots() if marks else {}
    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, text in drawings.items():
            src, big = os.path.join(tmp, name + '.svg'), os.path.join(tmp, name + '_2048.png')
            with open(src, 'w') as f:
                f.write(text)
            subprocess.run(['xcrun', 'swift', os.path.join(ROOT, 'tools', 'svg2png.swift'), src, big, '2048'],
                           check=True, stdout=subprocess.DEVNULL)
            im = Image.open(big).convert('RGBA')
            x0, y0, x1, y1 = im.getchannel('A').getbbox()
            x0, y0, x1, y1 = x0 - 12, y0 - 12, x1 + 12, y1 + 12
            scale = WIDTH / (x1 - x0)
            im = im.crop((x0, y0, x1, y1)).resize((WIDTH, round((y1 - y0) * scale)), Image.LANCZOS)
            png = os.path.join(tmp, name + '.png')
            im.save(png)
            heic = os.path.join(OUT, name + '.heic')
            subprocess.run(['sips', '-s', 'format', 'heic', '-s', 'formatOptions', '80', png, '--out', heic],
                           check=True, stdout=subprocess.DEVNULL)
            print('%s: %dx%d, %d bytes' % (heic, im.width, im.height, os.path.getsize(heic)), file=sys.stderr)
            sp = SPOTS[name]
            cells = ['{%d, %d, %d}' % (round((sp[k][0] - x0) * scale), round((sp[k][1] - y0) * scale), round(sp[k][2] * scale)) for k in ORDER]
            rows.append('    // picture: %s\n    static const PictureDef %s = {%d,  // %s\n        %s,\n        %s,\n        %s,\n        %s,\n        %s,\n    };' %
                        (name, name, im.height, NOTES[name], ', '.join(cells[0:4]), ', '.join(cells[4:8]), ', '.join(cells[8:11]),
                         ', '.join(cells[11:13]), ', '.join(cells[13:17])))
            if marks:
                view = Image.new('RGBA', im.size, (14, 30, 46, 255))
                view.alpha_composite(im)
                dr = ImageDraw.Draw(view)
                for key, (x, y, r) in table.get(name, {}).items():
                    dr.ellipse((x - r, y - r, x + r, y + r), outline=(90, 205, 255, 255), width=3)
                    dr.ellipse((x - 3, y - 3, x + 3, y + 3), fill=(255, 160, 50, 255))
                    dr.text((x + r + 2, y - r - 2), key, fill=(255, 255, 0, 255))
                os.makedirs(marks, exist_ok=True)
                view.convert('RGB').save(os.path.join(marks, name + '_marks.png'))
    print('\n'.join(rows))


if __name__ == '__main__':
    main()
