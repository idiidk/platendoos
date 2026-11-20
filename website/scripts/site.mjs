// Builds, serves or previews one edition of the site.
//
//   node scripts/site.mjs <dev|build|preview> [--private] [--trailer] [vitepress options…]
//
// The public edition is the default. --private (or DOCS_EDITION=private) is the full edition: it adds the pages,
// sections and pictures in ../private/website/, which only the full source tree has (tools/build/export_public.sh
// drops private/).
//
// VitePress reads its pages from an assembled copy, .site/<edition>/: docs/ with its edition markers resolved and,
// for the full edition, private/website/docs/ laid over it. The markers, each on a line of its own:
//
//   <!--@private NAME-->                  full edition: the contents of private/website/slots/NAME.md;
//                                         public edition: nothing
//   <!--@public NAME-->                   public edition: the lines in between;
//   …                                     full edition: private/website/slots/NAME.md instead
//   <!--@/public-->
//
// `dev` keeps the copy in step while it runs: edit docs/ or private/website/ as usual.
import { spawn } from 'node:child_process'
import {
  copyFileSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, watch, writeFileSync,
} from 'node:fs'
import { dirname, join, relative, sep } from 'node:path'
import { fileURLToPath } from 'node:url'

const site = join(dirname(fileURLToPath(import.meta.url)), '..')
const docs = join(site, 'docs')
const priv = join(site, '..', 'private', 'website')

const [cmd, ...rest] = process.argv.slice(2)
const flags = new Set(rest.filter((a) => a === '--private' || a === '--trailer'))
const passThrough = rest.filter((a) => !flags.has(a))
const edition = flags.has('--private') || ['private', 'full'].includes(process.env.DOCS_EDITION ?? '') ? 'private' : 'public'
const out = join(site, '.site', edition)

if (!['dev', 'build', 'preview'].includes(cmd)) {
  console.error('usage: node scripts/site.mjs <dev|build|preview> [--private] [--trailer] [vitepress options…]')
  process.exit(2)
}
if (edition === 'private' && !existsSync(priv)) {
  console.error(`The full edition needs ${relative(site, priv)}/, which this source tree doesn't have.`)
  process.exit(1)
}

// relative path -> absolute source, docs/ first, then the private pages over it
function sources() {
  const files = new Map()
  const walk = (root, dir = root) => {
    for (const e of readdirSync(dir, { withFileTypes: true })) {
      const p = join(dir, e.name)
      if (e.isDirectory()) {
        if (dir === root && e.name === '.vitepress') continue
        walk(root, p)
      } else if (!e.name.startsWith('.')) files.set(relative(root, p).split(sep).join('/'), p)
    }
  }
  walk(docs)
  if (edition === 'private' && existsSync(join(priv, 'docs'))) walk(join(priv, 'docs'))
  return files
}

function slot(name, where) {
  const f = join(priv, 'slots', `${name}.md`)
  if (!existsSync(f)) throw new Error(`${where}: no slot private/website/slots/${name}.md`)
  return readFileSync(f, 'utf8').replace(/\s+$/, '')
}

// Resolves the edition markers of one page.
export function resolveMarkers(text, where, ed = edition) {
  const lines = text.split('\n')
  const res = []
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i]
    let m = line.match(/^\s*<!--@private ([\w-]+)-->\s*$/)
    if (m) {
      if (ed === 'private') res.push(slot(m[1], where))
      continue
    }
    m = line.match(/^\s*<!--@public ([\w-]+)-->\s*$/)
    if (m) {
      const end = lines.findIndex((l, j) => j > i && /^\s*<!--@\/public-->\s*$/.test(l))
      if (end < 0) throw new Error(`${where}:${i + 1}: <!--@public ${m[1]}--> without <!--@/public-->`)
      if (ed === 'private') res.push(slot(m[1], where))
      else res.push(...lines.slice(i + 1, end))
      i = end
      continue
    }
    if (/<!--@\/?(private|public)\b/.test(line)) throw new Error(`${where}:${i + 1}: a marker must be on a line of its own`)
    res.push(line)
  }
  return res.join('\n')
}

function assemble() {
  const files = sources()
  if (flags.has('--trailer')) {
    const t = join(site, '..', 'trailer', 'platendoos-trailer.mp4')
    if (!existsSync(t)) throw new Error(`No trailer at ${t}: render it with trailer/render.py, or build without --trailer.`)
    files.set('public/platendoos-trailer.mp4', t)
  }
  let changed = 0
  for (const [rel, src] of files) {
    const dest = join(out, rel)
    mkdirSync(dirname(dest), { recursive: true })
    if (rel.endsWith('.md')) {
      const text = resolveMarkers(readFileSync(src, 'utf8'), rel)
      if (!existsSync(dest) || readFileSync(dest, 'utf8') !== text) { writeFileSync(dest, text); changed++ }
    } else {
      const s = statSync(src)
      if (!existsSync(dest) || statSync(dest).size !== s.size || statSync(dest).mtimeMs < s.mtimeMs) {
        copyFileSync(src, dest); changed++
      }
    }
  }
  // remove what has no source any more
  const prune = (dir) => {
    for (const e of readdirSync(dir, { withFileTypes: true })) {
      const p = join(dir, e.name)
      const rel = relative(out, p).split(sep).join('/')
      if (e.isDirectory()) {
        prune(p)
        if (readdirSync(p).length === 0) rmSync(p, { recursive: true })
      } else if (!files.has(rel)) { rmSync(p); changed++ }
    }
  }
  prune(out)
  return changed
}

mkdirSync(out, { recursive: true })
try {
  assemble()
} catch (e) {
  console.error(e.message)
  process.exit(1)
}

if (cmd === 'dev') {
  let timer
  const again = () => {
    clearTimeout(timer)
    timer = setTimeout(() => {
      try {
        const n = assemble()
        if (n) console.log(`[site] ${edition}: ${n} file(s) updated`)
      } catch (e) {
        console.error(`[site] ${e.message}`)
      }
    }, 120)
  }
  watch(docs, { recursive: true }, (_, f) => { if (!f || !String(f).startsWith('.vitepress')) again() })
  if (edition === 'private') watch(priv, { recursive: true }, again)
}

const vp = spawn(join(site, 'node_modules', '.bin', process.platform === 'win32' ? 'vitepress.cmd' : 'vitepress'),
  [cmd, 'docs', ...passThrough], {
    cwd: site, stdio: 'inherit', shell: process.platform === 'win32',
    env: { ...process.env, DOCS_EDITION: edition, ...(flags.has('--trailer') ? { PLATENDOOS_TRAILER: 'local' } : {}) },
  })
vp.on('exit', (code) => process.exit(code ?? 1))
for (const s of ['SIGINT', 'SIGTERM']) process.on(s, () => vp.kill(s))
