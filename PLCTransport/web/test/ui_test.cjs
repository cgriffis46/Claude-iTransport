// The built-in page in a real browser (headless Chromium, through
// Playwright), against PlcWebSecure_test --serve:
//
//   ./PlcWebSecure_test --serve 120 &        # prints its https URL and http port
//   NODE_PATH=$(npm root -g) node ui_test.cjs https://127.0.0.1:PORT HTTP_PORT [screenshot.png]
//
// Not part of ctest: it needs Node, Playwright and a Chromium. The test
// CA isn't installed in this browser, so certificate errors are ignored;
// everything else is as a user would see it.
const { chromium } = require('playwright');

const [base, httpPort, shot] = process.argv.slice(2);
let failures = 0;
function check(ok, what) {
  console.log((ok ? '  ok    ' : '  FAIL  ') + what);
  if (!ok) failures++;
}

(async () => {
  const browser = await chromium.launch();
  const page = await (await browser.newContext({ ignoreHTTPSErrors: true })).newPage();
  const row = name => page.locator('#tags tr', { hasText: name });
  const value = name => row(name).locator('td.v');

  await page.goto('http://127.0.0.1:' + httpPort + '/');
  check(page.url().startsWith(base), 'http:// is sent on to https://');
  await page.waitForSelector('#tags tr');
  check(await row('Oven.Setpoint').count() === 1 && await row('Safety.EStop').count() === 1, 'the tags are listed');
  check(await page.locator('#login').isVisible(), 'a login form');
  check(await row('Oven.Setpoint').locator('button').count() === 0, 'no Set controls before login');

  await page.fill('#user', 'operator');
  await page.fill('#password', 'wrong-password');
  await page.click('#login button');
  await page.waitForFunction(() => document.getElementById('loginMsg').textContent !== '');
  check((await page.textContent('#loginMsg')).includes('wrong user or password'), 'a wrong password is reported');

  await page.fill('#password', 'op-password');
  await page.click('#login button');
  await page.waitForFunction(() => document.getElementById('who').textContent.includes('Logged in as operator'));
  check(!(await page.locator('#login').isVisible()), 'logged in as the operator: the login form goes');
  const cookies = await page.context().cookies();
  const sid = cookies.find(c => c.name === 'sid');
  check(sid && sid.httpOnly && sid.secure && sid.sameSite === 'Strict', 'the session cookie: HttpOnly, Secure, SameSite=Strict');
  check(await page.evaluate(() => document.cookie.includes('sid=')) === false, 'and page scripts can\'t read it');

  await page.waitForSelector('#tags tr:has-text("Oven.Setpoint") button');
  check(await row('Oven.Setpoint').locator('button').count() === 1 && await row('Oven.Pump').locator('button').count() === 1,
        'Set controls on the allow-listed tags');
  check(await row('Safety.EStop').locator('button').count() === 0 && await row('Oven.Temp').locator('button').count() === 0,
        'none on the E-stop or the readings');

  await row('Oven.Setpoint').locator('input').fill('120');
  await row('Oven.Setpoint').locator('button').click();
  await page.waitForFunction(() => [...document.querySelectorAll('#tags tr')]
      .some(r => r.textContent.startsWith('Oven.Setpoint') && r.querySelector('td.v').textContent === '120'));
  check(true, 'setpoint set to 120, and the table shows it');

  await row('Oven.Setpoint').locator('input').fill('999');
  await row('Oven.Setpoint').locator('button').click();
  await page.waitForFunction(() => [...document.querySelectorAll('#tags tr')]
      .some(r => r.textContent.includes('outside the allowed range')));
  check(await value('Oven.Setpoint').textContent() === '120', '999 refused with a message, value unchanged');

  await row('Oven.Pump').locator('input').check();
  await row('Oven.Pump').locator('button').click();
  await page.waitForFunction(() => [...document.querySelectorAll('#tags tr')]
      .some(r => r.textContent.startsWith('Oven.Pump') && r.querySelector('td.v').textContent === 'true'));
  check(true, 'pump switched on');
  if (shot) await page.screenshot({ path: shot, fullPage: true });

  // A forged write from the page's own origin, but without the CSRF token.
  const forged = await page.evaluate(async () => (await fetch('/api/tags/Oven.Setpoint', {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{"value":50}' })).status);
  check(forged === 403, 'a write without the CSRF token: 403');

  await page.locator('#who button').click();
  await page.waitForSelector('#login', { state: 'visible' });
  check(!(await page.textContent('#who')).includes('Logged in'), 'logged out: the login form is back');
  check(await row('Oven.Setpoint').locator('button').count() === 0, 'logged out: the controls go');

  await page.fill('#user', 'viewer');
  await page.fill('#password', 'view-password');
  await page.click('#login button');
  await page.waitForFunction(() => document.getElementById('who').textContent.includes('(viewer)'));
  await page.waitForTimeout(1500);
  check(await page.locator('#tags button').count() === 0, 'a viewer sees no Set controls');

  await browser.close();
  console.log(failures ? 'FAILED' : 'all passed');
  process.exit(failures ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });
