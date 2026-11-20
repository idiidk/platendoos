# Writes the "Every option" tables of docs/guides/settings.md from core/config_template.h, the commented-out
# config.txt the mods carry (made by tools/build/config_gen.py).
#
# The template holds both editions (docs/editions.md): the public one under #ifdef RBM_PUBLIC, the full one in the
# #else, each with a macOS and a Windows variant. The page gets the public tables; the full edition's tables go to
# private/website/slots/settings-options.md, which the full edition of the site shows in their place. In a tree
# without private/ the template has only the public edition, and only the page is written.
#   npm run settings        (from website/)
import os, re, sys

here = os.path.dirname(os.path.abspath(__file__))
repo = os.path.normpath(os.path.join(here, '..', '..'))
template = os.path.join(repo, 'core', 'config_template.h')
page = os.path.join(here, '..', 'docs', 'guides', 'settings.md')
slot = os.path.join(repo, 'private', 'website', 'slots', 'settings-options.md')
mod_pages = os.path.join(here, '..', 'docs', 'mods')
private_pages = os.path.join(repo, 'private', 'website', 'docs', 'mods')
# Options the template carries without a description.
EXTRA = {
    'audiomem_code': "`0` switches one part off: keeping rekordbox's own code and data in memory (restart).",
    'audiomem_decks': '`0` switches one part off: keeping the next seconds of every loaded deck ready (restart).',
    'audiomem_fx': '`0` switches one part off: pinning the FX history buffers (restart).',
    'audiomem_lazy': "`0` switches one part off: deck loads that don't clear the new buffers on the main thread (restart).",
    'autocue_claude_path': "the claude program, if it isn't found by itself",
    'tfpool_warmup': "`0` skips loading and running rekordbox's beat model once at launch (macOS)",
}
# Public summaries where the template's (from the README) still speaks of the full edition.
PUBLIC_SUMMARY = {
    'autocue': 'Sets hot cues A–H (intro, build, drop, breakdown, build 2, drop 2, outro, fake drop) on a playlist, '
               'Spotify tracks too.',
}
BEGIN, END = '<!--@public settings-options-->', '<!--@/public-->'


def variants(text):
    """{(edition, platform): template text}"""
    out, stack = {}, []
    lines = text.split('\n')
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith('#ifdef '):
            stack.append([line[7:].strip(), True]); i += 1; continue
        if line.startswith('#else'):
            stack[-1][1] = False; i += 1; continue
        if line.startswith('#endif'):
            stack.pop(); i += 1; continue
        if 'R"__(' in line:
            body = [line.split('R"__(', 1)[1]]
            i += 1
            while not lines[i].startswith(')__"'):
                body.append(lines[i]); i += 1
            conds = {name: on for name, on in stack}
            edition = 'public' if conds.get('RBM_PUBLIC', True) else 'full'
            platform = 'win' if conds.get('_WIN32', False) else 'mac'
            out[(edition, platform)] = '\n'.join(body)
        i += 1
    return out


def parse(text):
    """[(mod, summary, [(key, default, description)])] in the template's order"""
    mods, cur, desc = [], None, []
    for line in text.split('\n'):
        m = re.match(r'^# ---- (\w+) ----$', line)
        if m:
            cur = [m.group(1), [], []]; mods.append(cur); desc = []; continue
        if cur is None:
            continue
        m = re.match(r'^#(\w+)=(.*)$', line)
        if m:
            cur[2].append((m.group(1), m.group(2), ' '.join(desc))); desc = []; continue
        if line.startswith('#   '):
            desc.append(line[4:].strip())
        elif line.startswith('# ') and not cur[2]:
            cur[1].append(line[2:].strip())
    return [(n, ' '.join(s), opts) for n, s, opts in mods]


def cell(s):
    return s.replace('|', '\\|').replace('<', '&lt;')


def tables(edition, var):
    mac = parse(var[(edition, 'mac')]) if (edition, 'mac') in var else []
    win = parse(var[(edition, 'win')]) if (edition, 'win') in var else []
    names = sorted({m[0] for m in mac} | {m[0] for m in win})
    bymac, bywin = {m[0]: m for m in mac}, {m[0]: m for m in win}
    out = []
    for name in names:
        tag = ''
        if name in bymac and name not in bywin: tag = ' <span class="pd-tag">mac</span>'
        if name in bywin and name not in bymac: tag = ' <span class="pd-tag">win</span>'
        summary = (bymac.get(name) or bywin[name])[1]
        if edition == 'public' and name in PUBLIC_SUMMARY and 'cache' in summary:
            summary = PUBLIC_SUMMARY[name]
        has_page = os.path.exists(os.path.join(mod_pages, name + '.md')) or \
            (edition == 'full' and os.path.exists(os.path.join(private_pages, name + '.md')))
        more = f' [More →](/mods/{name})' if has_page else ''
        out += [f'### {name}{tag}', '', f'{summary}{more}', '', '| Option | Default | Meaning |', '|---|---|---|']
        mo = {k: (d, t) for k, d, t in (bymac[name][2] if name in bymac else [])}
        wo = {k: (d, t) for k, d, t in (bywin[name][2] if name in bywin else [])}
        keys = sorted(set(mo) | set(wo), key=lambda k: (k != name, k))
        for k in keys:
            if k == name:
                out.append(f'| `{k}` | `1` | `0` switches the mod off, `1` on. Applies after a restart of rekordbox. |')
                continue
            dm, dw = mo.get(k), wo.get(k)
            fmt = lambda d: f'`{d}`' if d != '' else '*(empty)*'
            if dm and dw and dm[0] != dw[0]:
                default = f'{fmt(dm[0])} (macOS), {fmt(dw[0])} (Windows)'
            else:
                default = fmt((dm or dw)[0])
            if dm and dw and dm[1] != dw[1] and dm[1] and dw[1]:
                desc = cell(f'macOS: {dm[1]}. Windows: {dw[1]}')
            else:
                desc = cell((dm or dw)[1] or (dw or dm)[1]) or EXTRA.get(k, '')
            if edition == 'public':
                desc = re.sub(r'^public edition: ', '', desc)
            both = bymac.get(name) and bywin.get(name)
            if both and not (dm and dw) and not re.match(r'(macOS|Windows)\b', desc):
                desc += ' (macOS only)' if dm else ' (Windows only)'
            out.append(f'| `{k}` | {default} | {desc} |')
        out.append('')
    return '\n'.join(out).rstrip('\n')


def main():
    var = variants(open(template, encoding='utf-8').read())
    text = open(page, encoding='utf-8').read()
    head = text[:text.index('### ', text.index('## Every option'))] if BEGIN not in text else text[:text.index(BEGIN)]
    public = tables('public', var)
    open(page, 'w', encoding='utf-8').write(f'{head}{BEGIN}\n{public}\n{END}\n')
    print(f'{os.path.relpath(page, repo)}: the public edition, {public.count("### ")} mods')
    if os.path.isdir(os.path.dirname(slot)) and ('full', 'mac') in var:
        full = tables('full', var)
        open(slot, 'w', encoding='utf-8').write(full + '\n')
        print(f'{os.path.relpath(slot, repo)}: the full edition, {full.count("### ")} mods')


if __name__ == '__main__':
    sys.exit(main())
