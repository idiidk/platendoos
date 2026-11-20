// The settings you may want to change when hosting the site somewhere else.
export default {
  // Where the site lives on its host: '/' at the root of its own domain, '/docs/' in a sub-folder of another site.
  // Must start and end with '/'. The SITE_BASE environment variable overrides it for one build:
  // SITE_BASE=/docs/ npm run build. (The Docker image, website/Dockerfile, builds the public edition at '/'.)
  base: '/',

  // The trailer on the home page. The video (about 56 MB) isn't in git: by default the page plays it from the
  // v0.3.1 release. `npm run build:with-trailer` copies trailer/platendoos-trailer.mp4 into the build instead.
  // Note: while the GitHub repository is private, the release asset doesn't load for visitors.
  trailerUrl: 'https://github.com/idiidk/platendoos/releases/download/v0.3.1/platendoos-trailer.mp4',

  repo: 'https://github.com/idiidk/platendoos',
}
