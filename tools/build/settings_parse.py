# Reads the mods' settings from the sources: the `const Setting name{"key", default, {.label = ..., ...}};` declarations
# (core/settings.h) and each mod's RBM_MOD({.name, .title, .about, .area}). For what is made at build time from them:
# the docs' tables (website/scripts/settings_gen.py). At run time the core has the same from the objects themselves.
#   from settings_parse import mods          mods(repo, platform, edition) -> [Mod]
#   python3 -I tools/build/settings_parse.py <repo> [mac|win] [public|full]    prints the config file's list
#   python3 -I tools/build/settings_parse.py --check <repo>    fails (exit 1, saying where) on a declaration this can't
#                                                               read as the build does; compile.sh runs it
# A declaration must be a plain initializer (literals only). `#if ... _WIN32 ...` / `#else` around one picks the
# platform's (RBM_MAC_X86 counts as off: the docs describe Apple Silicon); About::platform and the file's name
# (x_mac.*, *.mm: macOS; x_win.*, x_ort.*: Windows) do the rest.
import glob, os, re, sys

SUFFIXES = ('_panel', '_mac', '_win', '_ort', '_view', '_overlay', '_backend', '_art', '_chip', '_model')


class Setting:
    def __init__(s, key, default, about, mod):
        s.key, s.default, s.about, s.mod = key, default, about, mod

    def text(s):
        a, t = s.about, s.about.get('label', '')
        if a.get('help'): t += ('. ' if t else '') + a['help']
        if a.get('choices'): t += ' (' + '; '.join('%s: %s' % (v or 'empty', l) for v, l in a['choices']) + ')'
        elif s.default in ('true', 'false'): t += ' (1 on, 0 off)'
        if a.get('unit'): t += ' [%s]' % a['unit']
        if a.get('restart'): t += ' (applies after a restart)'
        return t

    def default_text(s):
        return {'true': '1', 'false': '0'}.get(s.default, s.default)


class Mod:
    def __init__(m, name): m.name, m.title, m.about, m.area, m.untested, m.settings = name, '', '', '', '', []


class ParseError(Exception):
    pass


def where(repo, f, t, pos):
    return '%s:%d' % (os.path.relpath(f, repo), t.count('\n', 0, pos) + 1)


def modof(f):
    if os.sep + 'core' + os.sep in os.sep + f: return 'core'
    n = os.path.basename(f).split('.')[0]
    for suf in SUFFIXES:
        if n.endswith(suf): n = n[:-len(suf)]
    return n


def sources(repo, platform, edition):
    out = []
    for d in ('core', 'mods', 'engines') + (('private/mods',) if edition == 'full' else ()):
        for f in sorted(glob.glob(os.path.join(repo, d, '*'))):
            if not f.endswith(('.cpp', '.mm', '.h')): continue
            if platform == 'win' and (f.endswith('.mm') or '_mac.' in f): continue
            if platform == 'mac' and ('_win.' in f or '_ort.' in f or f.endswith(os.path.join('engines', 'ort.cpp'))): continue
            out.append(f)
    return out


TOKEN = re.compile(r'\s*(?:("(?:[^"\\]|\\.)*")|(-?\d+(?:\.\d+)?)|([A-Za-z_][\w:]*)|(.))', re.S)


def tokens(text):
    out, i = [], 0
    while i < len(text):
        m = TOKEN.match(text, i)
        if not m: break
        i = m.end()
        if m.group(1) is not None:
            s = bytes(m.group(1)[1:-1], 'utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8')
            if out and out[-1][0] == 'str': out[-1] = ('str', out[-1][1] + s)  # adjacent literals are one
            else: out.append(('str', s))
        elif m.group(2) is not None: out.append(('num', m.group(2)))
        elif m.group(3) is not None: out.append(('id', m.group(3)))
        elif m.group(4).strip(): out.append(('op', m.group(4)))
    return out


def value(toks, i):
    """One initializer value from toks[i]: a literal, or a braced list of values -> (value, next index)."""
    kind, v = toks[i]
    if kind == 'op' and v == '{':
        items, i = [], i + 1
        while toks[i] != ('op', '}'):
            if toks[i] == ('op', ','): i += 1; continue
            x, i = value(toks, i)
            items.append(x)
        return items, i + 1
    if kind == 'id': return {'true': True, 'false': False}.get(v, v), i + 1
    return v, i + 1


def about_of(toks):
    """{.label = "...", .min = 0, ...} -> dict"""
    out, i = {}, 1
    while i < len(toks) and toks[i] != ('op', '}'):
        if toks[i] == ('op', ','): i += 1; continue
        assert toks[i] == ('op', '.'), toks[i:i + 4]
        name = toks[i + 1][1]
        assert toks[i + 2] == ('op', '=')
        out[name], i = value(toks, i + 3)
    return out


def balanced(text, i):
    """text[i] == '{' -> the index after its matching '}' (strings skipped)."""
    depth = 0
    while True:
        c = text[i]
        if c == '"':
            i += 1
            while text[i] != '"': i += 2 if text[i] == '\\' else 1
        elif c == '{': depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0: return i + 1
        i += 1


def active(text, pos, platform):
    """Whether the preprocessor lines before pos leave it in for this platform."""
    stack = []
    for m in re.finditer(r'^[ \t]*#[ \t]*(ifdef|ifndef|if|elif|else|endif)\b([^\n]*)', text[:pos], re.M):
        d, cond = m.group(1), m.group(2)
        if d in ('if', 'ifdef', 'ifndef'):
            if '_WIN32' in cond:
                on = platform == 'win'
                if d == 'ifndef' or re.search(r'!\s*defined\s*\(\s*_WIN32', cond): on = not on
            else: on = None  # (not about the platform: both branches count)
            stack.append(on)
        elif d == 'else' and stack:
            if stack[-1] is not None: stack[-1] = not stack[-1]
        elif d == 'endif' and stack: stack.pop()
    return all(x is not False for x in stack)


def mods(repo, platform, edition):
    found, order = {}, []
    declared = set()
    files = sources(repo, platform, edition)
    texts = {f: open(f, encoding='utf-8', errors='replace').read() for f in files}
    for f, t in texts.items():
        for m in re.finditer(r'RBM_MOD\(\{\s*\.name\s*=\s*"(\w+)"', t):
            if '//' in t[t.rfind('\n', 0, m.start()) + 1:m.start()]: continue
            mod = found.setdefault(m.group(1), Mod(m.group(1)))
            end = balanced(t, t.index('{', m.start()))
            body = t[m.start():end]
            for field in ('title', 'about', 'area', 'untested'):
                fm = re.search(r'\.%s\s*=\s*((?:"(?:[^"\\]|\\.)*"\s*)+)' % field, body)
                if fm: setattr(mod, field, ''.join(x[1] for x in tokens(fm.group(1))))
    for f, t in texts.items():
        code = re.sub(r'//[^\n]*', lambda c: ' ' * len(c.group(0)), t)  # (comments: same length, so positions hold)
        strict = {m.start() for m in re.finditer(r'^[ \t]*(?:static\s+)?const\s+(?:rbm::)?Setting\s+\w+\s*\{', code, re.M)}
        for m in re.finditer(r'^[^\n]*\bSetting\s+\w+\s*[({=]', code, re.M):  # every other way to make one is unreadable here
            if m.start() not in strict and not re.match(r'\s*(?:Setting\s*\(|.*\bSetting\s*\(const char\*)', m.group(0)):
                raise ParseError('%s: a Setting declared in a way settings_parse.py can\'t read; write `const Setting name{"key", default, {...}};`' % where(repo, f, t, m.start()))
        for m in re.finditer(r'^[ \t]*(?:static\s+)?const\s+(?:rbm::)?Setting\s+\w+\s*\{', code, re.M):
            if not active(t, m.start(), platform): continue
            end = balanced(t, m.end() - 1)
            toks = tokens(t[m.end() - 1:end])
            try:
                if toks[0] != ('op', '{') or toks[1][0] != 'str' or toks[2] != ('op', ',') or toks[4] != ('op', ',') or toks[5] != ('op', '{'):
                    raise ParseError('not {"key", default, {...}}')
                if toks[3][0] == 'id' and toks[3][1] not in ('true', 'false'): raise ParseError('the default is a name, not a literal')
                key, default = toks[1][1], toks[3][1]
                about = about_of(toks[5:])
                bad = [k for k, v in about.items() if isinstance(v, str) and v and not any(x == ('str', v) or x == ('num', v) for x in toks)]
                if bad: raise ParseError('not a literal: ' + ', '.join('.' + k for k in bad))
            except (ParseError, AssertionError, IndexError) as e:
                raise ParseError('%s: %s' % (where(repo, f, t, m.start()), e or 'unreadable declaration'))
            if about.get('platform') and about['platform'] != platform: continue
            if key in declared: continue
            declared.add(key)
            name = about.get('page') or modof(os.path.relpath(f, repo))
            mod = found.setdefault(name, Mod(name))
            about['choices'] = [tuple(c) for c in about.get('choices', [])]
            mod.settings.append(Setting(key, default, about, name))
    return [found[n] for n in sorted(found)]


def wrap(out, text, first, rest):
    line = first
    for word in text.split(' '):
        if len(line) > len(first if not out or out[-1] != line else rest) and len(line) + 1 + len(word) > 100:
            out.append(line); line = rest
        line += (' ' if line not in (first, rest) else '') + word
    if line.strip('# '): out.append(line)


def off_unless_asked(repo, platform, edition='full'):
    """The mods that say they are untested on this platform (RBM_MOD .untested): off until the config asks for them."""
    return {m.name for m in mods(repo, platform, edition) if m.untested == platform}


def areas(repo, platform, edition):
    """The sidebar areas the mods declare (settings::Area): id -> (title, blurb, icon, order). Declarations of one id in
    several files must say the same."""
    out = {}
    for f in sources(repo, platform, edition):
        t = open(f, encoding='utf-8', errors='replace').read()
        code = re.sub(r'//[^\n]*', lambda c: ' ' * len(c.group(0)), t)
        for m in re.finditer(r'settings::Area\s+\w+\s*\{', code):
            toks = tokens(t[m.end() - 1:balanced(t, m.end() - 1)])
            vals = [x[1] for x in toks if x[0] in ('str', 'num')]
            if len(vals) != 5: raise ParseError('%s: an Area is {"id", "title", "blurb", "icon", order}' % where(repo, f, t, m.start()))
            aid, rest = vals[0], (vals[1], vals[2], vals[3], int(vals[4]))
            if aid in out and out[aid] != rest:
                raise ParseError('%s: area "%s" is declared differently elsewhere' % (where(repo, f, t, m.start()), aid))
            out[aid] = rest
    return out


def check(repo):
    """Everything the docs and the patcher read, for each platform and edition; the first problem raises."""
    editions = ('public', 'full') if os.path.isdir(os.path.join(repo, 'private')) else ('public',)
    for platform in ('mac', 'win'):
        for edition in editions:
            listed = mods(repo, platform, edition)
            known = {s.key for m in listed for s in m.settings}
            ids = areas(repo, platform, edition)
            for m in listed:
                if m.area and m.area not in ids and m.area != 'about':
                    raise ParseError('%s (%s, %s): area "%s" is declared by no mod' % (m.name, platform, edition, m.area))
                if m.untested and m.untested not in ('mac', 'win'): raise ParseError('%s: .untested is "mac" or "win"' % m.name)
                for s in m.settings:
                    need = s.about.get('needs', '')
                    k = re.split(r'!?=', need.lstrip('!'))[0]
                    if need and k not in known and platform == 'mac':
                        raise ParseError('%s: needs "%s", which no mod declares' % (s.key, k))


def config_list(repo, platform, edition):
    """The config file's commented list (as core's settings::configList writes it), one section per mod."""
    out = ['# platendoos options']
    listed = mods(repo, platform, edition)
    off = off_unless_asked(repo, platform)
    general = [s for m in listed if m.name in ('core', 'about') for s in m.settings]  # (the core's own: the About page)
    if general:
        out += ['', '# ---- general ----']
        for s in general:
            wrap(out, s.text(), '#   ', '#   ')
            out.append('#%s=%s' % (s.key, s.default_text()))
    for m in listed:
        if m.name in ('core', 'about'): continue
        out += ['', '# ---- %s ----' % m.name]
        what = m.title + (': ' if m.title and m.about else '') + m.about
        if what: wrap(out, what, '# ', '# ')
        out.append('#%s=%d' % (m.name, 0 if m.name in off else 1))
        for s in m.settings:
            wrap(out, s.text(), '#   ', '#   ')
            out.append('#%s=%s' % (s.key, s.default_text()))
    return '\n'.join(out) + '\n'


if __name__ == '__main__':
    if sys.argv[1] == '--check':
        try: check(sys.argv[2])
        except ParseError as e: sys.exit('settings_parse: %s' % e)
        sys.exit(0)
    repo = sys.argv[1]
    sys.stdout.write(config_list(repo, sys.argv[2] if len(sys.argv) > 2 else 'mac', sys.argv[3] if len(sys.argv) > 3 else 'full'))
