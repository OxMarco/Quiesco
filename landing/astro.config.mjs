// @ts-check
import { defineConfig } from 'astro/config';

import { SITE } from './src/site.ts';

// Served from GitHub Pages on the custom domain in public/CNAME, so no `base`.
export default defineConfig({
  site: SITE.url,
});
