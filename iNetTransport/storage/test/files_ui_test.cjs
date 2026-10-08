// The upload page (/files.html) in a real browser (headless Chromium,
// through Playwright), against StorageWeb_test --serve:
//
//   ./StorageWeb_test --serve 120 &          # prints its https URL; a fresh one each run
//   NODE_PATH=$(npm root -g) node files_ui_test.cjs https://127.0.0.1:PORT [screenshot.png]
//
// Not part of ctest: it needs Node, Playwright and a Chromium. The test
// CA isn't installed in this browser, so certificate errors are ignored.
const { chromium } = require('playwright');

const [base, shot] = process.argv.slice(2);
let failures = 0;
function check(ok, what) {
  console.log((ok ? '  ok    ' : '  FAIL  ') + what);
  if (!ok) failures++;
}

(async () => {
  const browser = await chromium.launch();
  const page = await (await browser.newContext({ ignoreHTTPSErrors: true })).newPage();
  page.on('dialog', d => d.accept());   // the delete confirmation
  const row = name => page.locator('#files tr', { hasText: name });
  // Fetched by the page itself, on the browser's own connections.
  const fetchIn = path => page.evaluate(async p => {
    const r = await fetch(p, { cache: 'no-store' });
    return { status: r.status, type: r.headers.get('content-type'), text: await r.text() };
  }, path);

  await page.goto(base + '/files.html');
  await page.waitForSelector('#login:not([hidden])');
  check(await page.locator('#admin').isHidden(), 'logged out: the login form, no file list');

  await page.fill('#user', 'operator');
  await page.fill('#password', 'op-password');
  await page.click('#login button');
  await page.waitForFunction(() => document.getElementById('space').textContent !== '');
  check((await page.textContent('#space')).includes('admin login is needed'), 'an operator: told an admin is needed');
  await page.click('#who button');   // log out
  await page.waitForSelector('#login:not([hidden])');

  await page.fill('#user', 'admin');
  await page.fill('#password', 'admin-password');
  await page.click('#login button');
  await page.waitForFunction(() => document.getElementById('space').textContent.includes('bytes free'));
  check((await page.textContent('#who')).includes('Logged in as admin (admin)'), 'logged in as the admin: the file list');

  // A page bigger than several pieces, a style sheet into a folder, and a
  // name that needs escaping in the URL.
  let html = '<!doctype html><title>From the browser</title><h1 id="hi">uploaded from files.html</h1>\n';
  while (html.length < 6000) html += '<!-- padding ' + html.length + ' -->\n';
  await page.setInputFiles('#pick', [
    { name: 'index.html', mimeType: 'text/html', buffer: Buffer.from(html) },
    { name: 'a b#c.txt', mimeType: 'text/plain', buffer: Buffer.from('odd name') },
    { name: 'empty.txt', mimeType: 'text/plain', buffer: Buffer.alloc(0) },
  ]);
  await page.click('#send');
  await page.waitForSelector('#files tr:has-text("/index.html")');
  await page.waitForSelector('#files tr:has-text("/empty.txt")');
  check(await row('/index.html').locator('td.n').textContent() === String(html.length), 'index.html listed with its size');
  check(await row('/a b#c.txt').count() === 1 && await row('/empty.txt').locator('td.n').textContent() === '0',
        'an odd name and an empty file too');
  check((await page.textContent('#log')).includes('empty.txt: 0 bytes, done'), 'progress shown, then done');

  await page.fill('#dir', '/css');
  await page.setInputFiles('#pick', [{ name: 'site.css', mimeType: 'text/css', buffer: Buffer.from('h1{color:teal}') }]);
  await page.click('#send');
  await page.waitForSelector('#files tr:has-text("/css/site.css")');
  check(true, 'into a folder: /css/site.css');

  check((await fetchIn('/')).text === html, '/ is now the uploaded page, whole');
  check((await fetchIn('/a%20b%23c.txt')).text === 'odd name', 'the odd name served');
  check((await fetchIn('/css/site.css')).type.startsWith('text/css'), 'site.css served as text/css');

  await row('/a b#c.txt').locator('button').click();
  await page.waitForFunction(() => ![...document.querySelectorAll('#files tr')].some(r => r.textContent.includes('a b#c.txt')));
  check((await fetchIn('/a%20b%23c.txt')).status === 404, 'deleted: gone from the list and the server');

  await page.fill('#dir', '/../x');
  await page.setInputFiles('#pick', [{ name: 'evil.txt', mimeType: 'text/plain', buffer: Buffer.from('x') }]);
  await page.click('#send');
  await page.waitForFunction(() => document.getElementById('log').textContent.startsWith('failed'));
  check((await page.textContent('#log')).includes('not a file path') &&
        await page.evaluate(() => getComputedStyle(document.getElementById('log')).color) === 'rgb(176, 0, 32)',
        'a path with .. : refused, and the reason shown in red');

  if (shot) await page.screenshot({ path: shot, fullPage: true });
  // The server takes three clients (maxClients): let this browser's
  // keep-alive connections go before another opens its own.
  await page.context().close();
  const dark = await (await browser.newContext({ ignoreHTTPSErrors: true, colorScheme: 'dark' })).newPage();
  await dark.goto(base + '/files.html');
  const bg = await dark.evaluate(() => getComputedStyle(document.body).backgroundColor);
  check(bg === 'rgb(17, 17, 17)', 'dark mode follows the system');

  await browser.close();
  console.log(failures ? 'FAILED' : 'all passed');
  process.exit(failures ? 1 : 0);
})().catch(e => { console.error(e); process.exit(1); });
