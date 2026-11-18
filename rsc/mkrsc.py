#!/usr/bin/env python3
"""
mkrsc.py - builds FArchive's GEM resource files, one per language, from
the texts in rsc/lang/*.txt (FView's resource builder, with FArchive's
own trees).

    python3 rsc/mkrsc.py          (from the project folder; make rsc)

Writes
    rsc/farchive.rsc (and farchivede.rsc ... for each rsc/lang/xx.txt there is)
    src/farsc.h                       object and string numbers for the C code

Every language gets the same trees with the same object numbers; only the
texts, and the positions and widths that follow from them, differ.

The files are plain (pre-AES 4) RSC files that every TOS loads, and the
script refuses anything that could upset TOS 1.x-4.x or EmuTOS:
  - a file over 32000 bytes (rsrc_load offsets are 16 bit; keep clear of
    the signed range);
  - alerts with more than 5 lines of 32 characters or 3 buttons of 10
    (the TOS form_alert limits; %.30s counts as 30, %ld as 8, %d as 5);
  - drop-down menus wider than 32 characters, longer than 20 items, or
    bigger than the AES's save buffer (a quarter of a 640x200 screen);
  - a menu bar wider than 79 characters;
  - dialogs that don't fit a 640x400 or 640x200 desktop in a window
    (also with MagiC's wider check boxes);
  - translations whose printf codes differ from the English ones;
  - characters the Atari character set doesn't have (Czech is written
    without accents: the ST system font has no Czech letters).
"""
import os, re, sys, struct, unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)

LANGS = [('en', ''), ('de', 'DE'), ('fr', 'FR'), ('it', 'IT'), ('es', 'ES'), ('cs', 'CZ')]

# ------------------------------------------------------------ AES values
G_BOX, G_TEXT, G_BOXTEXT, G_IMAGE, G_IBOX, G_BUTTON = 20, 21, 22, 23, 25, 26
G_STRING, G_FTEXT, G_FBOXTEXT, G_ICON, G_TITLE = 28, 29, 30, 31, 32
SELECTABLE, DEFAULT, EXIT, EDITABLE, RBUTTON, LASTOB = 1, 2, 4, 8, 16, 32
HIDETREE = 128
SELECTED, DISABLED, OUTLINED = 1, 8, 16

# Atari ST character set, 0x80-0xFF (Wikipedia, "Atari ST character set")
ATARI_HIGH = ("ÇüéâäàåçêëèïîìÄÅÉæÆôöòûùÿÖÜ¢£¥ßƒáíóúñÑªº¿⌐¬½¼¡«»ãõØøœŒÀÃÕ¨´†¶©®™"
              "ĳĲאבגדהוזחטיכלמנסעפצקרשתןךםףץ§∧∞αβΓπΣσµτΦΘΩδ∮ϕ∈∩≡±≥≤⌠⌡÷≈°•·√ⁿ²³¯")
assert len(ATARI_HIGH) == 128
ATARI = {c: 0x80 + i for i, c in enumerate(ATARI_HIGH)}


def encode(text, lang, where):
    if lang == 'cs':          # no Czech letters in the ST font: no accents
        text = ''.join(c for c in unicodedata.normalize('NFD', text)
                       if unicodedata.category(c) != 'Mn')
    out = bytearray()
    for c in text:
        o = ord(c)
        if o < 0x80:
            out.append(o)
        elif c in ATARI:
            out.append(ATARI[c])
        else:
            die('%s: %r has no Atari character (%s)' % (where, c, lang))
    return bytes(out)


ERRORS = []


def die(msg):
    ERRORS.append(msg)


# ------------------------------------------------------------- the texts
def load_texts(lang):
    t = {}
    path = os.path.join(HERE, 'lang', lang + '.txt')
    with open(path, encoding='utf-8') as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip('\n')
            if not line.strip() or line.lstrip().startswith('#'):
                continue
            m = re.match(r'^\s*(\w+)\s*=\s*"(.*)"\s*$', line)
            if not m:
                die('%s:%d: not key = "text"' % (path, n))
                continue
            v = m.group(2).replace('\\"', '"').replace('\\\\', '\\')
            t[m.group(1)] = v
    return t


SPEC = re.compile(r'%(?:%|[-+ #0]*(\d+)?(?:\.(\d+))?(l?)([sdxuc]))')


def specs(s):
    return [m.group(0) for m in SPEC.finditer(s) if m.group(0) != '%%']


def shown_len(s):
    """longest length of s once formatted (for the limit checks)"""
    n, pos = 0, 0
    for m in SPEC.finditer(s):
        n += m.start() - pos
        pos = m.end()
        if m.group(0) == '%%':
            n += 1
        elif m.group(4) == 's':
            n += int(m.group(2)) if m.group(2) else 32
        else:
            # a width says how wide it is (%3d: 3); otherwise a guess
            n += int(m.group(1)) if m.group(1) else (8 if m.group(3) else 5)
    return n + len(s) - pos


def check_alert(key, s, lang):
    m = re.match(r'^\[(\d)\]\[(.*)\]\[(.*)\]$', s)
    if not m:
        die('%s %s: not an alert: %s' % (lang, key, s))
        return
    lines, buttons = m.group(2).split('|'), m.group(3).split('|')
    if len(lines) > 5:
        die('%s %s: %d lines (5 at most)' % (lang, key, len(lines)))
    for l in lines:
        if shown_len(l) > 32:
            die('%s %s: line "%s" can be %d characters (32 at most)' % (lang, key, l, shown_len(l)))
    if len(buttons) > 3:
        die('%s %s: %d buttons (3 at most)' % (lang, key, len(buttons)))
    for b in buttons:
        if shown_len(b) > 10:
            die('%s %s: button "%s" can be %d characters (10 at most)' % (lang, key, b, shown_len(b)))
    if len(s) > 255:
        die('%s %s: alert longer than 255 characters' % (lang, key))


# -------------------------------------------------------------- trees
def ch(c, px=0):
    """object coordinate: characters in the low byte, pixels in the high"""
    return (c & 0xFF) | ((px & 0xFF) << 8)


class Obj:
    def __init__(self, typ, flags, state, spec, x, y, w, h, name):
        self.typ, self.flags, self.state, self.spec = typ, flags, state, spec
        self.x, self.y, self.w, self.h = x, y, w, h
        self.name = name
        self.next = self.head = self.tail = -1


class Tree:
    def __init__(self, name):
        self.name = name
        self.objs = []

    def add(self, parent, typ, spec, x, y, w, h, flags=0, state=0, name=None):
        o = Obj(typ, flags, state, spec, x, y, w, h, name)
        idx = len(self.objs)
        self.objs.append(o)
        if parent is not None:
            p = self.objs[parent]
            if p.head < 0:
                p.head = idx
            else:
                self.objs[p.tail].next = idx
            p.tail = idx
            o.next = parent
        return idx

    def finish(self):
        self.objs[-1].flags |= LASTOB
        return self


class Ted:
    def __init__(self, text, tmpl, valid, font=3, just=0, color=0x1180, thick=-1):
        self.text, self.tmpl, self.valid = text, tmpl, valid
        self.font, self.just, self.color, self.thick = font, just, color, thick


class Bits:                       # a 32x32 picture, 1 = black
    def __init__(self):
        self.px = [[0] * 32 for _ in range(32)]

    def set(self, x, y):
        if 0 <= x < 32 and 0 <= y < 32:
            self.px[y][x] = 1

    def words(self):
        out = b''
        for row in self.px:
            v = 0
            for b in row:
                v = (v << 1) | b
            out += struct.pack('>I', v)
        return out


class Icon:
    def __init__(self, data, mask, text):
        self.data, self.mask, self.text = data, mask, text


# ------------------------------------------------------------- pictures
FONT5x7 = {
    '!': ["..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."],
    '?': [".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."],
}


def glyph(b, ch_, x0, y0, scale):
    for r, row in enumerate(FONT5x7[ch_]):
        for c, v in enumerate(row):
            if v == '#':
                for dy in range(scale):
                    for dx in range(scale):
                        b.set(x0 + c * scale + dx, y0 + r * scale + dy)


def alert_icons():
    note, quest, stop = Bits(), Bits(), Bits()
    for y in range(3, 30):                       # note: triangle with !
        half = (y - 3) * 14 // 26
        for x in (16 - half, 16 + half, 15 - half, 17 + half):
            note.set(x, y)
    for x in range(2, 31):
        note.set(x, 29); note.set(x, 30)
    for r, row in enumerate(FONT5x7['!']):
        for c, v in enumerate(row):
            if v == '#':
                for dy in range(3):
                    for dx in range(2):
                        note.set(12 + c * 2 + dx, 9 + r * 3 + dy)
    for i in range(32):                          # question: box with ?
        for t in (1, 2):
            quest.set(i, t); quest.set(i, 31 - t + 1); quest.set(t, i); quest.set(31 - t + 1, i)
    glyph(quest, '?', 9, 6, 3)
    import math                                  # stop: octagon and a bar
    pts = []
    for k in range(8):
        a = math.pi / 8 + k * math.pi / 4
        pts.append((16 + 15 * math.cos(a), 16 + 15 * math.sin(a)))
    for y in range(32):
        for x in range(32):
            inside = all((pts[(k + 1) % 8][0] - pts[k][0]) * (y + .5 - pts[k][1]) -
                         (pts[(k + 1) % 8][1] - pts[k][1]) * (x + .5 - pts[k][0]) >= 0
                         for k in range(8))
            if inside:
                stop.set(x, y)
    for y in range(13, 19):
        for x in range(7, 25):
            stop.px[y][x] = 0
    return note, quest, stop


# ----------------------------------------------------------------- menus
def menu_tree(T, titles, menus, lang, tname):
    """titles: keys; menus: lists of (key | '-' | ('$text',)) , shortcut).
    The layout of build_menu() in the old tree.c: root, bar, active area,
    titles, screen area, then each drop-down's box and items."""
    t = Tree(tname)
    tt = [' ' + T(k) + ' ' for k in titles]
    width = sum(len(s) for s in tt)
    if width + 2 > 79:
        die('%s %s: menu bar is %d characters wide' % (lang, tname, width + 2))
    root = t.add(None, G_IBOX, 0, 0, 0, 80, 25)
    bar = t.add(root, G_BOX, 0x00001100, 0, 0, 80, ch(0, 2) | 1)
    act = t.add(bar, G_IBOX, 0, 2, 0, width, ch(1, 3))
    x, tx = 0, []
    for i, s in enumerate(tt):
        tx.append(x)
        t.add(act, G_TITLE, s, x, 0, len(s), ch(1, 3), name='MT_%d' % i)
        x += len(s)
    scr = t.add(root, G_IBOX, 0, 0, ch(1, 3), 80, 19)
    items = {}
    for i, m in enumerate(menus):
        rows = []
        for key, short in m:
            if key == '-':
                rows.append(None)
            else:
                label = key[1:] if key.startswith('$') else T(key)
                rows.append((key, label, short))
        need = max(2 + shown_len(r[1]) + 2 + len(r[2]) + 1 for r in rows if r)
        if i == 0:
            need = max(need, 21)
        w = need
        texts = []
        for r in rows:
            if r is None:
                texts.append(('-', '-' * w, DISABLED))
                continue
            key, label, short = r
            pad = w - 2 - shown_len(label) - len(short) - 1
            texts.append((key, '  ' + label + ' ' * pad + short + ' ', 0))
        if w > 32:
            die('%s %s: menu %d is %d characters wide (32 at most)' % (lang, tname, i, w))
        if len(rows) > 20:
            die('%s %s: menu %d has %d items (20 at most)' % (lang, tname, i, len(rows)))
        # TOS saves the screen under a drop-down in a buffer of a quarter
        # of the screen: 640x200 in 2 planes is 8000 bytes = 32000 pixels
        if (w * 8 + 2) * (len(rows) * 8 + 2) > 32000:
            die('%s %s: menu %d is too big for the AES menu buffer' % (lang, tname, i))
        bx = 2 + tx[i]
        if bx + w > 80:
            bx = 80 - w
        box = t.add(scr, G_BOX, 0x00FF1100, bx, 0, w, len(rows), name='MB_%d' % i)
        for k, (key, text, st) in enumerate(texts):
            nm = None if key == '-' else ('MI_' + key.split('_', 1)[1].upper() if not key.startswith('$') else None)
            o = t.add(box, G_STRING, text, 0, k, w, 1, state=st, name=nm)
            items.setdefault(i, []).append(o)
    return t.finish(), items


# --------------------------------------------------------- flow dialogs
MAXW = 78        # root width in characters: 624 pixels, fits 640 with the frame
MAXH = 21        # rows: 336 + window frame fits 640x400; 168 fits 640x200


def check_dialog(t, lang, magic_right=0):
    """magic_right: the right edge of the widest row once MagiC has made
    its check boxes 1.5 characters wider each (tree_finish in wdlg.c)"""
    root = t.objs[0]
    w, h = root.w & 0xFF, root.h & 0xFF
    if max(w, magic_right + 1) > MAXW:
        die('%s %s: dialog is %d characters wide (%.1f with MagiC check boxes)'
            % (lang, t.name, w, magic_right + 1))
    if h > MAXH:
        die('%s %s: dialog is %d rows high (%d at most)' % (lang, t.name, h, MAXH))


def row_buttons(t, root, T, keys, x, y, exit_=True, default=None, gap=1, minw=8):
    out = []
    for k in keys:
        w = max(len(T(k)) + 2, minw)
        o = t.add(root, G_BUTTON, T(k), x, y, w, 1,
                  flags=SELECTABLE | (EXIT if exit_ else 0) | (DEFAULT if k == default else 0),
                  name=None)
        out.append(o)
        x += w + gap
    return out, x


def field_ted():
    return Ted('', '_____', '99999')


def alert_tree(icons):
    """laid out by the program (wdlg.c) for each alert"""
    t = Tree('ALERT')
    root = t.add(None, G_BOX, 0x00021100, 0, 0, 40, 9, state=OUTLINED, name='A_ROOT')
    for nm, b in zip(('A_NOTE', 'A_QUEST', 'A_STOP'), icons):
        t.add(root, G_IMAGE, ('bitblk', b), 2, 1, ch(0, 32), ch(0, 32), flags=HIDETREE, name=nm)
    for i in range(5):
        t.add(root, G_STRING, '', 9, 1 + i, 32, 1, name='A_L%d' % (i + 1))
    for i in range(3):
        t.add(root, G_BUTTON, '', 9 + i * 12, 7, 10, 1, flags=SELECTABLE | EXIT, name='A_B%d' % (i + 1))
    return t.finish()


def fc_progress_tree(T):
    """FConv's progress dialog; laid out by fconv.c"""
    t = Tree('FPROGRESS')
    root = t.add(None, G_BOX, 0x00011100, 0, 0, 44, 10, name='P_ROOT')
    t.add(root, G_STRING, '', 2, 1, 40, 1, name='P_TOTAL')
    b = t.add(root, G_BOX, 0x00FF1100, 2, 2, 40, ch(0, 12), name='P_TBOX')
    t.add(b, G_BOX, 0x00001171, 0, 0, 0, ch(0, 12), name='P_TBAR')
    t.add(root, G_STRING, '', 2, 4, 40, 1, name='P_FILE')
    b = t.add(root, G_BOX, 0x00FF1100, 2, 5, 40, ch(0, 12), name='P_FBOX')
    t.add(b, G_BOX, 0x00001171, 0, 0, 0, ch(0, 12), name='P_FBAR')
    t.add(root, G_STRING, '', 2, 7, 28, 1, name='P_STAT')
    t.add(root, G_BUTTON, T('b_cancel'), 31, 7, 11, 1, flags=SELECTABLE | EXIT | DEFAULT, name='P_BTN')
    # shown beside OK only when something failed: opens the log
    t.add(root, G_BUTTON, T('b_log'), 19, 7, 11, 1, flags=SELECTABLE | EXIT | HIDETREE, name='P_LOG')
    return t.finish()


def icon_tree(name, bits, text):
    d, m = bits
    t = Tree(name)
    root = t.add(None, G_IBOX, 0, 0, 0, ch(0, 64), ch(0, 40))
    t.add(root, G_ICON, Icon(d, m, text), 0, 0, ch(0, 64), ch(0, 40))
    return t.finish()


# ------------------------------------------------------------ the writer
def write_rsc(path, trees, frstrs, lang):
    strings = bytearray()
    sref = {}

    def addstr(b):
        off = 36 + len(strings)
        strings.extend(b + b'\0')
        return off

    # pass 1: strings
    tedlist, iconlist, bitlist = [], [], []
    for t in trees:
        for o in t.objs:
            s = o.spec
            if isinstance(s, str):
                o._soff = addstr(encode(s, lang, t.name))
            elif isinstance(s, Ted):
                s._t = addstr(encode(s.text, lang, t.name).ljust(len(s.tmpl), b'\0')[:len(s.tmpl)])
                s._m = addstr(s.tmpl.encode())
                s._v = addstr(s.valid.encode())
                tedlist.append(s)
            elif isinstance(s, Icon):
                s._txt = addstr(encode(s.text, lang, t.name).ljust(12, b'\0'))
                iconlist.append(s)
            elif isinstance(s, tuple) and s[0] == 'bitblk':
                bitlist.append(s[1])
    froffs = [addstr(encode(s, lang, 'free string')) for s in frstrs]
    if len(strings) & 1:
        strings.append(0)
    pos = 36 + len(strings)
    # image data
    imdata = bytearray()
    for ic in iconlist:
        ic._m = pos + len(imdata); imdata += ic.mask.words()
        ic._d = pos + len(imdata); imdata += ic.data.words()
    for b in bitlist:
        b._d = pos + len(imdata); imdata += b.words()
    o_imdata = pos
    pos += len(imdata)
    o_frstr = pos
    frtab = b''.join(struct.pack('>I', x) for x in froffs)
    pos += len(frtab)
    o_bitblk = pos
    bits = b''
    for b in bitlist:
        b._off = pos + len(bits)
        bits += struct.pack('>Ihhhhh', b._d, 4, 32, 0, 0, 1)
    pos += len(bits)
    o_frimg = pos
    o_ted = pos
    teds = b''
    for s in tedlist:
        s._off = pos + len(teds)
        teds += struct.pack('>IIIhhhhhhhh', s._t, s._m, s._v, s.font, 0, s.just, s.color, 0,
                            s.thick, len(s.tmpl) + 1, len(s.tmpl) + 1)
    pos += len(teds)
    o_icon = pos
    icons = b''
    for ic in iconlist:
        ic._off = pos + len(icons)
        icons += struct.pack('>IIIhhhhhhhhhhh', ic._m, ic._d, ic._txt, 0x1000, 0, 0,
                             16, 0, 32, 32, 0, 32, 64, 8)
    pos += len(icons)
    o_obj = pos
    objs = b''
    nobs = 0
    roots = []
    for t in trees:
        roots.append(pos + len(objs))
        for o in t.objs:
            s = o.spec
            if isinstance(s, str):
                spec = o._soff
            elif isinstance(s, Ted):
                spec = s._off
            elif isinstance(s, Icon):
                spec = s._off
            elif isinstance(s, tuple):
                spec = s[1]._off
            else:
                spec = s & 0xFFFFFFFF
            objs += struct.pack('>hhhHHHIHHHH', o.next, o.head, o.tail, o.typ, o.flags,
                                o.state, spec, o.x, o.y, o.w, o.h)
            nobs += 1
    pos += len(objs)
    o_tree = pos
    trtab = b''.join(struct.pack('>I', r) for r in roots)
    pos += len(trtab)
    size = pos
    hdr = struct.pack('>18H', 0, o_obj, o_ted, o_icon, o_bitblk, o_frstr, 36, o_imdata,
                      o_frimg, o_tree, nobs, len(trees), len(tedlist), len(iconlist),
                      len(bitlist), len(frstrs), 0, size)
    data = hdr + bytes(strings) + bytes(imdata) + frtab + bits + teds + icons + objs + trtab
    assert len(data) == size
    if size > 32000:
        die('%s: %d bytes (32000 at most)' % (os.path.basename(path), size))
    with open(path, 'wb') as f:
        f.write(data)
    return size


# ------------------------------------------------------- FArchive's trees
FA_MENUS = (
    ['m_farchive', 'm_file', 'm_edit', 'm_archive', 'm_options'],
    [
        [('mi_about', ''), ('-', ''), ('$Desk Accessory 1', ''), ('$Desk Accessory 2', ''),
         ('$Desk Accessory 3', ''), ('$Desk Accessory 4', ''), ('$Desk Accessory 5', ''),
         ('$Desk Accessory 6', '')],
        [('mi_open', '^O'), ('mi_new', '^N'), ('-', ''), ('mi_extract', '^E'), ('-', ''),
         ('mi_dest', '^D'), ('-', ''), ('mi_quit', '^Q')],
        [('mi_cut', '^X'), ('mi_copy', '^C'), ('mi_paste', '^V'), ('mi_delete', 'Del'),
         ('mi_rename', '^R'),
         ('-', ''), ('mi_selall', '^S'), ('mi_selnone', '^Z')],
        [('mi_xsel', '^T'), ('mi_xall', '^Y'), ('-', ''), ('mi_find', '^F'), ('mi_fnext', '^G'), ('mi_filter', '^L'),
         ('mi_flat', '^B'), ('mi_list', '^P'), ('mi_check83', '^K'), ('mi_fix83', '^J'), ('-', ''), ('mi_add', '^A'), ('-', ''), ('mi_close', '^U')],
        [('mi_xdrop', ''), ('mi_folder', ''), ('mi_paths', ''), ('mi_replace', ''),
         ('mi_subdirs', ''), ('mi_83', ''), ('mi_nomac', ''), ('mi_confirm', '')],
    ])

FORMATS = ['f_zip', 'f_lha', 'f_zoo', 'f_arc', 'f_lzx', 'f_tar', 'f_tgz', 'f_gz']


def about_tree(T, lang):
    """About FArchive: a window; AB_VER is filled in by the program"""
    t = Tree('ABOUT')
    lines = [T('ab_1'), T('ab_2'), T('ab_3'), T('ab_4')]
    w = max(44, max(len(s) for s in lines) + 4)
    root = t.add(None, G_BOX, 0x00021100, 0, 0, w, 10, state=OUTLINED, name='AB_ROOT')
    t.add(root, G_STRING, '', 2, 1, w - 4, 1, name='AB_VER')
    y = 3
    for i, s in enumerate(lines):
        t.add(root, G_STRING, s, 2, y, len(s), 1)
        y += 1
    bw = len(T('b_ok')) + 6
    t.add(root, G_BUTTON, T('b_ok'), w - 2 - bw, y + 1, bw, 1,
          flags=SELECTABLE | EXIT | DEFAULT, name='AB_OK')
    t.objs[0].h = y + 3
    check_dialog(t, lang)
    return t.finish()


def newfmt_tree(T, lang):
    """New archive: its format (radio buttons) and Best compression"""
    t = Tree('NEWFMT')
    names = [T(k) for k in FORMATS]
    bw = max(len(s) for s in names) + 2
    lab = T('nf_label')
    w = max(2 * bw + 7, len(lab) + 4, len(T('nf_best')) + 6)
    root = t.add(None, G_BOX, 0x00021100, 0, 0, w, 10, state=OUTLINED, name='NF_ROOT')
    t.add(root, G_STRING, lab, 2, 1, len(lab), 1)
    rows = (len(FORMATS) + 1) // 2
    box = t.add(root, G_IBOX, 0, 2, 3, 2 * bw + 2, rows, name='NF_GROUP')
    for i, s in enumerate(names):
        t.add(box, G_BUTTON, s, (i // rows) * (bw + 2), i % rows, bw, 1,
              flags=SELECTABLE | RBUTTON, name='NF_' + FORMATS[i][2:].upper())
    y = 3 + rows + 1
    t.add(root, G_BUTTON, T('nf_best'), 2, y, len(T('nf_best')) + 2, 1,
          flags=SELECTABLE, name='NF_BEST')
    y += 2
    b = max(len(T('b_ok')), len(T('b_cancel'))) + 4
    t.add(root, G_BUTTON, T('b_ok'), w - 2 - 2 * b - 2, y, b, 1,
          flags=SELECTABLE | EXIT | DEFAULT, name='NF_OK')
    t.add(root, G_BUTTON, T('b_cancel'), w - 2 - b, y, b, 1,
          flags=SELECTABLE | EXIT, name='NF_CANCEL')
    t.objs[0].h = y + 2
    check_dialog(t, lang, len(T('nf_best')) + 5)
    return t.finish()


def find_tree(T, lang):
    """Archive > Find: one field"""
    t = Tree('FIND')
    lab = T('fd_label')
    w = max(len(lab) + 4, 46)
    root = t.add(None, G_BOX, 0x00021100, 0, 0, w, 10, state=OUTLINED, name='FD_ROOT')
    t.add(root, G_STRING, lab, 2, 1, len(lab), 1)
    t.add(root, G_FBOXTEXT, Ted('', '_' * 40, 'X' * 40), 2, 3, 41, 1,
          flags=EDITABLE, name='FD_FIELD')
    b = max(len(T('b_find')), len(T('b_cancel'))) + 4
    t.add(root, G_BUTTON, T('b_find'), w - 2 - 2 * b - 2, 5, b, 1,
          flags=SELECTABLE | EXIT | DEFAULT, name='FD_OK')
    t.add(root, G_BUTTON, T('b_cancel'), w - 2 - b, 5, b, 1,
          flags=SELECTABLE | EXIT, name='FD_CANCEL')
    t.objs[0].h = 7
    check_dialog(t, lang)
    return t.finish()


def rename_tree(T, lang):
    """Edit > Rename: one field, the name within its folder"""
    t = Tree('RENAME')
    lab = T('rn_label')
    w = max(len(lab) + 4, 68)
    root = t.add(None, G_BOX, 0x00021100, 0, 0, w, 10, state=OUTLINED, name='RN_ROOT')
    t.add(root, G_STRING, lab, 2, 1, len(lab), 1)
    t.add(root, G_FBOXTEXT, Ted('', '_' * 64, 'X' * 64), 2, 3, 65, 1,
          flags=EDITABLE, name='RN_FIELD')
    b = max(len(T('b_rename')), len(T('b_cancel'))) + 4
    t.add(root, G_BUTTON, T('b_rename'), w - 2 - 2 * b - 2, 5, b, 1,
          flags=SELECTABLE | EXIT | DEFAULT, name='RN_OK')
    t.add(root, G_BUTTON, T('b_cancel'), w - 2 - b, 5, b, 1,
          flags=SELECTABLE | EXIT, name='RN_CANCEL')
    t.objs[0].h = 7
    check_dialog(t, lang)
    return t.finish()


def filter_tree(T, lang):
    """Archive > Filter: one field; Show all takes the filter away"""
    t = Tree('FILTER')
    lab = T('fl_label')
    w = max(len(lab) + 4, 46)
    root = t.add(None, G_BOX, 0x00021100, 0, 0, w, 10, state=OUTLINED, name='FL_ROOT')
    t.add(root, G_STRING, lab, 2, 1, len(lab), 1)
    t.add(root, G_FBOXTEXT, Ted('', '_' * 40, 'X' * 40), 2, 3, 41, 1,
          flags=EDITABLE, name='FL_FIELD')
    b = max(len(T('b_filter')), len(T('b_showall')), len(T('b_cancel'))) + 4
    t.add(root, G_BUTTON, T('b_filter'), w - 2 - 3 * b - 4, 5, b, 1,
          flags=SELECTABLE | EXIT | DEFAULT, name='FL_OK')
    t.add(root, G_BUTTON, T('b_showall'), w - 2 - 2 * b - 2, 5, b, 1,
          flags=SELECTABLE | EXIT, name='FL_ALL')
    t.add(root, G_BUTTON, T('b_cancel'), w - 2 - b, 5, b, 1,
          flags=SELECTABLE | EXIT, name='FL_CANCEL')
    t.objs[0].h = 7
    check_dialog(t, lang)
    return t.finish()


def farchive_icon():
    """a box with a lid and a zip down the middle (as farchive.c drew it)"""
    d = Bits()
    for x in range(3, 29):
        d.set(x, 9); d.set(x, 28); d.set(x, 13)
    for y in range(9, 29):
        d.set(3, y); d.set(28, y)
    for x in range(6, 26):
        d.set(x, 4)
    for y in range(4, 10):
        d.set(6 - (y - 4) // 2, y); d.set(25 + (y - 4) // 2, y)
    for y in range(13, 28):
        d.set(15, y); d.set(16, y)
        if y & 1:
            d.set(14, y); d.set(17, y)
    for x in range(13, 19):
        d.set(x, 15); d.set(x, 18)
    for y in (16, 17):
        d.set(13, y); d.set(18, y)
    m = Bits()
    for y in range(4, 29):
        for x in range(3, 29):
            m.set(x, y)
    return d, m


def header_name(key):
    return 'S_' + key.upper()


# free strings: everything in en.txt that the trees don't use
TREE_ONLY = re.compile(r'^(m_|mi_|f_|nf_|fd_|rn_|fl_|ab_|b_)')


def build(lang, texts, en):
    def T(k):
        if k not in texts:
            if lang != 'en':
                print('  %s: %s not translated, English used' % (lang, k))
            return en[k]
        return texts[k]

    for k, v in texts.items():
        if k not in en:
            die('%s: unknown key %s' % (lang, k))
        elif specs(v) != specs(en[k]):
            die('%s %s: printf codes %s, English has %s' % (lang, k, specs(v), specs(en[k])))
    for k in en:
        if T(k).startswith('[') and T(k)[1:2].isdigit():
            check_alert(k, T(k), lang)

    note, quest, stop = alert_icons()
    menu, _ = menu_tree(T, FA_MENUS[0], FA_MENUS[1], lang, 'MENU')
    trees = [menu, about_tree(T, lang), newfmt_tree(T, lang), find_tree(T, lang), rename_tree(T, lang), filter_tree(T, lang),
             alert_tree((note, quest, stop)), fc_progress_tree(T),
             icon_tree('ICON', farchive_icon(), 'FArchive')]
    strings = [k for k in en if not TREE_ONLY.match(k)]
    suffix = dict(LANGS)[lang]
    size = write_rsc(os.path.join(HERE, 'farchive%s.rsc' % suffix.lower()), trees,
                     [T(k) for k in strings], lang)
    return trees, strings, size


def write_header(path, prog, trees, strings):
    lines = ['/* %s - generated by rsc/mkrsc.py: do not edit (edit rsc/lang/xx.txt) */' % os.path.basename(path),
             '#ifndef %s_H' % prog.upper(), '#define %s_H' % prog.upper(), '']
    for i, t in enumerate(trees):
        lines.append('#define T_%-12s %d' % (t.name, i))
    lines.append('')
    for t in trees:
        named = [(o.name, i) for i, o in enumerate(t.objs) if o.name]
        if named:
            lines.append('/* %s */' % t.name)
            for nm, i in named:
                lines.append('#define %-14s %d' % (nm, i))
        lines.append('#define %-14s %d' % (t.name + '_NOBJ', len(t.objs)))
        lines.append('')
    for i, k in enumerate(strings):
        lines.append('#define %-18s %d' % (header_name(k), i))
    lines.append('#define %-18s %d' % ('S_COUNT', len(strings)))
    lines += ['', '#endif', '']
    with open(path, 'w') as f:
        f.write('\n'.join(lines))


def main():
    en = load_texts('en')
    results = {}
    for lang, suf in LANGS:
        if lang != 'en' and not os.path.exists(os.path.join(HERE, 'lang', lang + '.txt')):
            continue
        texts = load_texts(lang) if lang != 'en' else en
        results[lang] = build(lang, texts, en)
    if ERRORS:
        print('\n'.join(ERRORS))
        print('%d problem(s): the resource files are not usable' % len(ERRORS))
        sys.exit(1)
    for lang in results:
        a = [[(o.typ, o.flags & ~DEFAULT, o.name) for o in t.objs] for t in results[lang][0]]
        b = [[(o.typ, o.flags & ~DEFAULT, o.name) for o in t.objs] for t in results['en'][0]]
        if a != b:
            print('%s: trees differ from English' % lang)
            sys.exit(1)
    trees, strings, _ = results['en']
    write_header(os.path.join(TOP, 'src', 'farsc.h'), 'farsc', trees, strings)
    for lang in results:
        print('%-2s  farchive%s.rsc %5d bytes' % (lang, dict(LANGS)[lang].lower(), results[lang][2]))


if __name__ == '__main__':
    main()
