// External links used across the page.
export const LINKS = {
  hackaday: 'https://hackaday.io/pages/1897237',
  mastodon: 'https://fosstodon.org/@oxmarco',
  github: 'https://github.com/OxMarco/Quiesco',
  // The build guide is the README section on GitHub; there is no separate docs site.
  buildYourOwn: 'https://github.com/OxMarco/Quiesco#-build-your-own',
  // Where "Get yours" leads. Until Crowd Supply's pre-launch page exists, the Hackaday page carries
  // build logs and launch news. TODO: point it at the Crowd Supply pre-launch page, then at the campaign.
  campaign: 'https://hackaday.io/pages/1897237',
  privacy: '/privacy',
  support: 'mailto:info@impossiblelabs.xyz',
};

export const SITE = {
  /** Canonical address, used by astro.config.mjs and the meta tags. */
  url: 'https://quiesco.rest',
  title: 'Quiesco · Open-source bedroom environment monitor',
  description:
    'Quiesco measures CO₂, temperature, humidity, light and noise in your bedroom and tells you what to fix. Open-source hardware, firmware and app.',
};
