# The documentation site as one container (website/README.md, "Host it"): nginx serving the public edition at /,
# for everyone.
# Build it from the repository's root (the build context, see /.dockerignore): docker compose up --build, or
#   docker build -f website/Dockerfile -t platendoos-docs .

# ---- build: VitePress -> static files ----
FROM node:24.22.1-alpine3.24 AS build
WORKDIR /src/website
COPY website/package.json website/package-lock.json ./
RUN npm ci --no-audit --no-fund --loglevel=error
COPY website/ ./
RUN SITE_BASE=/ npm run build
# Compressed copies next to the files, served as they are (gzip_static); what's left is compressed on the fly.
RUN find docs/.vitepress -path '*/dist*' -type f \
      \( -name '*.html' -o -name '*.js' -o -name '*.css' -o -name '*.json' -o -name '*.svg' -o -name '*.txt' -o -name '*.xml' \) \
      -size +1k -exec gzip -9 -k -n {} +

# ---- serve: nginx ----
FROM nginx:1.30.5-alpine3.24
# PORT: what nginx listens on inside the container (Coolify: the port after the domain, https://docs.example.com:8080)
ENV PORT=8080 \
    NGINX_ENVSUBST_FILTER=^PORT$
RUN rm -f /etc/nginx/conf.d/default.conf
COPY website/docker/docs.conf.template /etc/nginx/templates/docs.conf.template
COPY website/docker/headers.conf /etc/nginx/snippets/headers.conf
COPY --from=build /src/website/docs/.vitepress/dist/ /srv/public/
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD wget -q -O /dev/null "http://127.0.0.1:${PORT}/healthz" || exit 1
