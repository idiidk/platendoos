# platendoos website

The documentation site for platendoos: getting started, installing, every mod, guides and how it works inside.
Built with [VitePress](https://vitepress.dev) 1.6.4 (versions pinned in `package.json` and `package-lock.json`).

## Run it

You need Node.js 18 or later.

```sh
cd website
npm ci              # once: installs VitePress into website/node_modules (about 100 MB)
npm run dev         # live preview at http://localhost:5173/ , reloads as you edit
```

## Build it

```sh
npm run build       # -> website/docs/.vitepress/dist/, then checks it (check:public)
npm run preview     # serves that build at http://localhost:4173/
```

The build fails on a dead internal link. The output folder is plain static files: HTML, CSS, JS, images.

## Editions

platendoos has a public and a full edition ([Editions](docs/internals/editions.md), and `docs/editions.md` in the
repository). These commands make the **public** site, the one that is published. VitePress reads the pages from an
assembled copy, `.site/<edition>/` (ignored by git), which `scripts/site.mjs` makes from `docs/` and keeps in step
during `npm run dev`. A page can mark lines that differ per edition, each marker on a line of its own:

```md
<!--@private NAME-->          only in the full edition: its slot NAME goes here
<!--@public NAME-->
The public edition's text.   the full edition shows its slot NAME instead
<!--@/public-->
```

The slots, and the full edition's own pages and pictures, aren't in this folder: a source tree that has them
documents its full edition there. `npm run build` checks the public build for anything of the full edition and fails
if it finds some (`npm run check:public` runs the check alone); without the full edition's files the check has
nothing to look for and passes.

## Where it lives: the one setting

`site.config.mjs` has `base`, the path the site is served under: `'/'` (the default) for the root of its own
domain, or `'/docs/'` (whatever the folder is, with slashes at both ends) for a sub-folder of another site. For one
build, `SITE_BASE=/docs/ npm run build` overrides it without editing the file.

## Host it

The site is plain static files (HTML, CSS, JS, images): pages are `.html` files (no clean-URL rewrites needed) and the
search runs in the browser, no server. The repository has a Docker setup for it, made for
[Coolify](https://coolify.io) but fine on any Docker host:

| File | |
|---|---|
| `/docker-compose.yml` | The service `docs`, built from the repository's root with `website/Dockerfile` |
| `/docker-compose.local.yml` | Adds a port on this computer, for running it locally |
| `website/Dockerfile` | Builds the site with Node (pinned), then serves it with nginx (pinned) |
| `website/docker/` | The nginx configuration: caching, gzip, the 404 page, `/healthz` |
| `/.dockerignore` | Keeps the build context to `website/` (no `node_modules`, build output, models or videos) |

The container serves the public edition at `/`, for everyone, always.

**Environment variables** (set in Coolify's *Environment Variables*, or in the shell / an `.env` file locally):

| Variable | Default | |
|---|---|---|
| `PORT` | `8080` | The port nginx listens on inside the container |

**Coolify.** New resource > your Git repository (a private one needs Coolify's GitHub app or a deploy key) > build
pack *Docker Compose*, base directory `/`, compose file `/docker-compose.yml`. On the service `docs`, set the domain
with the port the container listens on, e.g. `https://docs.example.com:8080` (Coolify's proxy routes the domain to
that port and makes the certificate; the container needs no host port).
The image has a health check (`/healthz`) that Coolify shows.

**Locally** (Docker Desktop, OrbStack or colima):

```sh
docker compose -f docker-compose.yml -f docker-compose.local.yml up --build     # from the repository's root
# -> http://localhost:8080/   (another port on this computer: DOCS_HOST_PORT=8081 docker compose …)
docker compose down
```

Caching: the hashed files in `assets/` are cached for a year, pages are checked on every visit (`no-cache`),
pictures for an hour. Text files are sent gzipped (compressed once at build time).

**Without Docker.** Build, then upload the contents of `docs/.vitepress/dist/` to any static host (Netlify,
Cloudflare Pages, an S3 bucket, nginx, Apache…), with `base` set to where it will live. Netlify / Cloudflare Pages:
base directory `website`, build command `npm ci && npm run build`, publish directory `docs/.vitepress/dist`.

## The trailer

The trailer video (about 56 MB, `trailer/platendoos-trailer.mp4`) isn't in git. By default the home page plays it from
the v0.3.1 GitHub release (`trailerUrl` in `site.config.mjs`); while the repository is private that link doesn't play
for visitors. To serve it from the site itself, render it (`trailer/render.py`) and build with:

```sh
npm run build:with-trailer    # copies the video into the assembled pages only; nothing goes into git
```

## Edit it

| Folder | |
|---|---|
| `docs/index.md` | The home page |
| `docs/getting-started/` | Introduction, Installation, First steps, Release notes |
| `docs/mods/` | One page per mod, and `index.md` with all of them |
| `docs/guides/` | Settings, loading a track, caches, Spotify, FLX4, performance, Windows, troubleshooting |
| `docs/internals/` | How it works, building, rekordbox updates, Windows internals, licence |
| `docs/.vitepress/config.mts` | Navigation, sidebar (the mods' groups and platforms), search, theme |
| `docs/.vitepress/theme/` | Colours (`custom.css`), the mod facts box (`ModInfo.vue`), the trailer player, the edition label (`EditionBadge.vue`) |
| `scripts/` | `site.mjs` (assembles an edition and runs VitePress), `check-public.mjs`, `settings_gen.py` (`npm run settings`), `screens.py` (`npm run screens`) |
| `docs/public/` | Logo, icon and screenshots (`img/`) |

Sources the pages were written from, to keep them in step: `README.md`, `CHANGELOG.md`, `docs/*.md`, the header
comment at the top of each `mods/*` file, and `core/config_template.h` (every option and default; `npm run settings` makes the option tables in
`docs/guides/settings.md` from it, the public edition's). `docs/getting-started/release-notes.md` and
`docs/guides/loading-a-track.md`, `docs/internals/rekordbox-updates.md` and `docs/internals/windows.md` were copied from
`CHANGELOG.md` and `docs/`, with links adjusted: copy them again when those change.

**Screenshots** come from the trailer's footage (`trailer/clips`, `trailer/shots`, which stay on the Mac that
recorded them): `npm run screens` (needs ffmpeg and Pillow) remakes `docs/public/img/*.webp`, each cropped to the
part of rekordbox it shows. rekordbox's playlist sidebar is always cropped away and a personal playlist name that
can't be cropped is blurred; only track titles, artists and the playlists Minimal Techno Mix, DnB, Liked Songs and
dance (local) may show. No user names, home folder paths or e-mail addresses anywhere.

## Cleaning up

`node_modules/`, `.site/`, `docs/.vitepress/dist*/` and `docs/.vitepress/cache/` are build products, ignored by git; delete them
any time.
