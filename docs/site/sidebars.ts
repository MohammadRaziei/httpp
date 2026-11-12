import type {SidebarsConfig} from '@docusaurus/plugin-content-docs';

const sidebars: SidebarsConfig = {
  main: [
    'intro',
    'install',
    {
      type: 'category',
      label: 'Server',
      items: ['server/get-route', 'server/request-data', 'server/responses', 'server/hooks'],
    },
    {
      type: 'category',
      label: 'Client',
      items: ['client/get-request', 'client/post-request', 'client/download'],
    },
    'benchmarks',
  ],
};

export default sidebars;
