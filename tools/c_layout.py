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

# Lays out the C that the generators write as the style guide has it: each
# comment a sentence filled up to column 79, a trailing comment that would run
# past it on its own lines above its code instead, and no trailing comma after
# the last item of a multi-line list. The license notice keeps its own form.

import re
import textwrap

COMMENT_LIMIT = 79
NOTICE_END = '// with ReBIOS. If not, see <https://www.gnu.org/licenses/>.'


# The notice every generated file starts with.
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


def count_decl(name, array):
    """The declaration of an array's count, on one line where it fits in 99
    columns, else with its call broken after the opening parenthesis."""
    line = f'const size_t {name} = ARRAY_COUNT({array});'
    if len(line) <= 99:
        return [line]
    return [f'const size_t {name} = ARRAY_COUNT(', f'    {array}', ');']


def _sentence(text):
    return text if text[-1:] in '.?!' else text + '.'


def _fill(indent, text):
    return textwrap.wrap(text, COMMENT_LIMIT, initial_indent=indent + '// ',
                         subsequent_indent=indent + '// ', break_long_words=False,
                         break_on_hyphens=False)


def _split(line):
    """The code and the trailing comment of a line, the comment None where it
    has none. Generated code has no // inside its string literals."""
    at = line.find(' // ')
    if at < 0 or line.lstrip().startswith('//'):
        return line, None
    return line[:at], line[at + 4:]


def finish(text):
    lines = text.split('\n')
    start = next((i + 1 for i, line in enumerate(lines) if line == NOTICE_END), 0)
    out = lines[:start]
    i = start
    while i < len(lines):
        line = lines[i]
        stripped = line.lstrip()
        if stripped.startswith('// '):
            # A paragraph of comment lines: one sentence, filled.
            indent = line[:len(line) - len(stripped)]
            words = []
            while i < len(lines) and lines[i].startswith(indent + '// '):
                words.append(lines[i][len(indent) + 3:].strip())
                i += 1
            out += _fill(indent, _sentence(' '.join(words)))
            continue
        code, comment = _split(line)
        if comment is not None:
            comment = _sentence(comment.strip())
            if len(code) + 4 + len(comment) <= COMMENT_LIMIT:
                out.append(code + ' // ' + comment)
            else:
                indent = code[:len(code) - len(code.lstrip())]
                out += _fill(indent, comment) + [code]
        else:
            out.append(line)
        i += 1
    # The last item of a multi-line list: the next line with code closes it.
    for n, line in enumerate(out):
        if n < start:
            continue
        code, comment = _split(line)
        if not code.rstrip().endswith(','):
            continue
        k = n + 1
        while k < len(out) and out[k].lstrip().startswith('//'):
            k += 1
        if k == len(out):
            continue
        after = out[k]
        indent = len(code) - len(code.lstrip())
        if after.lstrip().startswith('}') and len(after) - len(after.lstrip()) < indent:
            code = code.rstrip()[:-1]
            out[n] = code if comment is None else code + ' // ' + comment
    return '\n'.join(out)
