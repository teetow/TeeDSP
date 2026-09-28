'use strict';
let values = {}, descriptors = [], controls = new Map(), pending = {}, sending = false, timer;
const $ = id => document.getElementById(id);
const format = n => Number(n.toFixed(2)).toString();
async function api(path, options) {
  const response = await fetch(path, options);
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || `HTTP ${response.status}`);
  return data;
}
function bypassLabel() {
  $('bypass').textContent = values[0] ? 'Bypassed · enable DSP' : 'Processing on · bypass';
  $('bypass').classList.toggle('off', Boolean(values[0]));
}
function queue(id, value) {
  values[id] = value; pending[id] = value;
  bypassLabel(); $('saved').textContent = 'Saving…'; $('saved').className = '';
  clearTimeout(timer); timer = setTimeout(save, 120);
}
async function save() {
  if (sending || !Object.keys(pending).length) return;
  sending = true;
  const patch = pending; pending = {};
  try {
    await api('/api/params', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(patch)});
    $('saved').textContent = 'Saved'; $('saved').className = '';
  } catch (e) {
    pending = {...patch, ...pending};
    $('saved').textContent = `Not saved: ${e.message} · retrying`; $('saved').className = 'error';
  }
  sending = false;
  if (Object.keys(pending).length) timer = setTimeout(save, 1000);
}
function addControl(p, parent) {
  const row = document.createElement('div'); row.className = 'control';
  const label = document.createElement('label'); label.className = 'label'; label.htmlFor = `p${p.id}`;
  const title = document.createElement('span'); title.textContent = p.name; label.append(title); row.append(label);
  let input;
  if (p.stepped && p.max === 1) {
    input = document.createElement('input'); input.type = 'checkbox'; input.checked = Boolean(values[p.id]);
    input.onchange = () => queue(p.id, input.checked ? 1 : 0); label.append(input);
  } else if (p.stepped) {
    input = document.createElement('select');
    ['Bell', 'Low shelf', 'High shelf'].forEach((name, value) => input.add(new Option(name, value)));
    input.value = values[p.id]; input.onchange = () => queue(p.id, Number(input.value)); label.append(input);
  } else {
    input = document.createElement('input'); input.type = 'number'; input.min = p.min; input.max = p.max;
    input.step = 'any'; input.value = format(values[p.id]); label.append(input);
    const range = document.createElement('input'); range.type = 'range'; range.min = 0; range.max = 1000;
    range.setAttribute('aria-label', p.module + ' ' + p.name);
    const log = p.min > 0 && p.max / p.min > 50;
    const toSlider = v => 1000 * (log ? Math.log(v/p.min)/Math.log(p.max/p.min) : (v-p.min)/(p.max-p.min));
    const fromSlider = v => log ? p.min * (p.max/p.min)**(v/1000) : p.min+(p.max-p.min)*v/1000;
    range.value = toSlider(values[p.id]);
    range.oninput = () => { const v = Number(fromSlider(Number(range.value)).toFixed(3)); input.value = format(v); queue(p.id,v); };
    input.onchange = () => {
      if (!input.value || !input.checkValidity()) { input.reportValidity(); return; }
      const v = Number(input.value); range.value = toSlider(v); queue(p.id,v);
    };
    row.append(range);
  }
  input.id = `p${p.id}`; controls.set(p.id, input); parent.append(row);
}
function render() {
  const groups = new Map();
  for (const p of descriptors) {
    if (p.id === 0) continue;
    if (!groups.has(p.module)) groups.set(p.module, []);
    groups.get(p.module).push(p);
  }
  for (const [name, params] of groups) {
    const section = document.createElement('section'); section.className = 'module';
    const title = document.createElement('h2'); title.textContent = name; section.append(title);
    let advanced;
    for (const p of params) {
      if (p.name.startsWith('Dyn ')) {
        if (!advanced) { advanced = document.createElement('details'); const summary = document.createElement('summary'); summary.textContent = 'Dynamic EQ'; advanced.append(summary); section.append(advanced); }
        addControl(p, advanced);
      } else addControl(p, section);
    }
    $('controls').append(section);
  }
  $('bypass').disabled = false; $('bypass').onclick = () => queue(0, values[0] ? 0 : 1); bypassLabel();
}
function setupCanvas(id) {
  const canvas = $(id), width = Math.round(canvas.clientWidth * devicePixelRatio), height = Math.round(canvas.clientHeight * devicePixelRatio);
  if (canvas.width !== width || canvas.height !== height) { canvas.width = width; canvas.height = height; }
  return [canvas.getContext('2d'), width, height];
}
function paint(m) {
  const [ctx,w,h] = setupCanvas('spectrum'); ctx.clearRect(0,0,w,h);
  ctx.strokeStyle = '#2b3542'; ctx.lineWidth = 1;
  for (let i=1;i<5;i++) { ctx.beginPath();ctx.moveTo(0,h*i/5);ctx.lineTo(w,h*i/5);ctx.stroke(); }
  const at = (bins,x) => bins[Math.min(bins.length-1, Math.max(1, Math.round(20*1000**(x/w)*2048/48000)))];
  for (const [bins,color] of [[m.input,'#62b5e5'],[m.output,'#e7ab69']]) {
    if (!bins?.length) continue;
    ctx.beginPath();ctx.strokeStyle=color;ctx.lineWidth=1.5*devicePixelRatio;
    for(let x=0;x<w;x++){const y=h*(1-Math.max(0,Math.min(1,(at(bins,x)+100)/100))); if(x===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);}
    ctx.stroke();
  }
  const [heat,hw,hh] = setupCanvas('waterfall');
  heat.drawImage($('waterfall'),0,0,hw,hh-1,0,1,hw,hh-1);
  if (m.output?.length) for(let x=0;x<hw;x++) {
    const bin=Math.min(1024,Math.max(1,Math.round(20*1000**(x/hw)*2048/48000)));
    const level=Math.max(0,Math.min(1,(m.output[bin]+90)/85));
    heat.fillStyle=`hsl(${270-240*level} 85% ${4+65*level}%)`;heat.fillRect(x,0,1,1);
  }
  const db = v => v > 0 ? (20*Math.log10(v)).toFixed(1) : '−∞';
  $('meters').textContent=`IN ${db(m.peakIn)} dBFS   ·   OUT ${db(m.peakOut)} dBFS   ·   COMP −${(m.compression||0).toFixed(1)} dB`;
  const routed=m.route?.routed;
  $('connection').textContent=routed ? (m.active && m.peakIn > 1e-7 ? '● AirPlay → TeeDSP → UA-25 · playing' : '● Ready · waiting for AirPlay audio') : '○ Waiting for audio routing';
  $('footerStatus').textContent=`48 kHz stereo · ${m.quantum||'—'} frames · AirPlay only`;
}
async function poll() {
  try { if(!document.hidden) paint(await api('/api/meters')); }
  catch(e){$('connection').textContent='○ Disconnected · reconnecting…';}
  setTimeout(poll, 80);
}
async function init() {
  try {const data=await api('/api/params');values=data.values;descriptors=data.parameters;render();poll();}
  catch(e){$('connection').textContent=`Connecting… ${e.message}`;setTimeout(init,2000);}
}
init();

let masterEditing=false,masterTimer;
async function masterUpdate(patch){
 try{await api('/api/volume',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(patch)});}
 catch(e){$('saved').textContent=e.message;}finally{masterEditing=false;}
}
$('master').oninput=()=>{masterEditing=true;$('masterValue').textContent=$('master').value+'%';clearTimeout(masterTimer);masterTimer=setTimeout(()=>masterUpdate({volume:Number($('master').value)}),100);};
$('mute').onclick=()=>masterUpdate({muted:$('mute').dataset.muted!=='true'});
async function masterPoll(){
 try{const v=await api('/api/volume');if(!masterEditing){$('master').value=v.volume;$('masterValue').textContent=v.volume+'%';}$('mute').dataset.muted=String(v.muted);$('mute').textContent=v.muted?'Unmute':'Mute';}
 catch{}setTimeout(masterPoll,1000);
}masterPoll();
