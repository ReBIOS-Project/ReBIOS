#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# This file is part of ReBIOS.
# Copyright (C) 2026 Mintsuki and contributors
#
# ReBIOS is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License version 2 as published by the Free
# Software Foundation.
#
# ReBIOS is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
# details.
#
# You should have received a copy of the GNU General Public License along
# with ReBIOS. If not, see <https://www.gnu.org/licenses/>.

# Usage: insn-mem-table.py <SDM text> <APM text> <output.c> [report]
#
# Generates src/virt/insn_mem_table.c: what every instruction form with a ModRM
# memory operand does with that operand, whether it reads it, writes it, or
# both, and how many bytes of it. The forms and their operands come from the
# instruction pages of the Intel SDM 325462-092, Vols. 2 and 3C (opcode tables
# and operand encoding tables), and, for the instructions only AMD processors
# have, from the encoding tables of the AMD APM 40332 rev. 4.10, Vols. 3-5.
# Both texts are `pdftotext -layout` output of the manuals. With report, it
# also lists what it could not parse, the sizes it corrected, where the two
# manuals' sizes differ, and the AMD-only forms.

import re
import sys

# The notice each generated file starts with.
NOTICE = [
    '// SPDX-License-Identifier: GPL-2.0-only',
    '//',
    '// This file is part of ReBIOS.',
    '// Copyright (C) 2026 Mintsuki and contributors',
    '//',
    '// ReBIOS is free software: you can redistribute it and/or modify it under the',
    '// terms of the GNU General Public License version 2 as published by the Free',
    '// Software Foundation.',
    '//',
    '// ReBIOS is distributed in the hope that it will be useful, but WITHOUT ANY',
    '// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS',
    '// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more',
    '// details.',
    '//',
    '// You should have received a copy of the GNU General Public License along',
    '// with ReBIOS. If not, see <https://www.gnu.org/licenses/>.',
]

FOOTER = re.compile(r'(Vol\. 2[A-D] \d+-\d+|\d+-\d+ Vol\. 3C|Vol\. 3C \d+-\d+)\s*$')
NAME_CHARS = re.compile(r'^[A-Za-z0-9/,()\[\] .-]+$')
HEX = re.compile(r'^[0-9A-F]{2}$')
# The SDM's titles: "ADD\u2014Add".
DASH = '\u2014'
SIZES = {8, 16, 32, 64, 80, 128, 256, 384, 512}

# Instructions the SDM gives a ModRM memory operand which they only take the
# address of, or hint with: they read and write no data.
NO_ACCESS = {
    'LEA', 'NOP', 'INVLPG', 'BNDMK', 'BNDCL', 'BNDCU', 'BNDCN', 'CLFLUSH', 'CLFLUSHOPT',
    'CLWB', 'CLDEMOTE', 'PREFETCH', 'PREFETCHW', 'PREFETCHWT1', 'PREFETCHNTA', 'PREFETCHT0',
    'PREFETCHT1', 'PREFETCHT2', 'PREFETCHIT0', 'PREFETCHIT1',
}

# Sizes the operand syntax leaves out, from the instructions' descriptions:
# SDM 325462-092 Vol. 2B, SGDT and SIDT ("In legacy or compatibility mode, the
# destination operand is a 6-byte memory location ... In 64-bit mode, the
# operand size is fixed at 8+2 bytes"), and Vol. 2A, LDDQU (a 128-bit load).
MODE_SIZES = {'SGDT': (10, 6), 'SIDT': (10, 6)}
FIXED_SIZES = {'LDDQU': 16}

# Sizes the operand syntax gets wrong: SDM 325462-092 Vol. 2B, MOV, "REX.W +
# 8E /r MOV Sreg, r/m64: Move lower 16 bits of r/m64 to segment register",
# and AMD APM 40332 rev. 4.10 Vol. 4, VFMADDSD, whose encoding table has
# "xmm3/mem128" for "a 64-bit value read from memory".
SYNTAX_SIZES = {('MOV', 'Sreg, r/m64'): 2, ('VFMADDSD', 'xmm1, xmm2, xmm3/m128, xmm4'): 8,
                ('VFMADDSD', 'xmm1, xmm2, xmm3, xmm4/m128'): 8}

# Stores that leave bytes of their operand alone: FXSAVE, SDM 325462-092 Vol.
# 2A ("The processor does not write to bytes 464:511 of an FXSAVE area"),
# whose reserved fields in the area are not written either.
UNKNOWN_WRITES = {'FXSAVE', 'FXSAVE64'}

# Bit string instructions: SDM 325462-092 Vol. 2A, BT, "If the bit base
# operand specifies a memory location ... The offset operand then selects a
# bit position within the range -2^31 to 2^31 - 1 for a register offset".
BIT_STRINGS = {'BT', 'BTC', 'BTR', 'BTS'}

# Operand encoding tables of SDM 325462-092 Vol. 2 that have an instruction
# only write the memory operand it reads: ARPL compares it first ("IF
# DEST[RPL] < SRC[RPL]"), RCL, RCR, ROL, and ROR rotate it, as SAL, SAR,
# SHL, and SHR shift it, with "(r, w)", SBB subtracts from it, as ADC adds to
# it with "(r, w)", and SHLD and SHRD shift it; and those that have DIV write
# its divisor, which it only reads, as IDIV and MUL do with "(r)", and IMUL's
# one-operand form, which writes its product to registers.
ACCESS = {('ARPL', 'w'): 'rw', ('RCL', 'w'): 'rw', ('RCR', 'w'): 'rw', ('ROL', 'w'): 'rw',
          ('ROR', 'w'): 'rw', ('SBB', 'w'): 'rw', ('SHLD', 'w'): 'rw', ('SHRD', 'w'): 'rw',
          ('DIV', 'w'): 'r', ('IMUL', 'rw'): 'r'}

# Operands the encoding tables do not name ModRM:r/m: LKGS's is a "16-bit
# selector" it loads.
OPERAND_ACCESS = {'LKGS': 'r'}

# Sizes the tuple types corrected, for the report.
CORRECTED = []

# VEX forms whose W does not matter in 64-bit mode, from the SDM's footnotes.
W_IGNORED = set()

# Loads and stores of only the elements a vector register's mask selects.
VECTOR_MASKED = {'VMASKMOVPS', 'VMASKMOVPD', 'VPMASKMOVD', 'VPMASKMOVQ'}


def instruction_ranges(lines):
    # Chapter 3's instructions to the end of Vol. 2D, and the VMX instruction
    # reference in Vol. 3C.
    start = next(i for i, l in enumerate(lines) if re.match(r'^\d+\.\d+\s+INSTRUCTIONS \(A-L\)\s*$', l))
    end = max(i for i, l in enumerate(lines) if re.search(r'Vol\. 2D \d+-\d+\s*$', l)) + 1
    vmx = next(i for i, l in enumerate(lines) if l.strip() == 'VMX INSTRUCTION REFERENCE' and i > end)
    vmx_end = max(i for i, l in enumerate(lines)
                  if re.search(r'(33-\d+ Vol\. 3C|Vol\. 3C 33-\d+)\s*$', l)) + 1
    return [(start, end), (vmx, vmx_end)]


def is_title(lines, i):
    s = lines[i].strip()
    if DASH not in s or len(s) > 120 or '. .' in s or FOOTER.search(s):
        return False
    name = s.split(DASH, 1)[0]
    if not name or not name[0].isupper() or len(name) > 70 or not NAME_CHARS.match(name):
        return False
    # An instruction's pages start with its opcode table.
    return any(re.match(r'^\s*Opcode', l) for l in lines[i + 1:i + 6])


def sections(lines, start, end):
    titles = [i for i in range(start, end) if is_title(lines, i)]
    for n, i in enumerate(titles):
        yield lines[i].strip(), lines[i:titles[n + 1] if n + 1 < len(titles) else end]


# Opcode expression tokens.
OPCODE_TOKEN = re.compile(
    r'^(NP|NFx|66|F2|F3|REX\.W|REX\.R|REX|\+|[0-9A-F]{2}|[0-9A-F]{2}\+r[bwdo]|[0-9A-F]{2}\+i|'
    r'/r|/[0-7]|/is4|/ib|ib|iw|id|io|cb|cw|cd|cp|co|ct|imm8|'
    r'\+rb|\+rw|\+rd|\+ro|\+i|(?:VEX|EVEX)\.[A-Z0-9.]+|!?\(?11\)?:[0-7r]{3}:bbb|11:\S+|\(?mod=\S+|m64|'
    r'REX\.W\+|REX\+)$')


def opcode_tokens(text):
    tokens = []
    for t in text.replace('*', ' ').split():
        # Footnote markers.
        t = t[:-1] if re.match(r'^(/r|/[0-7]|ib|iw|id)\d$', t) else t
        if not OPCODE_TOKEN.match(t):
            break
        tokens.append(t)
    return tokens


def starts_opcode(line):
    t = opcode_tokens(line.strip())
    return any(HEX.match(x) for x in t) or any(x.startswith(('VEX.', 'EVEX.')) for x in t)


def new_key():
    return {'space': 'legacy', 'map': 0, 'opcode': None, 'prefix': None, 'reg': None, 'w': None,
            'l': None, 'reg_only': False}


def parse_key(tokens):
    key = new_key()
    hexes = []
    for t in tokens:
        if t.startswith(('VEX.', 'EVEX.')):
            fields = t.split('.')
            key['space'] = fields[0].lower()
            key['prefix'] = 'none'
            for f in fields[1:]:
                if f in ('128', 'L0', 'LZ'):
                    key['l'] = 0
                elif f in ('256', 'L1'):
                    key['l'] = 1
                elif f == '512':
                    key['l'] = 2
                elif f in ('66', 'F2', 'F3'):
                    key['prefix'] = f
                elif f == 'NP':
                    key['prefix'] = 'none'
                elif f in ('0F', '0F38', '0F3A'):
                    key['map'] = {'0F': 1, '0F38': 2, '0F3A': 3}[f]
                elif f in ('MAP5', 'MAP6'):
                    key['map'] = int(f[3])
                elif f == 'W0':
                    key['w'] = 0
                elif f == 'W1':
                    key['w'] = 1
        elif t in ('66', 'F2', 'F3') and not hexes and key['space'] == 'legacy':
            key['prefix'] = t
        elif t == 'NP':
            key['prefix'] = 'none'
        elif t in ('REX.W', 'REX.W+'):
            key['w'] = 1
        elif HEX.match(t):
            hexes.append(t)
        elif re.match(r'^/[0-7]$', t):
            key['reg'] = int(t[1])
        elif t.startswith('!'):
            if re.match(r'^!\(11\):[0-7]{3}:bbb$', t):
                key['reg'] = int(t[6:9], 2)
        elif t.startswith('11:') or t in ('mod=11', '(mod=11)'):
            key['reg_only'] = True
    # FWAIT before an x87 instruction is an instruction of its own.
    if len(hexes) > 1 and hexes[0] == '9B':
        hexes = hexes[1:]
    if not hexes:
        return None
    if key['space'] == 'legacy':
        if hexes[0] == '0F' and len(hexes) > 1:
            if hexes[1] in ('38', '3A') and len(hexes) > 2:
                key['map'] = 2 if hexes[1] == '38' else 3
                key['opcode'] = int(hexes[2], 16)
            else:
                key['map'] = 1
                key['opcode'] = int(hexes[1], 16)
        else:
            key['opcode'] = int(hexes[0], 16)
    else:
        key['opcode'] = int(hexes[0], 16)
    return key


def strip_footnote(part):
    # A footnote marker glued to an operand, as in "r/m82" or "m641".
    m = re.match(r'^(.*?)(\d+)$', part)
    if m and int(m.group(2)) not in SIZES and int(m.group(2)[:-1] or 0) in SIZES:
        return m.group(1) + m.group(2)[:-1]
    return part


def clean_syntax(text):
    # Mnemonics hyphenated across lines join back up.
    text = re.sub(r'(\w)- (\w)', r'\1\2', ' '.join(text.split()))
    # Footnote markers set apart, as in "r/m8 1".
    text = re.sub(r'\b((?:r/)?m\d+|r\d+) \d(?=,|$)', r'\1', text)
    # Typos, as in "ymm3/.m256", "xmm2/ m128", and "xmm2/mm128".
    text = re.sub(r'/[. ]+', '/', text)
    text = re.sub(r'/mm(\d)', r'/m\1', text)
    return text


def operands(syntax):
    words = syntax.split(None, 1)
    if len(words) < 2:
        return words[0] if words else '', []
    ops = []
    for o in words[1].split(','):
        o = o.strip()
        o = re.sub(r'\s*(\{[^}]*\})\s*', r'\1', o)
        base = re.sub(r'\{[^}]*\}', '', o)
        deco = o[len(base):] if o.startswith(base) else ''
        ops.append('/'.join(strip_footnote(p) for p in base.split('/')) + deco)
    return words[0], ops


MODE = re.compile(r'^(V|I|N\.?E\.?|NE|VV|Valid|Invalid)\d?(/(V|I|N\.?E?\.?)\d?)?$')
OP_EN = re.compile(r'^[A-Z][A-Z0-9-]{0,9}$')


def normalize(text):
    text = text.strip()
    # Lowercase opcode bytes and prefixes, as in "66 0f 38 21 /r" and "66
    # REX.w 0F 38 F6 /r".
    parts = re.split(r'(\s{2,})', text, maxsplit=1)
    parts[0] = re.sub(r'(?<!\S)([0-9][0-9a-f]|[0-9a-f][0-9])(?!\S)', lambda m: m.group(1).upper(), parts[0])
    parts[0] = re.sub(r'\bREX\.w\b', 'REX.W', parts[0])
    text = ''.join(parts)
    # Opcode bytes glued to their ModRM notation or a comma, or to each
    # other, as in "0F B0/r", "0F E0, /r", and "0F38".
    text = re.sub(r'\b([0-9A-F]{2})/(r|[0-7])\b', r'\1 /\2', text)
    text = re.sub(r'\b([0-9A-F]{2}),(?= )', r'\1', text)
    text = re.sub(r'(^|\s)0F(38|3A)(?=\s)', r'\g<1>0F \2', text)
    # Typos in VEX notation, as in "VEX.128.660F.WIG" and "VEX.LZ. 0F38".
    text = re.sub(r'\.(66|F2|F3)(0F)', r'.\1.\2', text)
    text = re.sub(r'^((?:E?VEX)\.\S*\.) (?=\S)', r'\1', text)
    text = re.sub(r'^((?:E?VEX)\.\S*0F) (38|3A)(?=[.\s])', r'\1\2', text)
    # A truncated "/r", as in "VEX.128.66.0F.W0 6E /".
    text = re.sub(r'(\b[0-9A-F]{2}) /(?=\s|$)', r'\1 /r', text)
    # Notes after the opcode, as in "/5 (mod!=11, /5, memory only)".
    text = re.sub(r'(/[0-7r]) \((mod!=11|mod ?!= ?11b?)[^)]*\)', r'\1', text)
    return text


def parse_modes(cells):
    # The 64-bit mode and compatibility/legacy mode columns, or one combined
    # "64/32-bit" column: the modes the row is valid in.
    cells = [c for c in cells if c]
    if not cells:
        return {'64', '32'}
    first = cells[0].replace(' ', '')
    if re.match(r'^VV\d?$', first):
        return {'64', '32'}
    if '/' in first:
        a, b = first.split('/', 1)
    else:
        a = first
        b = cells[1].replace(' ', '') if len(cells) > 1 else 'V'
    modes = set()
    if a.upper().startswith('V'):
        modes.add('64')
    if b.upper().startswith('V'):
        modes.add('32')
    return modes


# A line of superscripts: a footnote marker, then what follows it on the
# instruction's line, as in "1, r81" under "ADD r/m8".
SUPERSCRIPT = re.compile(r'^\s{8,}\d([ ,].*)$')


def parse_rows(block):
    # block: the lines of one opcode table. A row's instruction follows its
    # opcode on the same line, with the Op/En after it, or takes the lines
    # after the opcode's, the Op/En right after the opcode then. x87 tables
    # have no Op/En: None.
    rows = []
    # Tables without mode columns, Vol. 3C's, take the instruction from the
    # next line, and say in the description which modes have it.
    header = []
    for line in block:
        if starts_opcode(normalize(line)):
            break
        header.append(line)
    no_modes = not re.search(r'Mode|64', ' '.join(header))
    # Superscripts sit on the line above their row.
    pending = ''
    i = 0
    while i < len(block):
        line = block[i]
        if not starts_opcode(normalize(line)):
            m = SUPERSCRIPT.match(line)
            if m:
                pending += m.group(1)
            i += 1
            continue
        text = normalize(line)
        toks = opcode_tokens(text)
        rest = text.split(None, len(toks))
        rest = rest[len(toks)] if len(rest) > len(toks) else ''
        # A footnote marker on the opcode.
        rest = re.sub(r'^\d\s+', '', rest)
        fields = re.split(r'\s{2,}', rest)
        head = fields[0].split()
        second = fields[1].split()[0] if len(fields) > 1 and fields[1] else ''
        if no_modes:
            j = i + 1
            syntax = ''
            description = ' '.join(fields[1:])
            while j < len(block) and block[j].strip() and not starts_opcode(normalize(block[j])):
                cells = re.split(r'\s{2,}', block[j].strip())
                if len(block[j]) - len(block[j].lstrip()) < 8:
                    syntax += ' ' + cells[0]
                    cells = cells[1:]
                description += ' ' + ' '.join(cells)
                j += 1
            modes = ({'32'} if 'outside 64-bit' in description else {'64'} if 'in 64-bit' in description
                     else {'64', '32'})
            rows.append((toks, clean_syntax(syntax), head[0] if head else None, modes))
            i = j
            continue
        two_lines = (bool(head) and OP_EN.match(head[0]) is not None
                     and not re.search(r'[a-z]', fields[0])
                     and ((len(head) > 1 and MODE.match(head[1]) is not None)
                          or MODE.match(second) is not None))
        j = i + 1
        if fields[0].endswith('-') and len(fields) > 1:
            # A mnemonic hyphenated onto the next line.
            two_lines = True
            head = fields[1].split()
            fields = [fields[0]] + fields[1:]
        if two_lines:
            op_en = head[0]
            if fields[0].endswith('-'):
                modes = parse_modes(fields[2:])
            elif len(head) > 1:
                modes = parse_modes([' '.join(head[1:])] + fields[1:])
            else:
                modes = parse_modes(fields[1:])
            column = line.find(' ' + op_en + ' ', len(line) - len(line.lstrip()) + 1) + 1
            syntax = fields[0] if fields[0].endswith('-') else ''
            while j < len(block) and block[j].strip() and not starts_opcode(normalize(block[j])):
                first = re.split(r'\s{2,}', block[j].strip())[0]
                if not syntax and first.split() and all(OPCODE_TOKEN.match(t) for t in first.split()):
                    # The opcode continues, as in "!(11):rrr:bbb".
                    toks = toks + first.split()
                # Description lines are indented: the instruction starts left.
                elif len(block[j]) - len(block[j].lstrip()) < 8:
                    syntax += ' ' + first
                # An Op/En too wide for its column, as in "RV" over "M".
                more = block[j][column - 1:column + 4] if column > 0 else ''
                if re.match(r'^ [A-Z]{1,3}(\s|$)', more):
                    op_en += more.split()[0]
                j += 1
        else:
            syntax = fields[0] + pending
            # An Op/En one space from its mode column, as in "MRC Valid".
            cells = fields[1:]
            if cells and len(cells[0].split()) > 1 and MODE.match(cells[0].split()[1]):
                cells = cells[0].split(None, 1) + cells[1:]
            if MODE.match(second):
                op_en = None
                modes = parse_modes(cells[0:2])
            else:
                op_en = second
                modes = parse_modes(cells[1:3])
            # Long operands continue on the next line.
            while j < len(block) and block[j].strip() and not starts_opcode(normalize(block[j])):
                more = re.split(r'\s{2,}', block[j].strip())[0]
                if more.startswith(','):
                    syntax += more
                if SUPERSCRIPT.match(block[j]):
                    break
                j += 1
        pending = ''
        rows.append((toks, clean_syntax(syntax), op_en, modes))
        i = j
    return rows


def parse_encodings(lines):
    # Op/En -> (tuple type, [operands]). A table broken by a page repeats
    # its header.
    table = {}
    i = 0
    while i < len(lines):
        if 'Op/En' not in lines[i] or not re.search(r'Operand ?1', lines[i]):
            i += 1
            continue
        names = re.findall(r'Op/En|Tuple(?: Type)?|Operand \d', lines[i])
        i += 1
        while i < len(lines) and lines[i].strip() and not lines[i].startswith('Description'):
            cells = re.split(r'\s{2,}', lines[i].strip())
            if re.match(r'^[A-Z][A-Za-z0-9-]{0,9}$', cells[0]) and len(cells) > 1:
                entry = dict(zip(names, cells))
                ops = [entry.get('Operand %d' % k, '') for k in range(1, 5)]
                tuple_type = entry.get('Tuple Type', entry.get('Tuple', ''))
                table[cells[0]] = (tuple_type, ops)
            i += 1
    return table


def mem_spec(operand):
    # The memory part of an operand: ('n', bytes), ('far', bytes) for a far
    # pointer, ('pair', bytes) for BOUND's pair, ('env',) and ('state',) for
    # the x87 environment and state, ('unknown',), or None for a register.
    op = re.sub(r'\{[^}]*\}', '', operand).strip()
    if re.search(r'\bm14/28byte$', op):
        return ('env',), None
    if re.search(r'\bm94/108byte$', op):
        return ('state',), None
    spec = None
    bcst = None
    for p in op.split('/'):
        m = re.match(r'^m(\d+)(fp|int|dec|bcd)?$', p)
        if m:
            spec = ('n', int(m.group(1)) // 8)
            continue
        m = re.match(r'^m(\d+)byte$', p)
        if m:
            spec = ('n', int(m.group(1)))
            continue
        m = re.match(r'^m(\d+)bcst$', p)
        if m:
            bcst = int(m.group(1)) // 8
            continue
        m = re.match(r'^m16:(16|32|64)$', p)
        if m:
            spec = ('far', 2 + int(m.group(1)) // 8)
            continue
        m = re.match(r'^m(\d+)&(\d+)$', p)
        if m:
            kind = 'pair' if m.group(1) == m.group(2) else 'n'
            spec = (kind, (int(m.group(1)) + int(m.group(2))) // 8)
            continue
        if p in ('m', 'mem', 'sibmem') or re.match(r'^vm\d+[xyz]$', p):
            spec = ('unknown',)
    if spec is None:
        m = re.match(r'^r/m(\d+)$', op)
        if m:
            spec = ('n', int(m.group(1)) // 8)
    return spec, bcst


def tuple_size(tuple_type, l):
    # SDM 325462-092 Vol. 2A, 2.7.5, Tables 2-34 and 2-35: the vector tuple
    # types access a fraction of the vector length, broadcasts aside.
    fractions = {'full': 8, 'fullmem': 8, 'half': 16, 'halfmem': 16, 'quarter': 32, 'quartermem': 32,
                 'eighthmem': 64}
    t = tuple_type.replace(' ', '').lower()
    if l is None or t not in fractions:
        return None
    return (128 << l) // fractions[t]


def tuple_scale(tuple_type, mnemonic, spec):
    # EVEX's compressed displacement: SDM 325462-092 Vol. 2A, 2.7.5, Tables
    # 2-34 and 2-35. Broadcasts scale by the element, as do the scalar tuples,
    # whose memory operand is the element but for compressing and expanding
    # forms. 0 means the operand's size.
    t = tuple_type.replace(' ', '').lower()
    if spec[0] != 'n' or t not in ('tuple1scalar', 'tuple1fixed', 'scalar') or spec[1] <= 8:
        return 0
    for suffix, size in (('PS', 4), ('PD', 8), ('PH', 2), ('B', 1), ('W', 2), ('D', 4), ('Q', 8)):
        if mnemonic.endswith(suffix):
            return size
    return None


def sdm_form(title, encodings, toks, syntax, op_en, modes, problems):
    key = parse_key(toks)
    mnemonic, ops = operands(syntax)
    # Only a ModRM byte brings a memory operand. SETcc's and Vol. 3C's
    # opcodes leave theirs out.
    encoded = encodings.get(op_en, ('', []))[1] if op_en else []
    has_modrm = (any(re.match(r'^(/r|/[0-7]|!.*)$', t) for t in toks)
                 or any(re.search(r'(^|/)(r\d*|reg)/m', o) for o in ops)
                 or any(o.startswith('ModRM:r/m') for o in encoded))
    if key is None or key['reg_only'] or not has_modrm or not modes:
        return None
    memory = [n for n, o in enumerate(ops) if mem_spec(o)[0] is not None]
    vsib = [n for n, o in enumerate(ops) if re.match(r'^vm\d+[xyz]', o)]
    where = (title, ' '.join(toks), syntax)
    if not memory and not vsib:
        # A register-only form, unless the syntax lost its memory operand.
        if re.search(r'\bm\d|\bmem\b', syntax):
            problems.append(where + ('memory operand not understood',))
        return None
    tuple_type = ''
    if op_en is None:
        # x87 tables give no operand encoding: the titles of its pages that
        # write memory say they store or save.
        position = memory[0]
        access = 'w' if re.search(DASH + '(Store|Save)', title) else 'r'
    else:
        enc = encodings.get(op_en)
        if enc is None:
            problems.append(where + ('no operand encoding ' + op_en,))
            return None
        tuple_type, encoded = enc
        # "ModRM:rm" is PTWRITE's spelling.
        claims = [n for n, o in enumerate(encoded) if o.startswith(('ModRM:r/m', 'ModRM:rm'))]
        if not claims and mnemonic in OPERAND_ACCESS:
            claims = memory[:1]
            encoded = list(encoded)
            encoded[claims[0]] = '(%s)' % OPERAND_ACCESS[mnemonic]
        if not claims and vsib:
            # Gathers and scatters: their elements are anywhere.
            access = 'w' if 'SCATTER' in mnemonic else 'r'
            return make_form(key, mnemonic, syntax, access, ('unknown',), 0, access == 'w', 0, modes, 'sdm')
        # Where an encoding table names ModRM:r/m twice, the syntax tells.
        position = next((n for n in claims if n in memory), None)
        if position is None:
            problems.append(where + ('no ModRM:r/m operand with memory', encoded))
            return None
        m = re.search(r'\(([^)]*)\)', encoded[position])
        flags = m.group(1).lower().split(',')[0:2] if m else []
        access = ''.join(sorted({f.strip()[:1] for f in flags} & {'r', 'w'}))
        if not access:
            problems.append(where + ('no access in ' + encoded[position],))
            return None
    spec, bcst = mem_spec(ops[position])
    # Where an EVEX form's tuple type says how large its operand is, the
    # syntax should agree; VPMOVQD's EVEX.128 form has "xmm1/m128" for two
    # doublewords.
    expected = tuple_size(tuple_type, key['l'])
    if key['space'] == 'evex' and expected is not None and spec[0] == 'n' and spec[1] != expected:
        CORRECTED.append(where + ('size %d for %s, taken as %d' % (spec[1], tuple_type, expected),))
        spec = ('n', expected)
    masked = 'w' in access and re.search(r'\{k1?\}', ops[position]) is not None
    if mnemonic in VECTOR_MASKED:
        masked = True
    scale = tuple_scale(tuple_type, mnemonic, spec) if key['space'] == 'evex' else 0
    if scale is None:
        problems.append(where + ('no element size for ' + tuple_type,))
        return None
    if mnemonic in MODE_SIZES:
        return [make_form(key, mnemonic, syntax, access, ('n', size), 0, False, 0, {mode}, 'sdm')
                for mode, size in zip(('64', '32'), MODE_SIZES[mnemonic]) if mode in modes]
    return make_form(key, mnemonic, syntax, access, spec, bcst, masked, scale, modes, 'sdm')


def sdm_forms(path, problems):
    text = open(path, encoding='utf-8', errors='replace').read()
    lines = text.split('\n')
    # SDM 325462-092 Vol. 2C, VPEXTRB and the like: "In 64-bit mode, VEX.W1 is
    # ignored for VPEXTRB".
    W_IGNORED.update(re.findall(r'In 64-bit mode, VEX\.W1 is ignored for (\w+)', text))
    forms = []
    sects = [x for start, end in instruction_ranges(lines) for x in sections(lines, start, end)]
    for title, body in sects:
        encodings = parse_encodings(body)
        # Opcode tables: from a header line with "Opcode" to the operand
        # encoding table or the description.
        blocks = []
        current = None
        for line in body:
            if re.search(r'^\s*Opcode', line):
                current = [line]
                blocks.append(current)
            elif current is not None:
                if 'Instruction Operand Encoding' in line or line.startswith(('Description', 'NOTES')):
                    current = None
                else:
                    current.append(line)
        for block in blocks:
            for toks, syntax, op_en, modes in parse_rows(block):
                form = sdm_form(title, encodings, toks, syntax, op_en, modes, problems)
                if isinstance(form, list):
                    forms.extend(form)
                elif form is not None:
                    forms.append(form)
    return forms


def make_form(key, mnemonic, syntax, access, spec, bcst, masked, scale, modes, source):
    if source == 'sdm':
        syntax = ' '.join(filter(None, [mnemonic, ', '.join(operands(syntax)[1])]))
    f = dict(key)
    f.update({'mnemonic': mnemonic, 'syntax': syntax, 'access': access, 'spec': spec, 'bcst': bcst or 0,
              'masked': masked, 'scale': scale, 'modes': frozenset(modes), 'source': source})
    if mnemonic in NO_ACCESS:
        f['spec'] = ('none',)
        f['access'] = ''
        f['bcst'] = 0
    if mnemonic in UNKNOWN_WRITES and 'w' in access:
        f['spec'] = ('unknown',)
    if mnemonic in BIT_STRINGS and re.search(r', r\d', syntax):
        f['spec'] = ('unknown',)
    if mnemonic in FIXED_SIZES:
        f['spec'] = ('n', FIXED_SIZES[mnemonic])
    fix = SYNTAX_SIZES.get((mnemonic, syntax[len(mnemonic) + 1:]))
    if fix is not None:
        f['spec'] = ('n', fix)
    f['access'] = ACCESS.get((mnemonic, f['access']), f['access'])
    if mnemonic in W_IGNORED and f['space'] != 'legacy':
        f['w'] = None
    return f


# The APM's encodings: legacy opcodes, or "C4 RXB.map W.vvvv.L.pp opcode" for
# VEX and "8F RXB.map W.vvvv.L.pp opcode" for XOP.
APM_ROW = re.compile(r'^\s*([A-Z][A-Z0-9]+)((?: FAR)?)\s+'
                     r'([a-z][\w/:&.]*(?:,\s?[a-z0-9][\w/:&.]*)*)\s{2,}(\S.*)$')
APM_OPERAND = [
    (r'\breg(\d+)/mem(\d+)', r'r\1/m\2'), (r'\breg/mem', 'r/m'), (r'mem(\d+)&mem(\d+)', r'm\1&\2'),
    (r'mem(\d+)real', r'm\1fp'), (r'mem(\d+)env', r'm\1byte'), (r'mem14/28env', 'm14/28byte'),
    (r'mem94/108env', 'm94/108byte'), (r'\bmem', 'm'), (r'\bmmx(\d?)', r'mm\1'),
]


def apm_forms(path):
    forms = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = APM_ROW.match(line.rstrip('\n'))
        if not m:
            continue
        mnemonic = m.group(1)
        syntax = m.group(3)
        for pattern, replacement in APM_OPERAND:
            syntax = re.sub(pattern, replacement, syntax)
        ops = ['/'.join(strip_footnote(p) for p in o.strip().split('/')) for o in syntax.split(',')]
        key = apm_key(m.group(4).split())
        if key is None:
            continue
        position = next((n for n, o in enumerate(ops) if mem_spec(o)[0] is not None), None)
        if position is None:
            continue
        spec, bcst = mem_spec(ops[position])
        # AMD's syntax puts the destination first.
        access = 'w' if position == 0 else 'r'
        masked = mnemonic in VECTOR_MASKED
        forms.append(make_form(key, mnemonic, mnemonic + ' ' + ', '.join(ops), access, spec, bcst,
                               masked, 0, {'64', '32'}, 'apm'))
    return forms


def amd_only(f):
    # What AMD alone defines: XOP, AMD's VEX instructions, and in the legacy
    # maps 3DNow!, SSE4a, and PREFETCH; rows that lost their escape bytes to
    # another line would otherwise read as one-byte opcodes.
    if f['space'] == 'xop':
        return 8 <= f['map'] <= 10
    if f['space'] == 'vex':
        return 1 <= f['map'] <= 3
    return f['space'] == 'legacy' and f['map'] != 0


def apm_key(tokens):
    key = new_key()
    if len(tokens) >= 4 and tokens[0] in ('C4', '8F') and tokens[1].startswith('RXB.'):
        key['space'] = 'vex' if tokens[0] == 'C4' else 'xop'
        select = tokens[1][4:]
        key['map'] = int(select, 2) if re.match(r'^[01]{5}$', select) else int(select, 16)
        fields = tokens[2].split('.')
        if len(fields) != 4:
            return None
        key['w'] = {'0': 0, '1': 1}.get(fields[0])
        key['l'] = {'0': 0, '1': 1}.get(fields[2])
        key['prefix'] = {'00': 'none', '01': '66', '10': 'F3', '11': 'F2'}.get(fields[3])
        if key['prefix'] is None or not HEX.match(tokens[3].upper()):
            return None
        key['opcode'] = int(tokens[3], 16)
        rest = tokens[4:]
    else:
        hexes = []
        rest = []
        for n, t in enumerate(tokens):
            if t in ('66', 'F2', 'F3') and not hexes:
                key['prefix'] = t
            elif t == '(W0)':
                key['w'] = 0
            elif t == '(W1)':
                key['w'] = 1
            elif HEX.match(t) and (not hexes or hexes[-1] in ('0F', '38', '3A')) and len(hexes) < 3:
                if hexes == ['0F'] and t not in ('38', '3A'):
                    hexes.append(t)
                    rest = tokens[n + 1:]
                    break
                hexes.append(t)
                if hexes[0] != '0F':
                    rest = tokens[n + 1:]
                    break
            else:
                return None
        if not hexes or hexes[0] == '0F' and len(hexes) < 2:
            return None
        if hexes[0] == '0F':
            key['map'] = 1 if len(hexes) == 2 else 2 if hexes[1] == '38' else 3
            key['opcode'] = int(hexes[-1], 16)
        else:
            key['opcode'] = int(hexes[0], 16)
    if not rest or not re.match(r'^/(r|[0-7])$', rest[0]):
        return None
    if rest[0] != '/r':
        key['reg'] = int(rest[0][1])
    return key


SPACES = {'legacy': 0x00, 'vex': 0x10, 'evex': 0x20, 'xop': 0x30}
PREFIXES = {'none': 'NONE', '66': '66', 'F3': 'F3', 'F2': 'F2', None: 'ANY'}


def matches(entry_value, value):
    return entry_value is None or value is None or entry_value == value


def covers(f, g):
    # Whether some instruction is both form f and form g.
    return (f['space'] == g['space'] and f['map'] == g['map'] and f['opcode'] == g['opcode']
            and matches(f['prefix'], g['prefix']) and matches(f['reg'], g['reg'])
            and matches(f['w'], g['w']) and matches(f['l'], g['l']) and f['modes'] & g['modes'])


def size_rule(specs, mode):
    # The size of a group of rows that differ in their operand size only:
    # (kind, bytes), or None where they do not agree on one.
    kinds = {s[0] for s in specs}
    sizes = {s[1] for s in specs if len(s) > 1}
    if len(kinds) != 1:
        return None
    kind = kinds.pop()
    if kind in ('env', 'state', 'unknown', 'none'):
        return (kind, 0)
    if len(sizes) == 1:
        size = sizes.pop()
        return ('far', size) if kind == 'far' and size == 10 else ('n', size)
    if kind == 'n' and sizes == {2, 4}:
        return ('operand', 0)
    if kind == 'n' and sizes == {2, 8} and mode == '64':
        return ('stack', 0)
    if kind == 'far' and sizes == {4, 6}:
        return ('far', 0)
    if kind == 'pair' and sizes == {4, 8}:
        return ('pair', 0)
    return None


def legacy_rule(rows, mode, problems):
    # REX.W forms are 64-bit operand size forms of the others, or other
    # instructions: split by W then.
    w0 = [r['spec'] for r in rows if r['w'] is None or r['w'] == 0]
    w1 = [r['spec'] for r in rows if r['w'] == 1]
    r0 = size_rule(w0, mode) if w0 else None
    r1 = size_rule(w1, mode) if w1 else None
    if (w0 and r0 is None) or (w1 and r1 is None):
        problems.append((rows[0]['mnemonic'], mode, 'sizes do not agree', w0, w1))
        return []
    if not w1:
        return [(None, r0)]
    if not w0:
        return [(1, r1)]
    if r0 == r1 or (r0[0] == 'operand' and r1 == ('n', 8)) or (r0[0] == 'far' and r1 == ('far', 10)):
        return [(None, r0)]
    return [(0, r0), (1, r1)]


def merge(forms, problems):
    # Entries: forms of one opcode that only differ in their operand size,
    # vector length, or mode, merged where their sizes follow a rule.
    entries = []
    groups = {}
    for f in forms:
        k = (f['space'], f['map'], f['opcode'], f['prefix'], f['reg'], f['masked'], f['bcst'] != 0)
        groups.setdefault(k, []).append(f)
    for k, rows in groups.items():
        per_mode = {}
        for mode in ('64', '32'):
            mode_rows = [r for r in rows if mode in r['modes']]
            if not mode_rows:
                continue
            # One instruction the SDM spells two ways, as XCHG's operands
            # either way round, with the access only one of them gives.
            access = ''.join(sorted(set(''.join(r['access'] for r in mode_rows))))
            if k[0] == 'legacy':
                rules = [(w, None, rule, 0, 0) for w, rule in legacy_rule(mode_rows, mode, problems)]
            else:
                # A W1 form only 64-bit mode has: elsewhere W1 is ignored,
                # the W0 form's (SDM 325462-092 Vol. 2A, ANDN: "VEX.W1 is
                # ignored in non-64-bit modes").
                if mode == '32' and all(r['w'] != 1 for r in mode_rows) \
                        and any(r['w'] == 1 and r['modes'] == frozenset({'64'}) for r in rows):
                    mode_rows = [dict(r, w=None) for r in mode_rows]
                rules = vector_rules(mode_rows, problems)
            per_mode[mode] = (access, rules, mode_rows[0])
        if per_mode.get('64', (0, 0))[:2] == per_mode.get('32', (0, 0))[:2]:
            per_mode = {'all': per_mode.get('64')}
        for mode, (access, rules, first) in per_mode.items():
            for w, l, rule, bcst, scale in rules:
                # The form to name the entry by.
                first = next((r for r in rows if (w is None or r['w'] == w or (w == 0 and r['w'] is None))
                              and (l is None or r['l'] == l)), first)
                entries.append({
                    'space': k[0], 'map': k[1], 'opcode': k[2], 'prefix': k[3], 'reg': k[4], 'w': w, 'l': l,
                    'modes': {'all': frozenset({'64', '32'}), '64': frozenset({'64'}),
                              '32': frozenset({'32'})}[mode],
                    'access': access, 'masked': k[5], 'kind': rule[0], 'size': rule[1], 'bcst': bcst,
                    'scale': scale, 'mnemonic': first['mnemonic'], 'syntax': first['syntax'],
                    'source': first['source']})
    return entries


def vector_rules(rows, problems):
    # VEX, EVEX, and XOP forms name their W and L: one size each, which may
    # double with each step of L.
    cells = {}
    for r in rows:
        cell = (r['w'], r['l'])
        value = (r['spec'], r['bcst'], r['scale'])
        if cells.setdefault(cell, value) != value:
            problems.append((r['mnemonic'], 'cell', cell, cells[cell], value))
    rules = []
    by_w = {}
    for (w, l), value in cells.items():
        by_w.setdefault(w, {})[l] = value
    merged = {}
    for w, by_l in by_w.items():
        specs = {v[0] for v in by_l.values()}
        bcsts = {v[1] for v in by_l.values()}
        scales = {v[2] for v in by_l.values()}
        lengths = sorted(l for l in by_l if l is not None)
        if None not in by_l and len(lengths) > 1 and len(bcsts) == 1 and len(scales) == 1 \
                and all(s[0] == 'n' for s in specs):
            base = by_l[lengths[0]][0][1] >> lengths[0]
            if all(by_l[l][0][1] == base << l for l in lengths):
                merged[w] = [(None, ('vector', base), bcsts.pop(), scales.pop())]
                continue
        merged[w] = [(l, v[0] if v[0][0] != 'pair' else ('n', v[0][1]), v[1], v[2]) for l, v in by_l.items()]
    if 0 in merged and 1 in merged and merged[0] == merged[1]:
        merged = {None: merged[0]}
    for w, items in merged.items():
        for l, spec, bcst, scale in items:
            kind = spec[0]
            rules.append((w, l, (kind, spec[1] if len(spec) > 1 else 0), bcst, scale))
    return sorted(rules, key=lambda r: (r[0] is None, r[0] or 0, r[1] is None, r[1] or 0))


def specificity(e):
    return (e['prefix'] is None, e['reg'] is None, e['w'] is None, e['l'] is None, len(e['modes']))


def emit(entries, path):
    entries.sort(key=lambda e: (SPACES[e['space']] | e['map'], e['opcode']) + specificity(e))
    out = []
    out.extend(NOTICE + [''])
    out.append('// Generated by tools/insn-mem-table.py from the Intel SDM 325462-092, Vols. 2 and 3C,')
    out.append('// and the AMD APM 40332 rev. 4.10, Vols. 3-5. Do not edit.')
    out.append('')
    out.append('#include <stdint.h>')
    out.append('#include <virt/insn_mem_table.h>')
    out.append('')
    out.append('#define LEGACY INSN_SPACE_LEGACY')
    out.append('#define VEX INSN_SPACE_VEX')
    out.append('#define EVEX INSN_SPACE_EVEX')
    out.append('#define XOP INSN_SPACE_XOP')
    for name in ('NONE', '66', 'F3', 'F2', 'ANY'):
        out.append('#define P_%s INSN_PREFIX_%s' % (name, name))
    out.append('#define R_ANY INSN_REG_ANY')
    out.append('#define W_ANY INSN_W_ANY')
    out.append('#define L_ANY INSN_L_ANY')
    out.append('#define M_64 INSN_MODE_64')
    out.append('#define M_OTHER INSN_MODE_OTHER')
    out.append('#define M_ALL (INSN_MODE_64 | INSN_MODE_OTHER)')
    out.append('#define R INSN_MEM_READ')
    out.append('#define W INSN_MEM_WRITE')
    out.append('#define RW (INSN_MEM_READ | INSN_MEM_WRITE)')
    out.append('#define RM (INSN_MEM_READ | INSN_MEM_MASKED)')
    out.append('#define WM (INSN_MEM_WRITE | INSN_MEM_MASKED)')
    for kind in ('FIXED', 'VECTOR', 'OPERAND', 'STACK', 'FAR', 'PAIR', 'FPU_ENV', 'FPU_STATE', 'NONE',
                 'UNKNOWN'):
        out.append('#define S_%s INSN_SIZE_%s' % (kind, kind))
    out.append('')
    out.append('const struct insn_mem_form insn_mem_forms[] = {')
    kinds = {'n': 'S_FIXED', 'vector': 'S_VECTOR', 'operand': 'S_OPERAND', 'stack': 'S_STACK', 'far': 'S_FAR',
             'pair': 'S_PAIR', 'env': 'S_FPU_ENV', 'state': 'S_FPU_STATE', 'none': 'S_NONE',
             'unknown': 'S_UNKNOWN'}
    for e in entries:
        size = e['size']
        kind = kinds[e['kind']]
        if e['kind'] == 'far' and size == 10:
            kind = 'S_FIXED'
        access = {'r': 'R', 'w': 'W', 'rw': 'RW', '': '0'}[e['access']]
        if e['masked'] and access in ('R', 'W'):
            access += 'M'
        modes = {frozenset({'64', '32'}): 'M_ALL', frozenset({'64'}): 'M_64', frozenset({'32'}): 'M_OTHER'}
        out.append('    {%s | %d, 0x%02x, P_%s, %s, %s, %s, %s, %s, %s, %d, %d, %d}, // %s' % (
            e['space'].upper(), e['map'], e['opcode'], PREFIXES[e['prefix']],
            'R_ANY' if e['reg'] is None else str(e['reg']), 'W_ANY' if e['w'] is None else str(e['w']),
            'L_ANY' if e['l'] is None else str(e['l']), modes[e['modes']], access, kind,
            size if kind in ('S_FIXED', 'S_VECTOR') else 0, e['bcst'], e['scale'], e['syntax']))
    out.append('};')
    out.append('')
    out.append('const unsigned insn_mem_form_count = sizeof(insn_mem_forms) / sizeof(insn_mem_forms[0]);')
    open(path, 'w').write('\n'.join(out) + '\n')


def main():
    problems = []
    sdm = sdm_forms(sys.argv[1], problems)
    apm = apm_forms(sys.argv[2])
    # The APM adds the instructions the SDM does not have; where both have a
    # form, the SDM's operand encoding tables say more.
    extra = [g for g in apm if amd_only(g) and not any(covers(f, g) for f in sdm)]
    entries = merge(sdm + extra, problems)
    emit(entries, sys.argv[3])
    print('SDM forms: %d, AMD-only forms: %d, entries: %d, problems: %d'
          % (len(sdm), len(extra), len(entries), len(problems)))
    if len(sys.argv) > 4:
        for p in problems:
            print('PROBLEM', p)
        for c in CORRECTED:
            print('CORRECTED', c)
        # Where both manuals have a form, their sizes should agree.
        for g in apm:
            if g in extra or g['spec'][0] != 'n':
                continue
            matching = [f for f in sdm if covers(f, g)]
            if matching and not any(f['spec'] == g['spec'] or f['spec'][0] not in ('n', 'pair')
                                    or (g['bcst'] and f['bcst'] == g['bcst']) for f in matching):
                print('DIFFER', g['syntax'], g['spec'], '| SDM:',
                      sorted({(f['syntax'], f['spec']) for f in matching}))
        for g in extra:
            print('AMD', g['syntax'], g['space'], g['map'], '%02X' % g['opcode'], g['prefix'], g['reg'],
                  g['w'], g['l'], g['spec'])


if __name__ == '__main__':
    main()
