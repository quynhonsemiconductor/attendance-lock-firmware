#pragma once
// Trang quan tri nhung trong flash. Goi /api/* bang fetch, tu lam moi moi 2s.

const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="vi"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RFID Door</title>
<style>
 body{font-family:system-ui,sans-serif;margin:0;background:#f4f5f7;color:#222}
 header{background:#1f3a5f;color:#fff;padding:12px 16px;font-size:18px}
 main{max-width:900px;margin:auto;padding:12px}
 .card{background:#fff;border-radius:8px;padding:12px 16px;margin-bottom:12px;box-shadow:0 1px 3px #0002}
 h2{font-size:16px;margin:0 0 10px}
 table{width:100%;border-collapse:collapse;font-size:14px}
 th,td{text-align:left;padding:6px;border-bottom:1px solid #eee}
 input{padding:7px;border:1px solid #ccc;border-radius:4px;margin:2px 0}
 button{padding:7px 12px;border:0;border-radius:4px;background:#1f6feb;color:#fff;cursor:pointer}
 button.del{background:#d73a49} button.sec{background:#6a737d} button.on{background:#d97706}
 .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:8px}
 .k{font-size:12px;color:#666}.v{font-size:18px;font-weight:600}
 .ok{color:#1a7f37}.bad{color:#d73a49}
 .scroll{max-height:420px;overflow:auto}
 #msg{display:none;padding:8px 12px;border-radius:6px;margin-bottom:12px}
 #msg.err{display:block;background:#ffebe9;color:#a40e26}
 #msg.info{display:block;background:#dafbe1;color:#116329}
 #rm{display:none;background:#fff8c5;border:1px solid #d4a72c;border-radius:6px;padding:8px 12px;margin:8px 0}
 .flash{animation:fl 1s}@keyframes fl{from{background:#fff3b0}to{background:#fff}}
</style></head><body>
<header>RFID Door - Quan tri</header>
<main>
 <div id="msg"></div>

 <div class="card"><h2>Trang thai</h2>
  <div class="grid">
   <div><div class="k">Cua</div><div class="v" id="lock">-</div></div>
   <div><div class="k">Pin</div><div class="v" id="bat">-</div></div>
   <div><div class="k">Thoi gian</div><div class="v" id="time" style="font-size:14px">-</div></div>
   <div><div class="k">IP</div><div class="v" id="ip" style="font-size:14px">-</div></div>
  </div>
  <p><button onclick="post('/api/unlock')">Mo cua tu xa</button></p>
 </div>

 <div class="card"><h2>Them the</h2>
  <p class="k"><b>Cach 1 (nhanh):</b> nhap ten &rarr; bam "Dang ky the moi" &rarr; quet the vao dau doc VAO hoac RA. ESP32 tu luu, keu 3 tieng bip la xong.</p>
  <input id="enName" placeholder="Ten nguoi dung" size="22">
  <button id="enBtn" onclick="startEnroll()">Dang ky the moi</button>
  <div id="en" style="display:none;background:#fff8c5;border:1px solid #d4a72c;border-radius:6px;padding:8px 12px;margin:8px 0">
   Dang cho quet the cho <b id="enWho"></b> (cua KHONG mo). Con <b id="enLeft">60</b>s.
   <button class="del" onclick="post('/api/enroll',{on:'0'})">Huy</button></div>
  <hr style="border:0;border-top:1px solid #eee;margin:12px 0">
  <p class="k"><b>Cach 2 (thu cong):</b> B1: bam "Che do doc the" &rarr; B2: quet the vao dau doc VAO hoac RA (UID tu dien) &rarr; B3: nhap ten &rarr; Them</p>
  <button id="rmBtn" class="sec" onclick="toggleRead()">Che do doc the</button>
  <div id="rm">Dang doc the: quet the vao 1 trong 2 dau doc (cua KHONG mo trong che do nay). Tu tat sau <b id="rmLeft">60</b>s.</div>
  <p class="k">The vua quet: <b id="last">-</b> <button class="sec" onclick="useLast()">Dung UID nay</button></p>
  <input id="uid" placeholder="UID (vd DE:AD:BE:EF)" size="22">
  <input id="name" placeholder="Ten nguoi dung" size="22">
  <button onclick="addCard()">Them</button>
 </div>

 <div class="card"><h2>Danh sach the (<span id="cnt">0</span>)</h2>
  <table><thead><tr><th>UID</th><th>Ten</th><th></th></tr></thead><tbody id="cards"></tbody></table>
 </div>

 <div class="card"><h2>Lich su vao / ra</h2>
  <div class="scroll"><table><thead><tr><th>Thoi gian</th><th>Loai</th><th>UID</th><th>Ten</th><th>Ket qua</th></tr></thead>
  <tbody id="logs"></tbody></table></div>
 </div>
</main>
<script>
const $=id=>document.getElementById(id);
let lastUid='', seenSeq=-1, readMode=false, seenEnroll=-1;
function td(t,cls){const e=document.createElement('td');e.textContent=t;if(cls)e.className=cls;return e}
function msg(t,cls){const m=$('msg');m.textContent=t;m.className=t?cls:'';}
async function post(url,data){
  try{
    const r=await fetch(url,{method:'POST',credentials:'same-origin',body:new URLSearchParams(data||{})});
    const t=await r.text();
    if(r.ok) msg('','');
    else msg('Loi '+r.status+': '+(t||(r.status==401?'chua dang nhap - tai lai trang (F5)':'khong ro')),'err');
    refresh(); return r.ok;
  }catch(e){ msg('Khong ket noi duoc ESP32: '+e.message,'err'); return false; }
}
function useLast(){ if(lastUid){ $('uid').value=lastUid; $('name').focus(); } }
async function addCard(){
  const uid=$('uid').value.trim(), name=$('name').value.trim();
  if(!uid){ msg('Chua co UID - bam "Che do doc the" roi quet the, hoac go UID','err'); return; }
  if(await post('/api/cards',{uid,name})){ msg('Da them the '+uid+(name?' ('+name+')':''),'info'); $('uid').value=''; $('name').value=''; }
}
function delCard(uid){ if(confirm('Xoa the '+uid+'?')) post('/api/delete',{uid}); }
async function startEnroll(){
  const name=$('enName').value.trim();
  if(!name){ msg('Nhap ten truoc khi dang ky','err'); $('enName').focus(); return; }
  if(await post('/api/enroll',{on:'1',name})) msg('Quet the cho '+name+' vao 1 trong 2 dau doc...','info');
}
function toggleRead(){ post('/api/readmode',{on:readMode?'0':'1'}); }
async function getJson(u){ const r=await fetch(u,{credentials:'same-origin'}); if(!r.ok) throw new Error(u+' -> HTTP '+r.status); return r.json(); }
async function refresh(){
  try{
    const s=await getJson('/api/status');
    $('lock').textContent=s.lowBat?'MO (PIN YEU)':(s.locked?'Dang khoa':'Dang mo');
    $('lock').className='v '+(s.locked?'ok':'bad');
    $('bat').textContent=s.batEnabled?(s.batPct+'% ('+s.batV.toFixed(2)+'V)'):'tat';
    $('bat').className='v '+(s.lowBat?'bad':'ok');
    $('time').textContent=s.time; $('ip').textContent=s.ip;
    lastUid=s.lastUid;
    $('last').textContent=s.lastUid?(s.lastUid+' @'+s.lastReader+' ('+s.lastAgo+'s truoc)'+(s.lastName?' - DA DANG KY: '+s.lastName:' - chua dang ky')):'-';

    readMode=s.readMode;
    $('rm').style.display=readMode?'block':'none'; $('rmLeft').textContent=s.readLeft;
    $('rmBtn').textContent=readMode?'Tat che do doc the':'Che do doc the';
    $('rmBtn').className=readMode?'on':'sec';
    // Che do doc the: moi lan quet moi -> tu dien UID vao o nhap
    if(seenSeq>=0 && s.scanSeq!==seenSeq && readMode && s.lastUid){
      $('uid').value=s.lastUid; $('uid').classList.remove('flash'); void $('uid').offsetWidth; $('uid').classList.add('flash');
      msg(s.lastName?('The '+s.lastUid+' da dang ky: '+s.lastName):('Da doc '+s.lastUid+' tu dau doc '+s.lastReader+' - nhap ten roi bam Them'), s.lastName?'err':'info');
      if(!s.lastName) $('name').focus();
    }
    seenSeq=s.scanSeq;

    // Dang ky 1 cham
    $('en').style.display=s.enroll?'block':'none';
    $('enWho').textContent=s.enrollName; $('enLeft').textContent=s.enrollLeft;
    $('enBtn').disabled=s.enroll; $('enBtn').textContent=s.enroll?'Dang cho quet...':'Dang ky the moi';
    if(seenEnroll>=0 && s.enrollSeq!==seenEnroll && s.enrollMsg){
      const ok=s.enrollMsg.startsWith('Da them');
      msg(s.enrollMsg, ok?'info':'err');
      if(ok) $('enName').value='';
    }
    seenEnroll=s.enrollSeq;

    const c=await getJson('/api/cards');
    $('cnt').textContent=c.length; const cb=$('cards'); cb.innerHTML='';
    c.forEach(x=>{const tr=document.createElement('tr');tr.append(td(x.uid),td(x.name));
      const b=document.createElement('button');b.className='del';b.textContent='Xoa';b.onclick=()=>delCard(x.uid);
      const t=document.createElement('td');t.append(b);tr.append(t);cb.append(tr);});

    const l=await getJson('/api/logs'); const lb=$('logs'); lb.innerHTML='';
    l.forEach(x=>{const tr=document.createElement('tr');
      tr.append(td(x.time),td(x.type),td(x.uid),td(x.name),td(x.result,x.result=='granted'?'ok':'bad'));lb.append(tr);});
  }catch(e){ msg('Mat ket noi ESP32: '+e.message,'err'); }
}
refresh(); setInterval(refresh,2000);
</script></body></html>)HTML";
