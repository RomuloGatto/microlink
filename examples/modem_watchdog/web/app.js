const $=id=>document.getElementById(id);
let busy=false,settings={},healthTimer=null,countTimer=null,stateTickTimer=null,baseline='',stateName='',stateLabel='',stateRemaining=0,stateSyncMs=0,otaFile=null;
function formatCountdown(sec){sec=Math.max(0,Math.ceil(sec));const m=Math.floor(sec/60),s=sec%60;return String(m).padStart(2,'0')+':'+String(s).padStart(2,'0')}
function renderState(){const el=$('st_state');if(!el)return;let html=stateLabel||stateName||'-';if(stateName==='waiting-for-modem'&&stateRemaining>0){const elapsed=Math.floor((Date.now()-stateSyncMs)/1000),left=Math.max(0,stateRemaining-elapsed);html+="<span class='state-countdown'>"+formatCountdown(left)+"</span>";if(left===0)setTimeout(loadHealth,250)}el.innerHTML=html;el.classList.toggle('state-warn',stateName!=='monitoring')}
function toast(msg,type){const el=$('toast');el.textContent=msg;el.className='toast show '+(type||'success');clearTimeout(el._t);el._t=setTimeout(()=>el.className='toast',3000)}
function dot(ok,label){return "<span class='dot "+(ok?'ok':'bad')+"'></span>"+label}
function setv(id,v){$(id).value=v==null?'':v}
function modalClose(){if(!busy)$('overlay').classList.remove('show')}
function modalOpen(){$('overlay').classList.add('show')}
function showConfirm(){
 busy=false;otaFile=null;$('modal_symbol').className='modal-symbol';$('modal_title').textContent='Restart modem?';
 $('confirm').textContent='Restart';$('confirm').onclick=doReboot;$('confirm').className='modal-btn danger';$('count_suffix').textContent='s';
 $('modal_desc').textContent='The ESP will cut modem power for '+Number($('modem_off_s').value||20)+' seconds and turn it back on.';
 $('countdown').classList.remove('show');$('modal_actions').style.display='flex';$('cancel').style.display='';$('confirm').style.display='';modalOpen()
}
function showApplying(wifiChanged){
 busy=true;$('modal_symbol').className='modal-symbol blue';$('modal_title').textContent='Applying settings';
 $('modal_desc').textContent=wifiChanged?'The ESP will restart and validate the new network. If it fails, the previous network will be restored.':'The ESP will restart to apply the changes.';
 $('modal_actions').style.display='none';$('countdown').classList.add('show');$('count_suffix').textContent='s';$('count_label').textContent='waiting for the ESP to come back...';
 let left=5;$('count_value').textContent=left;$('progress').style.width='0%';
 clearInterval(countTimer);countTimer=setInterval(()=>{left--;if(left<0)left=0;$('count_value').textContent=left;$('progress').style.width=((5-left)/5*100)+'%';if(left===0){clearInterval(countTimer);waitForReturn(0)}},1000);modalOpen()
}
async function waitForReturn(attempt){
 try{const r=await fetch('/health',{cache:'no-store'});if(r.ok){location.reload();return}}catch(e){}
 if(attempt>=20){busy=false;$('modal_title').textContent='ESP is still not responding';$('modal_desc').textContent='The settings were saved. If Wi-Fi changed, the IP address may have changed as well.';$('countdown').classList.remove('show');$('modal_actions').style.display='flex';$('cancel').style.display='none';$('confirm').textContent='Close';$('confirm').onclick=()=>location.reload();return}
 setTimeout(()=>waitForReturn(attempt+1),2000)
}
async function loadSettings(){
 const r=await fetch('/api/config',{cache:'no-store'});if(!r.ok)throw new Error('settings '+r.status);const j=await r.json();settings=j;
 ['wifi_ssid','device_name','ctrl_host','subnet_route','priority_peer_ip','check_interval_s','failures_before_reboot','modem_off_s','modem_boot_s','max_auto_reboots','reboot_window_s'].forEach(k=>setv(k,j[k]));
 $('ctrl_tls').checked=!!j.ctrl_tls;$('subnet_enabled').checked=!!j.subnet_enabled;$('ctrl_tls').disabled=!j.tls_supported;$('subnet_enabled').disabled=!j.subnet_supported;
 $('wifi_password').placeholder=j.wifi_password_configured?'Configured - leave blank to keep':'Enter password';
 $('auth_key').placeholder=j.auth_key_configured?'Configured - leave blank to keep':'Enter auth key';
 $('off_note').textContent=j.modem_off_s||20;baseline=JSON.stringify(payload());$('save').disabled=true
}
async function loadHealth(){
 try{
  const r=await fetch('/health',{cache:'no-store'});if(!r.ok)return;const h=await r.json();
  if(h.rescue_ap_active){$('st_wifi').textContent='setup';$('st_local').textContent=h.rescue_ap_ip||'192.168.4.1'}else{$('st_wifi').innerHTML=dot(!!h.wifi,h.wifi?'connected':'desconnected');$('st_local').textContent=h.local_ip||'-'}$('st_ts').innerHTML=dot(!!h.tailscale_connected,h.tailscale_connected?'connected':'desconnected');
  $('st_ip').textContent=h.tailscale_ip||'-';$('st_fail').textContent=String(h.failures)+' / '+String(h.failures_limit);
  $('st_reboots').textContent=String(h.auto_reboots)+' / '+String(h.auto_reboots_limit);stateName=h.state||'';stateLabel=h.state_label||h.state||'-';stateRemaining=Number(h.state_remaining_s||0);stateSyncMs=Date.now();renderState();
  const pn=$('pending');if(h.rescue_ap_active){pn.textContent='Modo de setup: conecte este ESP a uma rede Wi-Fi e salve as configuracoes.';pn.classList.add('show')}else{pn.textContent='Nova rede Wi-Fi em validacao. Se ela nao conectar, a setup anterior sera restaurada automaticamente.';pn.classList.toggle('show',!!h.wifi_pending)}if(h.modem_off_s)$('off_note').textContent=h.modem_off_s
 }catch(e){}
}
function payload(){
 return {wifi_ssid:$('wifi_ssid').value.trim(),wifi_password:$('wifi_password').value,device_name:$('device_name').value.trim(),auth_key:$('auth_key').value.trim(),
 ctrl_host:$('ctrl_host').value.trim(),ctrl_tls:$('ctrl_tls').checked,noise_pubkey:'',subnet_enabled:$('subnet_enabled').checked,
 subnet_route:$('subnet_route').value.trim(),priority_peer_ip:$('priority_peer_ip').value.trim(),check_interval_s:Number($('check_interval_s').value),
 failures_before_reboot:Number($('failures_before_reboot').value),modem_off_s:Number($('modem_off_s').value),modem_boot_s:Number($('modem_boot_s').value),
 max_auto_reboots:Number($('max_auto_reboots').value),reboot_window_s:Number($('reboot_window_s').value)}
}
function syncDirty(){if(!baseline||busy)return;$('save').disabled=JSON.stringify(payload())===baseline}
async function save(ev){
 ev.preventDefault();if(busy)return;$('save').disabled=true;
 try{const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload())});const j=await r.json();if(!r.ok||!j.ok)throw new Error(j.error||'Failed to save');toast('Settings saved','success');showApplying(!!j.wifi_changed)}
 catch(e){toast(e.message||'Failed to save','error');$('save').disabled=false}
}
function showOtaPicker(){if(busy)return;$('ota_file').value='';$('ota_file').click()}
function showOtaConfirm(file){if(!file)return;otaFile=file;busy=false;$('modal_symbol').className='modal-symbol blue';$('modal_title').textContent='Update firmware?';$('modal_desc').textContent='Arquivo: '+file.name+' ('+(file.size/1024/1024).toFixed(2)+' MB). The ESP will restart after flashing.';$('countdown').classList.remove('show');$('modal_actions').style.display='flex';$('cancel').style.display='';$('confirm').style.display='';$('confirm').textContent='Update';$('confirm').className='modal-btn';$('confirm').onclick=doOta;modalOpen()}
function otaFailure(msg){busy=false;$('countdown').classList.remove('show');$('modal_actions').style.display='flex';$('cancel').style.display='none';$('confirm').style.display='';$('confirm').textContent='Close';$('confirm').className='modal-btn';$('confirm').onclick=()=>{otaFile=null;$('overlay').classList.remove('show')};$('modal_title').textContent='Update failed';$('modal_desc').textContent=msg||'The firmware could not be updated.'}
async function waitForOtaReturn(attempt){try{const r=await fetch('/health',{cache:'no-store'});if(r.ok){location.reload();return}}catch(e){}if(attempt>=100){busy=false;$('countdown').classList.remove('show');$('modal_actions').style.display='flex';$('cancel').style.display='none';$('confirm').style.display='';$('confirm').textContent='Reload';$('confirm').onclick=()=>location.reload();$('modal_title').textContent='ESP is still not responding';$('modal_desc').textContent='The firmware was uploaded, but the ESP is still not responding.';return}setTimeout(()=>waitForOtaReturn(attempt+1),2000)}
function doOta(){if(!otaFile||busy)return;busy=true;$('modal_actions').style.display='none';$('countdown').classList.add('show');$('count_suffix').textContent='%';$('count_value').textContent='0';$('count_label').textContent='uploading firmware...';$('progress').style.width='0%';$('modal_title').textContent='Updating firmware';$('modal_desc').textContent='Do not power off the ESP while flashing.';const xhr=new XMLHttpRequest();xhr.open('POST','/api/ota');xhr.setRequestHeader('Content-Type','application/octet-stream');xhr.upload.onprogress=e=>{if(e.lengthComputable){const p=Math.max(0,Math.min(100,Math.round(e.loaded/e.total*100)));$('count_value').textContent=p;$('progress').style.width=p+'%'}};xhr.onload=()=>{if(xhr.status>=200&&xhr.status<300){$('count_value').textContent='100';$('progress').style.width='100%';$('count_label').textContent='restarting ESP...';$('modal_desc').textContent='Firmware flashed. Waiting for the ESP to come back.';setTimeout(()=>waitForOtaReturn(0),2500)}else{otaFailure(xhr.responseText||('HTTP '+xhr.status))}};xhr.onerror=()=>otaFailure('Connection interrupted during upload. The current firmware remains active.');xhr.send(otaFile)}
async function doReboot(){
 if(busy)return;busy=true;$('modal_actions').style.display='none';$('countdown').classList.add('show');$('modal_title').textContent='Restarting modem';$('modal_desc').textContent='Command sent to the ESP.';
 const total=Math.max(1,Number($('modem_off_s').value||20));let left=total;$('count_suffix').textContent='s';$('count_value').textContent=left;$('count_label').textContent='restoring power in...';$('progress').style.width='0%';
 try{const r=await fetch('/api/reboot',{method:'POST'});if(!r.ok)throw new Error(await r.text());
 countTimer=setInterval(()=>{left--;if(left<0)left=0;$('count_value').textContent=left;$('progress').style.width=((total-left)/total*100)+'%';if(left===0){clearInterval(countTimer);location.reload()}},1000)}
 catch(e){busy=false;$('countdown').classList.remove('show');$('modal_actions').style.display='flex';toast('Failed to restart modem','error')}
}
document.querySelectorAll('[data-eye]').forEach(b=>b.onclick=()=>{const i=$(b.getAttribute('data-eye'));i.type=i.type==='password'?'text':'password'});
$('form').addEventListener('submit',save);$('form').addEventListener('input',syncDirty);$('form').addEventListener('change',syncDirty);$('reboot').onclick=showConfirm;$('ota').onclick=showOtaPicker;$('ota_file').addEventListener('change',e=>showOtaConfirm(e.target.files&&e.target.files[0]));$('cancel').onclick=modalClose;$('confirm').onclick=doReboot;$('overlay').onclick=e=>{if(e.target===$('overlay'))modalClose()};document.addEventListener('keydown',e=>{if(e.key==='Escape')modalClose()});
$('modem_off_s').addEventListener('input',()=>{$('off_note').textContent=$('modem_off_s').value||20});
Promise.all([loadSettings(),loadHealth()]).catch(e=>toast('Failed to load dashboard','error'));healthTimer=setInterval(loadHealth,10000);stateTickTimer=setInterval(renderState,1000);
