// The settings you may want to change when hosting the site somewhere else.
export default {
  // Where the site lives on its host: '/' at the root of its own domain, '/docs/' in a sub-folder of another site.
  // Must start and end with '/'. The SITE_BASE environment variable overrides it for one build:
  // SITE_BASE=/docs/ npm run build. (The Docker image, website/Dockerfile, builds the public edition at '/'.)
  base: '/',

  // The trailer on the home page (the public edition's, 2:57). The video isn't in git: it's served from the
  // platendoos-media R2 bucket on Cloudflare (media.idiidk.nl). `npm run build:with-trailer` copies
  // trailer/platendoos-trailer.mp4 into the build instead.
  trailerUrl: 'https://media.idiidk.nl/platendoos-trailer.mp4',

  repo: 'https://github.com/idiidk/platendoos',
}
