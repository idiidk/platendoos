import { defineConfig, type DefaultTheme } from 'vitepress'
import { existsSync } from 'node:fs'
import { fileURLToPath, pathToFileURL } from 'node:url'
import site from '../../site.config.mjs'

const base = process.env.SITE_BASE || site.base
// Which edition: public (the default) or private, the full one with what ../../../private/website/ adds.
// scripts/site.mjs sets it and assembles the pages in ../../.site/<edition>/ (see that script).
const edition = process.env.DOCS_EDITION === 'private' ? 'private' : 'public'
type Extra = {
  // sidebar entries per group: { group: 'Guides' | <a mods group>, text?, link? | mod?, platform?, before? }
  sidebar?: { group: string; text?: string; link?: string; mod?: string; platform?: string; before?: string }[]
  badge?: string // shown next to the site title
  trailer?: string // the trailer's URL, instead of site.trailerUrl
}
const extraFile = fileURLToPath(new URL('../../../private/website/site.mjs', import.meta.url))
const extra: Extra = edition === 'private' && existsSync(extraFile) ? (await import(pathToFileURL(extraFile).href)).default : {}
// npm run build:with-trailer serves the trailer from the site itself (scripts/build-with-trailer.mjs)
const trailer = process.env.PLATENDOOS_TRAILER === 'local' ? '/platendoos-trailer.mp4' : extra.trailer ?? site.trailerUrl

// The mods, grouped as in the README. mac: macOS only; win: Windows only.
const modGroups: { text: string; mods: [string, string?][] }[] = [
  { text: 'Spotify', mods: [['spotifyapi'], ['spotifyfix'], ['spotifystart', 'win'], ['autocue'], ['genres'], ['spotifyaudio'], ['artcache'], ['spotifylog'], ['spotifydl']] },
  { text: 'Analysis', mods: [['analyzer'], ['bettergrid'], ['betterkey'], ['quickdeck'], ['parallel'], ['tfpool'], ['streamio'], ['timing']] },
  { text: 'Browsing and playing', mods: [['stemcache'], ['betterstems'], ['fastwave'], ['audiomem', 'mac'], ['betterfx', 'mac'], ['keepmaster'], ['tidymenu'], ['multisort'], ['keycolumn'], ['xfadelock'], ['outputsync', 'mac'], ['fontcache', 'mac'], ['quickframe', 'mac'], ['relayout', 'mac']] },
  { text: 'Controllers', mods: [['flx4'], ['linein']] },
  { text: 'Quieter in the background', mods: [['framerate'], ['badge'], ['prefs'], ['screeninfo', 'mac'], ['midipoll', 'mac'], ['idlewake', 'mac']] },
]
const tag = (p?: string) => (p === 'mac' ? ' <span class="pd-tag">mac</span>' : p === 'win' ? ' <span class="pd-tag">win</span>' : '')

// The full edition's extra pages, each put before the entry it names (or at the end of its group).
function insert(items: DefaultTheme.SidebarItem[], group: string) {
  for (const e of extra.sidebar ?? []) {
    if (e.group !== group) continue
    const item = e.mod ? { text: e.mod + tag(e.platform), link: `/mods/${e.mod}` } : { text: e.text!, link: e.link! }
    const at = e.before ? items.findIndex((i) => i.link?.endsWith('/' + e.before)) : -1
    items.splice(at < 0 ? items.length : at, 0, item)
  }
  return items
}

const sidebar: DefaultTheme.Sidebar = {
  '/': [
    {
      text: 'Getting started',
      items: insert([
        { text: 'Introduction', link: '/getting-started/introduction' },
        { text: 'Installation', link: '/getting-started/installation' },
        { text: 'First steps', link: '/getting-started/first-steps' },
        { text: 'Release notes', link: '/getting-started/release-notes' },
      ], 'Getting started'),
    },
    {
      text: 'Guides',
      items: insert([
        { text: 'Settings and config.txt', link: '/guides/settings' },
        { text: 'Loading a track', link: '/guides/loading-a-track' },
        { text: 'Caches and external drives', link: '/guides/caches' },
        { text: 'Spotify', link: '/guides/spotify' },
        { text: 'DDJ-FLX4 with four decks', link: '/guides/flx4-four-decks' },
        { text: 'Performance and dropouts', link: '/guides/performance' },
        { text: 'Windows notes', link: '/guides/windows' },
        { text: 'Troubleshooting and FAQ', link: '/guides/troubleshooting' },
      ], 'Guides'),
    },
    {
      text: 'Mods',
      items: [
        { text: 'All mods', link: '/mods/' },
        ...modGroups.map((g) => ({
          text: g.text,
          collapsed: true,
          items: insert(g.mods.map(([m, p]) => ({ text: m + tag(p), link: `/mods/${m}` })), g.text),
        })),
      ],
    },
    {
      text: 'Under the hood',
      collapsed: true,
      items: insert([
        { text: 'How it works', link: '/internals/how-it-works' },
        { text: 'Building from source', link: '/internals/building' },
        { text: 'When rekordbox updates', link: '/internals/rekordbox-updates' },
        { text: 'Windows internals', link: '/internals/windows' },
        { text: 'Intel Macs (beta)', link: '/internals/intel-mac' },
        { text: 'Editions', link: '/internals/editions' },
        { text: 'Licence and credits', link: '/internals/licence' },
      ], 'Under the hood'),
    },
  ],
}

export default defineConfig({
  base,
  srcDir: `../.site/${edition}`,
  outDir: edition === 'private' ? '.vitepress/dist-full' : '.vitepress/dist',
  lang: 'en',
  title: 'platendoos',
  description: 'Mods for rekordbox 7 on macOS and Windows: faster, smoother, especially with Spotify.',
  appearance: 'dark', // dark first, like rekordbox; the switch in the top bar gives light mode
  cleanUrls: false, // pages end in .html, so any static host serves them without rewrite rules
  lastUpdated: false,
  head: [
    ['link', { rel: 'icon', type: 'image/svg+xml', href: `${base}icon.svg` }],
    ['meta', { name: 'theme-color', content: '#2978eb' }],
    // the full edition is internal: never in a search engine
    ...(edition === 'private' ? [['meta', { name: 'robots', content: 'noindex, nofollow' }] as [string, Record<string, string>]] : []),
  ],
  markdown: { theme: { light: 'github-light', dark: 'github-dark' } },
  themeConfig: {
    logo: '/icon.svg',
    siteTitle: 'platendoos',
    nav: [
      { text: 'Get started', link: '/getting-started/introduction', activeMatch: '/getting-started/' },
      { text: 'Mods', link: '/mods/', activeMatch: '/mods/' },
      { text: 'Guides', link: '/guides/settings', activeMatch: '/guides/' },
      { text: 'Under the hood', link: '/internals/how-it-works', activeMatch: '/internals/' },
      { text: 'Download', link: `${site.repo}/releases/latest` },
    ],
    sidebar,
    socialLinks: [{ icon: 'github', link: site.repo }],
    search: { provider: 'local' },
    outline: { level: [2, 3], label: 'On this page' },
    docFooter: { prev: 'Previous', next: 'Next' },
    footer: {
      message: 'platendoos is an unofficial hobby project, not made by or affiliated with AlphaTheta / Pioneer DJ or Spotify. Licensed under CC BY-NC-SA 4.0.',
    },
    // read by the <Trailer> component on the home page
    trailer,
    // read by the <EditionBadge> next to the site title; only the full edition has one
    editionBadge: extra.badge ?? '',
  } as DefaultTheme.Config & { trailer: string; editionBadge: string },
})
