# Writes core/config_template.h: the commented-out config.txt that core creates, listing every mod and option this
# platform and edition has with its default. Keys and defaults come from the cfg*() calls in the sources, the
# descriptions from the mods' header comments and README's tables (private/README.md's for the private mods).
# With private/ in the tree it holds both editions (docs/editions.md), the public one under RBM_PUBLIC; without it
# (an exported public tree) only the public one.
#   python3 -I tools/build/config_gen.py <repo> > core/config_template.h
import sys, os, re, glob, textwrap
repo = sys.argv[1]
HAS_PRIVATE = os.path.isdir(os.path.join(repo, 'private'))
# Options of public mods that only some editions have (docs/editions.md).
FULL_ONLY_OPTS = set()

# Not settings: written or cleared by the mods themselves.
SKIP = {'analyze_playlist', 'flx4_inject', 'flx4_dump'}
# Defaults that aren't a plain literal in the call, or differ per platform.
DEFAULTS = {'analyze_timeout': {'mac': '90', 'win': '180'}, 'bettergrid_wait': {'mac': '20', 'win': '60'}}
GPU = {'mac': "0 keeps bettergrid on the CPU (Intel Macs: stems come from rekordbox's own engine then)", 'win': "0 keeps the models off the GPU: Beat This! and the genre model run on the CPU, stems come from rekordbox's own engine"}
# README's first sentence says too little for these.
SUMMARY = {'badge': "The PLATENDOOS chip: its window shows the versions, every mod and whether it's on, and opens the log and this file.",
           'bettergrid': "Better beat grids: combines the Beat This! beat tracker's tempo and bar starts with beats placed on the hits in the audio.",
           'spotifystart': 'Fixes Spotify hanging on "loading the library" after a restart. Only does anything on rekordbox 7.2.19 for Windows.',
           'streamio': 'Reads the audio faster during analysis: about 10% faster overall and 4x faster phrase analysis.'}
EXTRA = {'bettergrid_apply': "0: compare only, log both grids and keep rekordbox's",
         'bettergrid_debug': '1 logs the beats of both grids (for debugging)',
         'bettergrid_title': 'header name of the column',
         'betterkey_title': 'header name of the column',
         'keycol_title': 'header name of the column',
         'flx4_meterlog': '1 logs the level meters (for debugging)',
         'flx4_mixer': 'fixed keeps the mixer channels on decks 1/2',
         'flx4_device': 'MIDI device to map (for testing with another device using the same mapping)',
         'flx4_log': '1 logs every MIDI message in and out',
         'framerate_idle_after': 'seconds without input before the UI drops to 30 fps',
         'framerate_empty': "0 keeps rekordbox's 30 fps while the decks are empty, also while you use the mouse",
         'relayout_log': '1 logs how many buttons kept their images in a layout pass (for debugging)',
         'relayout_hidden': "0 lays out and restyles hidden panels every time, as rekordbox does",
         'relayout_check': '1 renders every button anyway and logs those whose images differ from the ones it would have kept (for testing)',
         'idlewake_midi': '0: the MIDI output thread wakes every millisecond again',
         'idlewake_meters': '0: level meters restart their timer on every frame again',
         'idlewake_envelope': "0: the envelope followers of silent channels run again (they feed the Color FX gate / comp)",
         'idlewake_check': '1 runs the envelope followers anyway and logs whether their output changed (for testing)',
         'idlewake_log': '1 logs every 10 s what was left out (for debugging)',
         'quickdeck_progressive': "an unfinished experiment (macOS): 1 lets a Spotify track's analysis start reading while the track downloads, so its overview waveform grows during the download; not proven safe (the one time it ran, Spotify signed rekordbox out for over an hour right after): leave it off",
         'quickdeck_stream': '0: the overview waveform appears when the whole track has been read, as in rekordbox',
         'quickdeck_log': '1 logs each partial overview (for debugging)',
         'audiomem_fx': "0: the FX history buffers aren't kept in memory (restart)",
         'audiomem_decks': "0: a playing deck's audio isn't kept ready ahead of the playhead (restart)",
         'audiomem_code': "0: rekordbox's own code and data aren't kept in memory (restart)",
         'audiomem_lazy': "0: a deck load zeroes its new buffers at once on the main thread, as rekordbox does (restart)",
         'tfpool_warmup': '0 skips the warm-up of the beat models at launch',
         'autocue_claude_path': "the claude program, if it isn't found by itself",
         'quickdeck_gridonly': '0: a new beat grid builds the deck\'s waveforms again, as rekordbox does',
         'bettergrid_offbeat': "rekordbox's beats a fixed 60 ms or more off the kicks: 1 the combined grid when Beat This! and the kick band both say so, 0 never",
         'quickframe_drawables': "frames queued for the display: 2, or rekordbox's own 3",
         'quickframe_playing': '1 shows frames as they are made while a deck plays too (quicker clicks; a late frame then shows as a hitch in the waveform)',
         'quickframe_stopped': "0 keeps the display's rhythm while no deck plays too",
         'artcache_mb': 'memory cache size in MB',
         'fastwave_ms': "wait before rebuilding a waveform (rekordbox's own is 500)",
         'spotifyapi_slots': 'Spotify lists loaded at once (applies from the next launch)',
         'stemcache_dir': 'folder of the cached stems (empty: the default one)',
         'streamio_phrase': '0 reads the audio again for phrase analysis',
         }

# For the settings window (the badge mod): options whose default is 0 or 1 are on/off switches, except these numbers.
NUMBERS = {'analyze_live', 'autocue_build_bars', 'autocue_outro_bars', 'betterkey_min_conf', 'betterstems_live_rest', 'outputsync_offset'}
# Options with a fixed set of values, in the order shown ('' is the empty value).
CHOICES = {'autocue_intro': 'sound|drums|start', 'autocue_provider': 'openrouter|claude|none', 'betterkey_mode': 'compare|replace',
           'genres_detail': 'style|broad', 'stemcache_staging': 'auto|1|0', 'stemcache_keep': 'all|chosen|none',
           'stemcache_quality': 'lossless|256|192|128', 'quickframe_drawables': '2|3', 'flx4_mixer': '|fixed',
           'autocue_build_bars': '0|4|8|16|32', 'autocue_outro_bars': '0|16|32|64',
           }

def modname(f):
    m = os.path.basename(f).split('.')[0]
    for suf in ('_panel', '_mac', '_win', '_ort', '_view', '_overlay', '_backend', '_art', '_chip', '_model'):
        if m.endswith(suf): m = m[:-len(suf)]
    return m

def files(plat, edition):
    out = []
    for d in ('core', 'mods', 'engines') + (('private/mods',) if edition == 'full' else ()):
        for f in sorted(glob.glob(os.path.join(repo, d, '*'))):
            if f.endswith('config_template.h') or not f.endswith(('.cpp', '.mm', '.h')): continue
            if plat == 'win' and (f.endswith('.mm') or '_mac.' in f): continue
            if plat == 'mac' and ('_win.' in f or '_ort.' in f or f.endswith('engines/ort.cpp')): continue
            out.append(f)
    return out

def default_of(text, i):  # text[i] is just after the key's closing quote
    m = re.match(r'\s*,\s*', text[i:])
    if not m: return ''
    i += m.end(); depth = 0; start = i
    while i < len(text):
        c = text[i]
        if c == '"':
            i += 1
            while text[i] != '"': i += 2 if text[i] == '\\' else 1
        elif c == '(': depth += 1
        elif c == ')':
            if depth == 0: break
            depth -= 1
        elif c == ',' and depth == 0: break
        i += 1
    return text[start:i].strip().strip('"')

def summaries(edition):  # README.md's rows; the full edition's own (private/README.md) over them
    readmes = [os.path.join(repo, 'README.md')] + ([os.path.join(repo, 'private', 'README.md')] if edition == 'full' else [])
    readme = '\n'.join(open(f, encoding='utf-8').read() for f in readmes if os.path.exists(f))
    summary = {}
    for name, text in re.findall(r'^\| \*\*(\w+)\*\* \| (.*?) \|\s*$', readme, re.M):
        s = re.sub(r'\*\*?|`', '', text)
        m = re.match(r'(.+?(?<!e\.g)[.!?])(\s|$)', s)
        summary[name] = (m.group(1) if m else s).strip()
    summary.setdefault('flx4', 'DDJ-FLX4 as a four-deck controller with stems.')
    summary.update(SUMMARY)
    return summary
# The public edition's description where a mod's header comment describes both editions.
PUBLIC_DOCS = {
    'analyze_min_free_gb': 'stop a run when the disk has less free space than this, in GB (at most 1: a run writes only rekordbox\'s analysis files)',
}

def build(plat, edition):
    mods, opts, docs = set(), {}, {}
    summary = summaries(edition)
    fs = files(plat, edition)
    for f in fs:
        t = open(f, encoding='utf-8', errors='replace').read()
        winOnly = re.search(r'^//\s*windows-only\s*$', t, re.M)
        if '/mods/' in f and not (plat == 'mac' and winOnly) and modname(f) in summary: mods.add(modname(f))
    for f in fs:
        t = open(f, encoding='utf-8', errors='replace').read()
        if re.search(r'^//\s*windows-only\s*$', t, re.M) and plat == 'mac': continue
        mod = modname(f)
        for m in re.finditer(r'\bcfg(?:Int|Str|Num)\(\s*"(\w+)"', t):
            k = m[1]
            if k in SKIP or k in summary or k in mods or (edition == 'public' and k in FULL_ONLY_OPTS): continue
            opts.setdefault(mod, {}).setdefault(k, default_of(t, m.end()))
        cur = None
        for line in t.split('\n'):
            if not line.startswith('//'): cur = None; continue
            b = re.sub(r'^//\s*(config:\s*)?', '', line)
            m = re.match(r'(\w+)=(\S+)\s\s+(.*)', b)
            if m: docs[m[1]] = m[3]; cur = m[1]
            elif cur and line.startswith('//     ') and '=' not in b.split(' ')[0]: docs[cur] += ' ' + b.strip()
            else: cur = None
    out = ['# platendoos options',
           '# Every line starting with # is off. Remove the # to use a line, then change its value.',
           '# <mod>=0 turns a mod off (<mod>=1 on); that applies after a restart of rekordbox. The values shown are the defaults. Every other option is re-read every few seconds.', '']
    listed = set()  # an option several mods read is shown once
    for mod in sorted(mods | set(opts)):
        if mod not in mods: continue
        out.append('# ---- ' + mod + ' ----')
        out += ['# ' + l for l in textwrap.wrap(summary[mod], 96)]
        out.append('#%s=1' % mod)
        for k, d in sorted(opts.get(mod, {}).items()):
            if k in listed: continue
            listed.add(k)
            d = DEFAULTS.get(k, {}).get(plat, d)
            desc = GPU[plat] if k == 'gpu' else EXTRA.get(k, docs.get(k, ''))
            if edition == 'public': desc = PUBLIC_DOCS.get(k, desc)
            desc = re.sub(r'\s+', ' ', desc).strip()
            out += ['#   ' + l for l in textwrap.wrap(desc, 94)]
            out.append('#%s=%s' % (k, d))
        out.append('')
    return '\n'.join(out)

editions = ('full', 'public') if HAS_PRIVATE else ('public',)
def kinds(e):
    """The switches and the choices among the options an edition lists on either platform: ' a b c ' and 'k=x|y;k2=...;'."""
    keys = {}
    if True:
        for plat in ('mac', 'win'):
            mod = None
            for line in build(plat, e).split('\n'):
                m = re.match(r'# ---- (\w+) ----', line)
                if m: mod = m[1]; continue
                m = re.match(r'#(\w+)=(.*)$', line)
                if m and m[1] != mod: keys.setdefault(m[1], m[2])
    switches = sorted(k for k, d in keys.items() if d in ('0', '1') and k not in NUMBERS and k not in CHOICES)
    choices = ''.join('%s=%s;' % (k, v) for k, v in sorted(CHOICES.items()) if k in keys)
    return ' ' + ' '.join(switches) + ' ', choices

for e in editions:
    for k in ('mac', 'win'):
        assert ')__"' not in build(k, e)
def emit(e):
    print('#ifdef _WIN32')
    print('static const char kConfigTemplate[] = R"__(%s)__";' % build('win', e))
    print('#else')
    print('static const char kConfigTemplate[] = R"__(%s)__";' % build('mac', e))
    print('#endif')
    # For the settings window: which options are on/off switches, and which have a fixed set of values.
    switches, choices = kinds(e)
    print('static const char kConfigSwitches[] = "%s";' % switches)
    print('static const char kConfigChoices[] = "%s";' % choices)
print('// generated by tools/build/config_gen.py')
print('#pragma once')
if HAS_PRIVATE:
    print('#ifdef RBM_PUBLIC')
    emit('public')
    print('#else')
    emit('full')
    print('#endif')
else:
    emit('public')

