#pragma once
#include <Arduino.h>

// Pagina's in PROGMEM. Bewust klein en zonder frameworks (RAM is krap).
// De volwaardige web-UI komt in fase 4; dit is de testpagina + herstelpad.

static const char SQ_STYLE[] PROGMEM = R"css(
<meta name=viewport content="width=device-width,initial-scale=1">
<style>
:root{--bg:#f4f6f5;--card:#fff;--fg:#1d2421;--mute:#66706b;--acc:#1f7a52;--red:#c0362c;--line:#dde3e0}
@media(prefers-color-scheme:dark){:root{--bg:#121614;--card:#1c2220;--fg:#e6ece9;--mute:#94a09a;--acc:#3fb47c;--red:#e5544a;--line:#2c3431}}
*{box-sizing:border-box}body{margin:0;font:16px/1.4 system-ui,sans-serif;background:var(--bg);color:var(--fg)}
main{max-width:640px;margin:auto;padding:16px}h1{font-size:1.4em;margin:0 0 4px}small,.mute{color:var(--mute)}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px;margin:12px 0}
button,input{font:inherit;border-radius:8px;border:1px solid var(--line);padding:8px 12px;background:var(--card);color:var(--fg)}
button{cursor:pointer}button.p{background:var(--acc);border-color:var(--acc);color:#fff}
.stop{width:100%;background:var(--red);border-color:var(--red);color:#fff;font-size:1.3em;font-weight:700;padding:16px}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}.grow{flex:1}
.badge{font-size:.8em;padding:2px 8px;border-radius:99px;background:var(--line)}.badge.open{background:var(--acc);color:#fff}
#msg{position:fixed;left:16px;right:16px;bottom:16px;max-width:608px;margin:auto;padding:12px;border-radius:8px;background:var(--fg);color:var(--bg);display:none}
a{color:var(--acc)}input[type=number]{width:5.5em}
.pin{margin:6px 0 0}.pin b{font-family:ui-monospace,monospace;background:var(--line);padding:1px 6px;border-radius:4px}
table{border-collapse:collapse;width:100%;margin:10px 0}td,th{text-align:left;padding:6px 4px;border-bottom:1px solid var(--line)}
td:first-child{font-family:ui-monospace,monospace;font-weight:700}summary{cursor:pointer}
</style>)css";

// Gedeeld: inloggen, API-aanroep, melding.
static const char SQ_SCRIPT[] PROGMEM = R"js(
<script>
const $=s=>document.querySelector(s);
function msg(t){const m=$('#msg');m.textContent=t;m.style.display='block';clearTimeout(msg.t);msg.t=setTimeout(()=>m.style.display='none',4000)}
async function api(path,method='GET',body){
  const r=await fetch('/api'+path,{method,headers:body?{'Content-Type':'application/json'}:{},body:body?JSON.stringify(body):undefined});
  if(r.status==401&&path!='/login'){showLogin();throw new Error('login')}
  const j=await r.json().catch(()=>({}));
  if(!r.ok){msg(j.message||('Fout '+r.status));throw new Error(j.error)}
  return j}
function showLogin(){$('#login').style.display='block';$('#app').style.display='none'}
async function doLogin(e){e.preventDefault();
  try{await api('/login','POST',{password:$('#pw').value});$('#pw').value='';$('#login').style.display='none';$('#app').style.display='block';start()}catch(_){}}
async function logout(){await api('/logout','POST').catch(()=>{});showLogin()}
</script>)js";

static const char SQ_LOGIN[] PROGMEM = R"html(
<form id=login class=card style=display:none onsubmit=doLogin(event)>
<p><b>Inloggen</b></p><div class=row><input id=pw class=grow type=password placeholder="Admin-wachtwoord" autocomplete=current-password required><button class=p>Inloggen</button></div></form>
<div id=msg></div>)html";

static const char INDEX_TOP[] PROGMEM = "<!doctype html><html lang=nl><head><meta charset=utf-8><title>sqwrtr</title>";
static const char INDEX_MID[] PROGMEM = "</head><body><main><h1>sqwrtr</h1><small id=info>&nbsp;</small>";
static const char INDEX_BODY[] PROGMEM = R"html(
<div id=app style=display:none>
<div class=card><button class=stop onclick="api('/stop','POST').then(load)">NOODSTOP</button></div>
<div id=zones></div>
<details class=card><summary><b>Aansluitingen relaisbord</b></summary>
<table><tr><th>ESP-bord</th><th>Relaisbord</th></tr>
<tr><td>D1</td><td>IN1 (zone 1)</td></tr><tr><td>D2</td><td>IN2 (zone 2)</td></tr>
<tr><td>D5</td><td>IN3 (zone 3)</td></tr><tr><td>D6</td><td>IN4 (zone 4)</td></tr>
<tr><td>D7</td><td>IN5 (zone 5)</td></tr><tr><td>3V3</td><td>VCC</td></tr>
<tr><td>5 V buck (bureau: VIN)</td><td>JD-VCC <small>(jumper eraf)</small></td></tr>
<tr><td>GND</td><td>GND</td></tr></table>
<p class=mute>Niet gebruiken: <b>D8</b> (boot faalt), <b>D3</b> = FLASH-knop, <b>D4</b> = LED.
Gereserveerd: D0 hoofdklep/pomp (IN6), RX flowmeter, A0 regensensor.</p></details>
<p class=mute>Testpagina (fase 3) · <a href=/update>Firmware-update</a> · <a href=# onclick=logout()>Uitloggen</a></p>
</div></main>)html";
static const char INDEX_JS[] PROGMEM = R"js(<script>
let zones=[];
function fmt(s){return s>=60?Math.floor(s/60)+' min '+(s%60)+' s':s+' s'}
function render(st){
  const el=$('#zones');
  if(el.children.length!=st.zones.length){el.innerHTML=st.zones.map(z=>`<div class=card id=z${z.id}>
<div class=row><b class=grow>${z.id}. <span class=n></span></b><span class=badge></span></div>
<p class=pin>Pin <b>${z.pin}</b> <small>(GPIO${z.gpio})</small> → relais <b>IN${z.relay}</b></p>
<p class="mute r">&nbsp;</p>
<div class=row><input type=number min=1 value=1 id=m${z.id}> min
<button class=p onclick="openZ(${z.id},true)">Open</button><button onclick="openZ(${z.id},false)">Open ∞</button>
<button onclick="api('/zones/${z.id}/close','POST').then(load)">Sluit</button></div></div>`).join('')}
  for(const z of st.zones){const c=$('#z'+z.id);c.querySelector('.n').textContent=z.name;
    const b=c.querySelector('.badge');b.textContent={open:'open',pending:'wacht',closed:'dicht'}[z.state];b.className='badge '+z.state;
    c.querySelector('.r').textContent=z.state=='closed'?' ':'Nog '+fmt(z.remaining_s)}}
async function openZ(id,timed){const body=timed?{duration_s:Math.round($('#m'+id).value*60)}:{};
  await api('/zones/'+id+'/open','POST',body).catch(()=>{});load()}
async function load(){try{render(await api('/status'))}catch(_){}}
async function start(){try{const i=await api('/info');$('#info').textContent='v'+i.version+' · '+i.ip+' · RSSI '+i.rssi+' dBm'}catch(_){return}
  $('#app').style.display='block';load();clearInterval(start.t);start.t=setInterval(load,2000)}
start();
</script></body></html>)js";

static const char UPDATE_TOP[] PROGMEM = "<!doctype html><html lang=nl><head><meta charset=utf-8><title>sqwrtr update</title>";
static const char UPDATE_MID[] PROGMEM = "</head><body><main><h1>Firmware-update</h1><small id=info>&nbsp;</small>";
static const char UPDATE_BODY[] PROGMEM = R"html(
<div id=app style=display:none>
<div class=card><p><b>Firmware</b> <small>(.pio/build/…/firmware.bin)</small></p>
<div class=row><input type=file id=fw accept=.bin class=grow><button class=p onclick="up('fw')">Uploaden</button></div></div>
<div class=card><p><b>Bestandssysteem</b> <small>(littlefs.bin)</small></p>
<div class=row><input type=file id=fs accept=.bin class=grow><button class=p onclick="up('fs')">Uploaden</button></div></div>
<div class=card><progress id=pr max=100 value=0 style=width:100%></progress><p id=st class=mute>Alleen mogelijk als alle kleppen dicht zijn.</p></div>
<p class=mute><a href=/>Terug</a></p></div></main>)html";
static const char UPDATE_JS[] PROGMEM = R"js(<script>
function up(type){const f=$('#'+type).files[0];if(!f){msg('Kies eerst een bestand');return}
  const fd=new FormData();fd.append('file',f,f.name);const x=new XMLHttpRequest();
  x.upload.onprogress=e=>{$('#pr').value=e.loaded/e.total*100;$('#st').textContent='Uploaden… '+Math.round(e.loaded/e.total*100)+'%'};
  x.onload=()=>{let j={};try{j=JSON.parse(x.responseText)}catch(_){}
    if(x.status==200){$('#st').textContent='Geslaagd, herstarten… pagina laadt over 15 s opnieuw.';setTimeout(()=>location.href='/',15000)}
    else if(x.status==401)showLogin();else{$('#st').textContent='Mislukt: '+(j.message||x.status);$('#pr').value=0}};
  x.onerror=()=>{$('#st').textContent='Verbinding verbroken'};
  x.open('POST','/update?type='+type);x.send(fd)}
async function start(){try{const i=await api('/info');$('#info').textContent='Huidige versie v'+i.version}catch(_){return}$('#app').style.display='block'}
start();
</script></body></html>)js";

// Een pagina = reeks PROGMEM-blokken, achter elkaar verstuurd (gedeelde blokken staan maar één keer in flash).
static const char* const INDEX_PAGE[] = {INDEX_TOP, SQ_STYLE, INDEX_MID, SQ_LOGIN, INDEX_BODY, SQ_SCRIPT, INDEX_JS};
static const char* const UPDATE_PAGE[] = {UPDATE_TOP, SQ_STYLE, UPDATE_MID, SQ_LOGIN, UPDATE_BODY, SQ_SCRIPT, UPDATE_JS};
