import type {Config} from '@docusaurus/types';
import type * as Preset from '@docusaurus/preset-classic';

const config: Config = {
  title: 'httpp',
  tagline: 'HTTP client and server for Python and C++',
  favicon: 'img/favicon.svg',
  // GitHub Pages of the repository (.github/workflows/pages.yml, ADR-0064).
  url: 'https://mohammadraziei.github.io',
  baseUrl: '/httpp/',
  organizationName: 'mohammadraziei',
  projectName: 'httpp',
  onBrokenLinks: 'throw',
  onBrokenMarkdownLinks: 'warn',
  i18n: {defaultLocale: 'en', locales: ['en']},
  stylesheets: [
    'https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@400;600&family=Public+Sans:wght@400;500;700&display=swap',
  ],
  presets: [
    [
      'classic',
      {
        docs: {routeBasePath: '/', sidebarPath: './sidebars.ts'},
        blog: false,
        theme: {customCss: './src/css/custom.css'},
      } satisfies Preset.Options,
    ],
  ],
  themeConfig: {
    colorMode: {respectPrefersColorScheme: true},
    navbar: {
      title: 'httpp',
      items: [
        {type: 'custom-langSwitch', position: 'right'},
        {href: 'https://github.com/mohammadraziei/httpp', label: 'GitHub', position: 'right'},
      ],
    },
    prism: {additionalLanguages: ['cpp', 'bash']},
  } satisfies Preset.ThemeConfig,
};

export default config;
