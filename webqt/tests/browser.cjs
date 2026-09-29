// Test actual Qt pointer/keyboard gestures against an isolated, simulated API.
// Browser assets come from the deployed container; no real settings are changed.
const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const assert=require('node:assert/strict');
const base=process.env.TEEDSP_URL||'http://cm3588.lan:8790';
(async()=>{
 const browser=await chromium.launch({headless:true,executablePath:process.env.CHROMIUM_PATH,args:['--enable-unsafe-swiftshader']});
 try {
 const page=await browser.newPage({viewport:{width:1440,height:900}});
 const schema=await (await page.request.get(base+'/api/params')).json();
 let values=Object.fromEntries(schema.parameters.map(p=>[p.id,p.default]));
 values['125']=-20; // EQ-only reset must preserve dynamics.
 let volume={volume:40,muted:false}, writes=[], failNext=false, metersOffline=false;
 const errors=[];page.on('pageerror',e=>errors.push(e.message));
 const bins=Array.from({length:1025},(_,i)=>-85+65*Math.exp(-Math.pow((i-43)/14,2)));
 await page.route('**/api/**',async route=>{
  const request=route.request(),path=new URL(request.url()).pathname;
  let data;
  if(path==='/api/params'){
   if(request.method()==='POST'){
    const patch=request.postDataJSON();writes.push(patch);
    if(failNext){failNext=false;return route.fulfill({status:503,body:'unavailable'});}
    values={...values,...patch};data=values;
   }else data={parameters:schema.parameters,values};
  }else if(path==='/api/volume'){
   if(request.method()==='POST')volume={...volume,...request.postDataJSON()};
   data=volume;
  }else {
   if(metersOffline)return route.fulfill({status:503,body:'offline'});
   data={input:bins,output:bins.map(v=>v-6),sampleRate:48000,quantum:1024,active:true,
    route:{routed:true},inPeakDbfs:[-12,-24],outPeakDbfs:[-18,-30],outRmsDbfs:-24,
    outLufsCh:[-22,-25],outLufsM:-20.2,compression:3,inputGain:2,outputGain:-1,
    bandGrDb:[1,2,3,4,5],spectralGain:[-2,-1,0,1,2,3,2,1,0,-1]};
  }
  return route.fulfill({contentType:'application/json',body:JSON.stringify(data)});
 });
 await page.goto(base+'/qt/index.html');
 await page.waitForSelector('body[data-ready=true]',{timeout:60000});
 await page.waitForTimeout(1500);
 assert.equal(writes.length,0,'opening editor must never write defaults');
 // A real geometry value saved by Qt WebAssembly made the next Chrome visit
 // fail during restoreGeometry() with "null function". Browser layout is fullscreen.
 const savedGeometry=Buffer.from(
  'QEJ5dGVBcnJheSgBw5nDkMOLAAMAAAAAAAAAAAAAAAAEw78AAAM4AAAAAAAAAAAAAARLAAACwpMAAAAAAAQAAAUAAAAAAAAAAAAAAATDvwAAAzgp',
  'base64').toString('utf8');
 await page.evaluate(value=>localStorage.setItem('qt-v0-TeeDSP-TeeDSP Web-ui/geometry',value),savedGeometry);
 await page.reload();
 await page.waitForSelector('body[data-ready=true]',{timeout:60000});
 await page.waitForTimeout(500);
 assert.equal(await page.locator('#loading').evaluate(el=>el.style.display),'none',
  'saved browser geometry must not crash the editor on reload');
 assert.equal(writes.length,0,'reopening editor must never write defaults');
 // Third EQ node: frequency and gain drag, then wheel changes Q.
 await page.mouse.move(621,384);await page.mouse.down();
 await page.mouse.move(680,330,{steps:8});await page.mouse.up();await page.waitForTimeout(900);
 assert(values['122']>1300 && values['124']>3,'EQ drag must update frequency and gain');
 assert(writes.every(p=>Object.keys(p).every(k=>['122','124'].includes(k))),'gesture must only patch its own fields');
 const q=values['123'];await page.mouse.wheel(0,-120);await page.waitForTimeout(600);
 assert.notEqual(values['123'],q,'wheel adjusts band Q');
 // Double-click resets EQ shape while retaining the band dynamics.
 await page.mouse.dblclick(680,330,{delay:80});await page.waitForTimeout(700);
 assert.equal(values['124'],0);assert.equal(values['122'],1000);assert.equal(values['125'],-20);
 // Context menu is asynchronous (QMenu::exec cannot work on default WASM).
 await page.mouse.click(621,384,{button:'right'});await page.waitForTimeout(200);await page.screenshot({path:'/tmp/qt-menu.png'});
 await page.mouse.click(700,492);await page.waitForTimeout(700);
 assert.equal(values['125'],0,'Reset Band context action must reset dynamics too');
 await page.mouse.move(1078,451);await page.mouse.down();await page.mouse.move(1078,411,{steps:5});await page.mouse.up();
 await page.waitForTimeout(500);assert(values['8']>-18,'compressor knob drag works');
 await page.mouse.move(1238,547);await page.mouse.wheel(0,120);await page.waitForTimeout(500);
 assert(values['13']<0,'negative compressor makeup has Qt parity');
 failNext=true;
 await page.mouse.click(239,32);await page.waitForTimeout(1000);
 assert.equal(values['0'],1,'failed save must retry and preserve the gesture');
 await page.mouse.click(239,32);await page.waitForTimeout(500);
 assert.equal(values['0'],0);
 await page.mouse.click(1394,32);await page.waitForTimeout(700);
 assert.equal(volume.muted,true,'master mute works');
 // Remote changes (another tab) update the Qt editor without echo writes.
 const count=writes.length;values['0']=1;await page.waitForTimeout(800);
 assert.equal(writes.length,count);
 await page.mouse.click(239,32);await page.waitForTimeout(600);assert.equal(values['0'],0);
 await page.screenshot({path:'/tmp/teedsp-qt-parity.png'});
 metersOffline=true;await page.waitForTimeout(3800);
 const before=writes.length;await page.mouse.click(239,32);await page.waitForTimeout(400);
 assert.equal(writes.length,before,'disconnected editor disables parameter writes');
 metersOffline=false;await page.waitForTimeout(1000);
 await page.mouse.click(239,32);await page.waitForTimeout(500);assert.equal(values['0'],1);
 assert.deepEqual(errors,[]);
 console.log('PASS Qt startup, EQ drag/Q/reset/menu, isolated patches, retry/reconnect, external updates, master mute and rendered meters');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exit(1)});
