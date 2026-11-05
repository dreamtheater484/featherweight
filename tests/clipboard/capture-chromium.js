// Copies a test page in Playwright's own headless Chromium (never an installed Chrome) and saves
// what the copy put on the clipboard (text/html read unsanitized, text/plain), wrapped as Chrome
// writes CF_HTML on Windows. For the clipboard tests (test_clipboard.cpp).
//   node tests\clipboard\capture-chromium.js tests\clipboard
// Uses the global npm Playwright; after installing or upgrading it, `npx playwright install chromium`.
const path = require('path');
const fs = require('fs');
const { chromium } = require(path.join(process.env.APPDATA, 'npm', 'node_modules', 'playwright'));
const out = process.argv[2];

const page1 = `<!doctype html><html><head><meta charset="utf-8"><title>Clip</title><style>
body{font-family:Georgia,serif;font-size:16px;color:#222}
h1{font-family:Arial,sans-serif;color:#1a4d8f}
.lead{font-size:18px;text-align:center}
.note{background:#fff3b0}
td,th{border:1px solid #999;padding:4px}
</style></head><body>
<h1>Quarterly report</h1>
<p class="lead">Sales rose by <b>12%</b> in the <em style="color:red">third quarter</em>; the full figures are <a href="https://example.com/figures">online</a>.</p>
<p>Line one<br>line two &amp; more&nbsp;&nbsp;spaces, H<sub>2</sub>O and x<sup>2</sup>.</p>
<ol><li>First point</li><li>Second point<ul><li>Nested bullet</li></ul></li><li>Third point</li></ol>
<table><tr><th>Region</th><th>Q2</th><th>Q3</th></tr><tr><td>North</td><td>1,200</td><td class="note">1,344</td></tr></table>
<p>After the table.</p>
</body></html>`;

function cfHtml(fragment, url) {
  const pre = '<html>\r\n<body>\r\n<!--StartFragment-->';
  const post = '<!--EndFragment-->\r\n</body>\r\n</html>';
  const header = (a, b, c, d) =>
    `Version:0.9\r\nStartHTML:${a}\r\nEndHTML:${b}\r\nStartFragment:${c}\r\nEndFragment:${d}\r\nSourceURL:${url}\r\n`;
  const pad = (n) => String(n).padStart(10, '0');
  const h0 = Buffer.byteLength(header(pad(0), pad(0), pad(0), pad(0)));
  const f = Buffer.byteLength(fragment), p = Buffer.byteLength(pre), q = Buffer.byteLength(post);
  return header(pad(h0), pad(h0 + p + f + q), pad(h0 + p), pad(h0 + p + f)) + pre + fragment + post;
}

(async () => {
  const browser = await chromium.launch({ headless: true });
  try {
    const context = await browser.newContext({ permissions: ['clipboard-read', 'clipboard-write'] });
    const page = await context.newPage();
    await page.route('https://example.test/**', (r) => r.fulfill({ contentType: 'text/html', body: page1 }));
    await page.goto('https://example.test/report.html');
    const cases = [
      ['page', 'document.body'],
      ['partial', null],
    ];
    for (const [name, sel] of cases) {
      await page.evaluate((sel) => {
        const r = document.createRange();
        if (sel) {
          r.selectNodeContents(eval(sel));
        } else {
          const t = document.querySelector('.lead').firstChild;  // "Sales rose by " ... "third"
          r.setStart(t, 6);
          const em = document.querySelector('em').firstChild;
          r.setEnd(em, 5);
        }
        getSelection().removeAllRanges();
        getSelection().addRange(r);
      }, sel);
      await page.keyboard.press('ControlOrMeta+C');
      const got = await page.evaluate(async () => {
        const items = await navigator.clipboard.read({ unsanitized: ['text/html'] });
        const o = {};
        for (const item of items)
          for (const type of item.types) o[type] = await (await item.getType(type)).text();
        return o;
      });
      if (!got['text/html']) throw new Error('no text/html for ' + name + ': ' + JSON.stringify(Object.keys(got)));
      fs.writeFileSync(path.join(out, `chromium-${name}.html`), cfHtml(got['text/html'], 'https://example.test/report.html'));
      fs.writeFileSync(path.join(out, `chromium-${name}.txt`), got['text/plain'] || '');
      console.log(name, got['text/html'].length, 'chars of HTML');
    }
    console.log('version', browser.version());
  } finally {
    await browser.close();
  }
})().catch((e) => { console.error(e.message); process.exit(1); });
