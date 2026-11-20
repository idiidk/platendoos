// Fails when the public build (docs/.vitepress/dist/) mentions anything only the full edition has.
// npm run build runs it after building; npm run check:public runs it alone.
import { existsSync, readdirSync, readFileSync } from 'node:fs'
import { dirname, join, relative, sep } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const site = join(dirname(fileURLToPath(import.meta.url)), '..')
const dist = join(site, 'docs', '.vitepress', 'dist')

// The patterns live with the full edition's pages (private/website/leak-patterns.mjs), so this file names nothing
// private. Every page and picture of private/website/docs/ is also checked not to be in the build.
const priv = join(site, '..', 'private', 'website')
const patternsFile = join(priv, 'leak-patterns.mjs')
if (!existsSync(patternsFile)) {
  console.log('check-public: no private/website/ in this source tree, so nothing private to leak.')
  process.exit(0)
}
const banned = (await import(pathToFileURL(patternsFile).href)).default
const privateFiles = new Set()
const walkPriv = (dir) => {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name)
    if (e.isDirectory()) walkPriv(p)
    else privateFiles.add(relative(join(priv, 'docs'), p).split(sep).join('/').replace(/^public\//, '').replace(/\.md$/, '.html'))
  }
}
if (existsSync(join(priv, 'docs'))) walkPriv(join(priv, 'docs'))
// pages that exist in both editions (the public one with its own text) aren't private files
const docs = join(site, 'docs')
for (const f of [...privateFiles]) {
  const pub = join(docs, f.endsWith('.html') ? f.replace(/\.html$/, '.md') : join('public', f))
  if (existsSync(pub)) privateFiles.delete(f)
}

if (!existsSync(dist)) {
  console.error('No public build: run npm run build first.')
  process.exit(1)
}
const hits = []
let files = 0
const walk = (dir) => {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name)
    const rel = relative(dist, p).split(sep).join('/')
    if (e.isDirectory()) { walk(p); continue }
    if (privateFiles.has(rel) || banned.some((r) => r.test(rel))) hits.push(`${rel}: a full-edition file`)
    if (!/\.(html|js|mjs|css|json|txt|xml|svg|md|map)$/.test(e.name)) continue
    files++
    const text = readFileSync(p, 'utf8')
    for (const r of banned) {
      const g = new RegExp(r.source, r.flags + 'g')
      for (const m of text.matchAll(g)) {
        const at = m.index ?? 0
        hits.push(`${rel}: "${text.slice(Math.max(0, at - 50), at + m[0].length + 50).replace(/\s+/g, ' ')}"`)
      }
    }
  }
}
walk(dist)
if (hits.length) {
  console.error(`check-public: ${hits.length} mention(s) of full-edition content in the public build:`)
  for (const h of hits.slice(0, 60)) console.error('  ' + h)
  if (hits.length > 60) console.error(`  … and ${hits.length - 60} more`)
  process.exit(1)
}
console.log(`check-public: ${files} text files checked, no full-edition content.`)
