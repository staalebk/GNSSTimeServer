// Run from the repository root after editing main/index.html.
const fs = require('fs');
const path = require('path');
const main = path.join(__dirname, '../main');
const source = fs.readFileSync(path.join(main, 'index.html'), 'utf8');
const target = path.join(main, 'status_page.h');
const header = fs.readFileSync(target, 'utf8');
const start = header.indexOf('static const char STATUS_PAGE_HTML');
const end = header.indexOf(';', header.indexOf('</html>', start));
if (start < 0 || end < 0) throw new Error('Embedded page boundaries missing');
const lines = source.split(/\r?\n/).map(line =>
  line.includes('const HEALTH_POLL_MS =')
    ? '"const HEALTH_POLL_MS = " SP_STR(HEALTH_POLL_INTERVAL_MS) ";\\n"'
    : JSON.stringify(line + '\n')).join('\n');
fs.writeFileSync(target, header.slice(0, start) +
  'static const char STATUS_PAGE_HTML[] =\n' + lines + ';' + header.slice(end + 1));
