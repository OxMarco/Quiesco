# Quiesco landing page

The site at [quiesco.rest](https://quiesco.rest), built with [Astro](https://astro.build) as a static page.

```sh
npm install
npm run dev      # http://localhost:4321
npm run check    # type-check
npm run build    # static site in dist/
```

## Layout

| Path | Contents |
|---|---|
| `src/pages/index.astro` | The page, assembled from the sections in `src/components/` |
| `src/components/UnitDemo.astro` | The unit's bento screen with the example rooms |
| `src/lib/bento.ts` | Bento layout and comfort bands, ported from `firmware/src/ui` |
| `src/lib/units.ts` | °F for visitors whose time zone is in the US (or another °F country) |
| `src/lib/night.ts` | The generated sample night on the app preview |
| `src/lib/inview.ts` | Holds each section's animations until it scrolls into view |
| `src/site.ts` | External links (X, GitHub, the build guide, the campaign) |
| `src/styles/global.css` | Tide & Sand palette and shared styles |

Fonts are self-hosted through Fontsource, and the page loads nothing from third parties.

## Deploying

The site lives in the Quiesco monorepo, and its workflows are at the repository root. Pushing to `main` with changes under `landing/` runs [`landing-deploy.yml`](../.github/workflows/landing-deploy.yml), which builds the site and publishes it to GitHub Pages. Pull requests that touch `landing/` run [`landing-check.yml`](../.github/workflows/landing-check.yml) (type-check and build). Changes elsewhere in the repository don't trigger either one.

One-time setup on GitHub:

1. **Settings → Pages → Build and deployment → Source:** GitHub Actions.
2. **Custom domain:** `public/CNAME` sets `quiesco.rest`. Point the domain's DNS at GitHub Pages (A records to `185.199.108.153`, `.109.153`, `.110.153`, `.111.153`, or a `CNAME` to `<user>.github.io` for a subdomain), then tick **Enforce HTTPS**.
