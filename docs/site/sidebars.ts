import type {SidebarsConfig} from '@docusaurus/plugin-content-docs';

const sidebars: SidebarsConfig = {
  main: [
    'intro',
    'install',
    {
      type: 'category',
      label: 'Server',
      items: ['server/get-route', 'server/request-data', 'server/responses', 'server/errors', 'server/swagger', 'server/send-file', 'server/serve-directory', 'server/server-handle', 'server/file-browser', 'server/hooks'],
    },
    {
      type: 'category',
      label: 'Client',
      items: ['client/get-request', 'client/post-request', 'client/download', 'client/upload', 'client/curl-command'],
    },
    'benchmarks',
  ],
};

export default sidebars;
