// Run with an existing Playwright installation; no dev server is needed.
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const assert = require('node:assert/strict');
const base = process.env.TEEDSP_URL || 'http://cm3588.lan:8790';
(async () => {
  const browser = await chromium.launch({headless:true, ...(process.env.CHROMIUM_PATH ? {executablePath:process.env.CHROMIUM_PATH} : {})});
  try {
    const page = await browser.newPage({viewport:{width:1280,height:900}});
    const errors=[]; page.on('pageerror', error => errors.push(error.message));
    await page.goto(base+'/index.html');
    await page.locator('#bypass:not([disabled])').waitFor();
    assert.equal(await page.locator('.module').count(),11);
    assert.equal(await page.locator('.control').count(),68);
    const before = await (await page.request.get(base+'/api/params')).json();
    await page.locator('#bypass').click();
    await page.locator('#saved').filter({hasText:'Saved'}).waitFor();
    const toggled=await (await page.request.get(base+'/api/params')).json();
    assert.equal(toggled.values['0'], before.values['0'] ? 0 : 1);
    await page.locator('#bypass').click();
    await page.locator('#saved').filter({hasText:'Saved'}).waitFor();
    const invalid=await page.request.post(base+'/api/params',{data:{'1':999}});
    assert.equal(invalid.status(),400);
    const after=await (await page.request.get(base+'/api/params')).json();
    assert.deepEqual(after.values,before.values);
    const volumeBefore=await (await page.request.get(base+'/api/volume')).json();
    try {
      await page.locator('#master').fill('40');
      await page.waitForFunction(()=>document.querySelector('#masterValue').textContent==='40%');
      await page.waitForTimeout(1200);
      assert.equal((await (await page.request.get(base+'/api/volume')).json()).volume,40);
      await page.locator('#mute').click();
      await page.waitForTimeout(1200);
      assert.equal((await (await page.request.get(base+'/api/volume')).json()).muted,!volumeBefore.muted);
      const invalidVolume=await page.request.post(base+'/api/volume',{data:{volume:101}});
      assert.equal(invalidVolume.status(),400);
    } finally { await page.request.post(base+'/api/volume',{data:volumeBefore}); }
    await page.setViewportSize({width:390,height:844});
    assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth <= innerWidth),true);
    await page.screenshot({path:'/tmp/teedsp-mobile.png',fullPage:true});
    await page.setViewportSize({width:1280,height:900});
    await page.screenshot({path:'/tmp/teedsp-desktop.png',fullPage:true});
    assert.deepEqual(errors,[]);
    console.log('PASS browser controls, master volume/mute, bypass save/restore, input validation, responsive layout, no JS errors');
  } finally { await browser.close(); }
})().catch(error=>{console.error(error);process.exit(1)});
