const path = require('node:path');

module.exports = {
  uiHost: '127.0.0.1',
  uiPort: Number(process.env.PORT || 1880),
  flowFile: path.join(__dirname, 'flow.json'),
  nodesDir: path.join(__dirname, '..', 'node_modules'),
  httpStatic: [{ root: '/firmware', path: path.join(__dirname, '..', 'firmware') }],
  credentialSecret: process.env.NODE_RED_CREDENTIAL_SECRET || false,
  editorTheme: { projects: { enabled: false } },
  logging: { console: { level: 'info', metrics: false, audit: false } },
};