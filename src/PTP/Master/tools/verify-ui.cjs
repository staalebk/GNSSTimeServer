const fs=require('fs'),vm=require('vm'),assert=require('assert');
const html=fs.readFileSync('src/PTP/Master/main/index.html','utf8');
const ids=[...html.matchAll(/id="([^"]+)"/g)].map(m=>m[1]);
assert.equal(new Set(ids).size,ids.length,'unique IDs');
const nodes={};
function element(){return {textContent:'',style:{},classList:{add(){},remove(){}},addEventListener(){},setAttribute(){},appendChild(v){this.firstChild ||=v;},replaceChildren(){this.firstChild=null;},getContext(){return new Proxy({}, {get:()=>()=>{}});}};}
ids.forEach(id=>nodes[id]=element());
let js=html.match(/<script>([\s\S]*?)<\/script>/)[1].replace('" SP_STR(HEALTH_POLL_INTERVAL_MS) "','1000');
js=js.replace(/\npoll\(\);\nsetInterval\(poll, 5000\);/,'');
let monotonicMs = 100;
const ctx=vm.createContext({performance:{now:()=>monotonicMs},document:{getElementById:id=>{assert(nodes[id],id);return nodes[id];},createElement:element},console,setInterval:()=>1,clearInterval(){},fetch:async()=>({ok:true,json:async()=>({})})});
vm.runInContext(js,ctx);
vm.runInContext('render({tasks:[]})',ctx);
assert.equal(nodes.clockUtc.textContent,'--:--:--.-');
vm.runInContext(`render({gnss_fix:true,clock_utc_ms:1792584000123,gnss_epoch:1792584000,tai_offset:37,signal_age_ms:100,used_age_ms:100,satellites_used:4,satellites_visible:8,satellites:[{system:'GPS',prn:1,elevation:45,azimuth:180,cn0:40}],tasks:[]},25);showPanel(true);`,ctx);
assert.equal(nodes.statusPanel.hidden,true);
assert.equal(nodes.systemPanel.hidden,false);
assert.equal(nodes.satSummary.textContent,'4 used / 8 in view');
assert(nodes.clockDate.textContent.includes('UTC'));
assert(nodes.clockUtc.textContent.endsWith('.1'), 'fraction and latency correction');
monotonicMs += 1234;
vm.runInContext('tickClock()',ctx);
assert.equal(nodes.clockUtc.textContent,new Date(1792584000123+25+1234).toISOString().slice(11,21),'clock advances without polling');
// Verify date rollover and expiry using monotonic elapsed time.
vm.runInContext(`renderReference({clock_utc_ms:Date.parse('2026-10-21T23:59:59.900Z')})`,ctx);
monotonicMs += 200;
vm.runInContext('tickClock()',ctx);
assert.equal(nodes.clockUtc.textContent,'00:00:00.1');
assert(nodes.clockDate.textContent.startsWith('2026-10-22'));
monotonicMs += 15000;
vm.runInContext('tickClock()',ctx);
assert.equal(nodes.clockUtc.textContent,'--:--:--.-');
vm.runInContext('render({gnss_fix:false,signal_age_ms:15000,used_age_ms:15000,tasks:[]});showPanel(false);',ctx);
assert.equal(nodes.clockUtc.textContent,'--:--:--.-');
assert.equal(nodes.satSummary.textContent,'— used / — in view');
assert.equal(nodes.systemPanel.hidden,true);
console.log('PASS: JavaScript syntax, all render IDs, missing/live/stale data, satellite rendering, panel switching');

vm.runInContext('renderHistory({ranges:[{step_s:1,samples:[null,[0,-100,-90],[4,-120,-80]]},{step_s:5,samples:[[2,10,10]]},{step_s:900,samples:[null]}]})',ctx);
assert(nodes.historyStatus0.innerHTML.includes('#bd93f9'),'worst status color');
assert(nodes.historyFreq0.innerHTML.includes('-120 ppb'),'frequency minimum');
assert(nodes.historyFreq0.innerHTML.includes('-80 ppb'),'frequency maximum');
assert(!nodes.historyFreq2.innerHTML.includes('NaN'),'empty history');
console.log('PASS: history gaps, severity colors, frequency extremes and empty range');

vm.runInContext('renderHistory({ranges:[{step_s:1,samples:[[1,null,null],[0,-51200,-51190],[0,-51195,null]]},{step_s:5,samples:[[1,null,null],[0,-51200,-51190]]},{step_s:900,samples:[[1,null,null]]}]})',ctx);
for (const r of [0,1]) {
  assert(nodes['historyFreq'+r].innerHTML.includes('-51190 ppb'),'masked frequency excluded from upper axis');
  assert.equal((nodes['historyFreq'+r].innerHTML.match(/<rect /g)||[]).length,1,'masked frequency is a gap');
}
assert(nodes.historyStatus0.innerHTML.includes('frequency unavailable'),'masked status remains visible');
assert(!nodes.historyFreq2.innerHTML.includes('<rect '),'entirely masked frequency stays empty');
vm.runInContext('renderHistory({ranges:[{step_s:1,samples:[[0,0,0]]}]})',ctx);
assert(nodes.historyFreq0.innerHTML.includes('0 to 0 ppb'),'genuine zero adjustment retained');
console.log('PASS: masked frequency gaps, axis limits, status and genuine zero');

vm.runInContext('renderHistory({ranges:[{step_s:1,samples:[null,[0,null,null,12,18,5,8,20,48,34.5],[0,0,0,null,null,0,0,null,null,null]]},{step_s:5,samples:[null]},{step_s:900,samples:[null]}]})',ctx);
assert(nodes.historySats0.innerHTML.includes('In view: 12 to 18'),'visible range');
assert(nodes.historySats0.innerHTML.includes('In use: 0 to 0'),'zero in use retained');
assert(nodes.historySignal0.innerHTML.includes('average 34.5'),'signal average');
assert(!nodes.historySignal1.innerHTML.includes('NaN'),'empty GNSS range');
assert(!html.includes(' ? ' + '<span'),'no damaged legend separators');
assert(!html.includes('minimum?maximum'),'no damaged range separator');
console.log('PASS: GNSS ranges, zero counts, signal average, missing data and separators');

// Announce fields must preserve wire codes and handle unsent/unknown quality.
vm.runInContext('render({announce_sent:true,clock_class:7,clock_accuracy:0x22,ptp_udp_ipv4:true,tasks:[]})',ctx);
assert.equal(nodes.v_class.textContent,7);
assert.equal(nodes.v_accuracy.textContent,'\u2264 250 ns');
assert(nodes.ptpTransport.textContent.includes('UDP/IPv4'));
assert.equal(nodes.v_mcast.textContent,'224.0.1.129');
vm.runInContext('render({announce_sent:true,clock_class:248,clock_accuracy:0xfe,ptp_udp_ipv4:false,tasks:[]})',ctx);
assert.equal(nodes.v_accuracy.textContent,'Unknown');
assert.equal(nodes.v_mcast.textContent,'01:1B:19:00:00:00');
vm.runInContext('render({announce_sent:false,tasks:[]})',ctx);
assert.equal(nodes.v_class.textContent,'\u2014');
assert.equal(nodes.v_accuracy.textContent,'\u2014');
console.log('PASS: announced class/accuracy, unknown and unsent quality, UDP and Ethernet transport');
