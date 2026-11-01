"use strict";

const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => [...document.querySelectorAll(selector)];
let token = sessionStorage.getItem("rv1126-token") || "";
let state = {status: null, metrics: null, config: null, tool: "region", shapes: {regions: [], lines: []}, start: null};

function toast(message, error = false) {
  const node = $("#toast");
  node.textContent = message; node.className = error ? "show error" : "show";
  clearTimeout(toast.timer); toast.timer = setTimeout(() => node.className = "", 3200);
}

async function api(path, options = {}) {
  const headers = new Headers(options.headers || {});
  if (token) headers.set("Authorization", `Bearer ${token}`);
  const response = await fetch(path, {...options, headers});
  if (!response.ok) {
    let message = `${response.status} ${response.statusText}`;
    try { message = (await response.json()).error || message; } catch (_) {}
    if (response.status === 401) openToken();
    throw new Error(message);
  }
  return response;
}

const fmt = (value, digits = 1) => Number.isFinite(Number(value)) ? Number(value).toFixed(digits) : "--";
const metric = (name, value, unit, note) => `<article class="metric"><p class="eyebrow">${name}</p><span class="value">${value}</span><span class="unit">${unit}${note ? ` · ${note}` : ""}</span></article>`;

function render() {
  const s = state.status || {}, m = state.metrics || {};
  $("#connection").textContent = s.running ? "ONLINE" : "OFFLINE";
  $("#connection").className = `pill ${s.running ? "" : "bad"}`;
  $("#device-state").textContent = s.running ? "媒体管线正在运行" : "媒体管线已停止";
  $("#device-meta").textContent = `版本 ${s.version || "--"} · Git ${(s.git_commit || "--").slice(0, 12)} · PID ${s.pid || "--"}`;
  $("#metrics").innerHTML = [
    metric("CPU", fmt(m.cpu_total_percent), "%", "媒体进程"),
    metric("NPU", m.npu_load_percent == null ? "--" : fmt(m.npu_load_percent), "%", m.npu_frequency_mhz == null ? "驱动未提供负载" : `${fmt(m.npu_frequency_mhz,0)} MHz`),
    metric("CPU TEMP", fmt(m.cpu_c), "°C"), metric("NPU TEMP", fmt(m.npu_c), "°C"),
    metric("MEMORY", fmt(m.memory_used_percent), "%", `${fmt((m.memory_available_kb||0)/1024,0)} MiB free`),
    metric("RSS", fmt((m.rss_kb || 0) / 1024), "MiB"), metric("THREADS", m.threads ?? "--", "个"),
    metric("FD", m.fd_count ?? "--", "个"), metric("MAIN VENC", m.main_venc ?? "--", "frames"),
    metric("SUB VENC", m.sub_venc ?? "--", "frames"), metric("AI", m.ai_completed ?? "--", "inferences"),
    metric("OSD", m.osd_boxes ?? "--", "boxes"), metric("AUDIO", m.audio_sent ?? "--", "packets"),
    metric("ISPP ERR", m.errcnt ?? "--", "count")
  ].join("");
  const address = (s.addresses || []).find(x => x.interface === "wlan0")?.address || s.addresses?.[0]?.address || location.hostname;
  $("#streams").innerHTML = ["main", "sub"].map(name => {
    const url = `rtsp://${address}:${s.rtsp?.port || 8554}${s.rtsp?.[name] || `/live/${name}_stream`}`;
    return `<div class="stream"><div><p class="eyebrow">${name.toUpperCase()}</p><code>${url}</code></div><button class="ghost copy" data-value="${url}">复制</button></div>`;
  }).join("");
  $("#health-list").innerHTML = [["看门狗 PID",s.watchdog_pid||"--"],["看门狗重启",s.watchdog_restarts||0],["主流 PTS 缺口",m.main_pts_gap||0],["子流 PTS 缺口",m.sub_pts_gap||0],["背压恢复",m.backpressure_recoveries||0],["RKISPP 错误",m.errcnt||0]].map(x=>`<span>${x[0]}</span><strong>${x[1]}</strong>`).join("");
  $$(".copy").forEach(b => b.onclick = () => navigator.clipboard.writeText(b.dataset.value).then(()=>toast("已复制 RTSP 地址")));
}

async function refreshAll() {
  if (!token) return openToken();
  try {
    const [status, metrics, config] = await Promise.all([api("/api/status"), api("/api/metrics"), api("/api/config")]);
    state.status = await status.json(); state.metrics = await metrics.json(); state.config = await config.json();
    state.shapes = structuredClone(state.config.analytics || {regions: [], lines: []});
    fillConfig(); render(); drawShapes(); renderShapes();
  } catch (error) { toast(error.message, true); }
}

function fillConfig() {
  const media = state.config?.media || {};
  Object.entries(media).forEach(([key, value]) => { const input = $(`[name="${key}"]`); if (input) input.value = value; });
}

function openToken() { $("#token-input").value = token; $("#token-dialog").showModal(); }
$("#token-form").addEventListener("submit", event => {
  if (event.submitter?.value === "cancel") return;
  event.preventDefault(); token = $("#token-input").value.trim(); sessionStorage.setItem("rv1126-token", token);
  $("#token-dialog").close(); refreshAll();
});

$("#config-form").addEventListener("submit", async event => {
  event.preventDefault();
  const media = {}; new FormData(event.target).forEach((value,key)=> media[key] = key === "threshold" ? Number(value) : Number.parseInt(value,10));
  try {
    const response = await api("/api/config", {method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({media,analytics:state.shapes,apply:$("#apply-now").checked})});
    const result = await response.json(); state.config = result.config; toast(result.applied ? "配置已应用，管线已重启" : "配置已保存，下次重启生效"); setTimeout(refreshAll, result.applied ? 2500 : 0);
  } catch (error) { toast(error.message,true); }
});

function canvasPoint(event) { const r=$("#rule-canvas").getBoundingClientRect(); return {x:Math.max(0,Math.min(1,(event.clientX-r.left)/r.width)),y:Math.max(0,Math.min(1,(event.clientY-r.top)/r.height))}; }
const canvas=$("#rule-canvas"), ctx=canvas.getContext("2d");
function drawShapes(preview=null) {
  ctx.clearRect(0,0,canvas.width,canvas.height); ctx.lineWidth=4; ctx.font="22px sans-serif";
  [...state.shapes.regions.map(x=>({kind:"region",...x})),...state.shapes.lines.map(x=>({kind:"line",...x})),...(preview?[preview]:[])].forEach(s=>{
    ctx.strokeStyle=s.kind==="region"?"#35e5d0":"#f5b94c";ctx.fillStyle=ctx.strokeStyle;
    if(s.kind==="region")ctx.strokeRect(s.x*canvas.width,s.y*canvas.height,s.width*canvas.width,s.height*canvas.height);
    else{ctx.beginPath();ctx.moveTo(s.x1*canvas.width,s.y1*canvas.height);ctx.lineTo(s.x2*canvas.width,s.y2*canvas.height);ctx.stroke();}
    if(s.name)ctx.fillText(s.name,(s.x??s.x1)*canvas.width+8,(s.y??s.y1)*canvas.height+25);
  });
}
canvas.onpointerdown=e=>{state.start=canvasPoint(e);canvas.setPointerCapture(e.pointerId)};
canvas.onpointermove=e=>{if(!state.start)return;const p=canvasPoint(e);drawShapes(state.tool==="region"?{kind:"region",x:Math.min(state.start.x,p.x),y:Math.min(state.start.y,p.y),width:Math.abs(p.x-state.start.x),height:Math.abs(p.y-state.start.y)}:{kind:"line",x1:state.start.x,y1:state.start.y,x2:p.x,y2:p.y})};
canvas.onpointerup=e=>{if(!state.start)return;const p=canvasPoint(e),name=prompt(state.tool==="region"?"区域名称":"越线名称",`${state.tool==="region"?"区域":"越线"}${state.tool==="region"?state.shapes.regions.length+1:state.shapes.lines.length+1}`);if(name){if(state.tool==="region"){const s={name:name.slice(0,48),x:Math.min(state.start.x,p.x),y:Math.min(state.start.y,p.y),width:Math.abs(p.x-state.start.x),height:Math.abs(p.y-state.start.y)};if(s.width>.01&&s.height>.01)state.shapes.regions.push(s)}else state.shapes.lines.push({name:name.slice(0,48),x1:state.start.x,y1:state.start.y,x2:p.x,y2:p.y})}state.start=null;drawShapes();renderShapes()};
function renderShapes(){const items=[...state.shapes.regions.map((x,i)=>({type:"区域",key:"regions",i,x})),...state.shapes.lines.map((x,i)=>({type:"越线",key:"lines",i,x}))];$("#shape-list").innerHTML=items.length?items.map(v=>`<div class="list-row"><span><strong>${v.x.name}</strong><br><small class="muted">${v.type}</small></span><button class="ghost remove-shape" data-key="${v.key}" data-index="${v.i}">删除</button></div>`).join(""):'<div class="empty">尚未定义规则</div>';$$('.remove-shape').forEach(b=>b.onclick=()=>{state.shapes[b.dataset.key].splice(Number(b.dataset.index),1);drawShapes();renderShapes()})}
$$('[data-tool]').forEach(b=>b.onclick=()=>{$$('[data-tool]').forEach(x=>x.classList.remove('active'));b.classList.add('active');state.tool=b.dataset.tool});
$("#clear-shapes").onclick=()=>{state.shapes={regions:[],lines:[]};drawShapes();renderShapes()};
$("#save-shapes").onclick=async()=>{try{const media=state.config.media;const r=await api('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({media,analytics:state.shapes,apply:false})});state.config=(await r.json()).config;toast('规则坐标已保存')}catch(e){toast(e.message,true)}};

async function loadEvents(){try{const r=await api('/api/events?limit=100'),data=await r.json();$("#event-list").innerHTML=data.events.length?data.events.map(e=>`<div class="list-row"><span><strong>${e.type||e.event||'事件'}</strong><br><small class="muted">${e.time||e.timestamp||''}</small></span><code>${e.label||e.name||''}</code></div>`).join(''):'<div class="empty">暂无事件记录</div>'}catch(e){toast(e.message,true)}}
async function loadFiles(){try{const kind=$("#file-kind").value,r=await api(`/api/files?kind=${kind}`),data=await r.json();$("#file-list").innerHTML=data.files.length?data.files.map(f=>`<div class="list-row"><span><strong>${f.name}</strong><br><small class="muted">${(f.size/1048576).toFixed(2)} MiB</small></span><button class="ghost download-file" data-kind="${f.kind}" data-name="${encodeURIComponent(f.name)}">下载</button></div>`).join(''):'<div class="empty">暂无文件</div>';$$('.download-file').forEach(b=>b.onclick=()=>downloadAuthorized(`/api/files/download?kind=${b.dataset.kind}&name=${b.dataset.name}`,decodeURIComponent(b.dataset.name)))}catch(e){toast(e.message,true)}}
async function downloadAuthorized(path,name){try{const r=await api(path),blob=await r.blob(),a=document.createElement('a');a.href=URL.createObjectURL(blob);a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000)}catch(e){toast(e.message,true)}}
async function loadModels(){try{const r=await api('/api/models');$("#model-state").textContent=JSON.stringify(await r.json(),null,2)}catch(e){toast(e.message,true)}}
async function sha256(file){return [...new Uint8Array(await crypto.subtle.digest('SHA-256',await file.arrayBuffer()))].map(x=>x.toString(16).padStart(2,'0')).join('')}
$("#upload-model").onclick=async()=>{const files={rknn:$('#model-rknn').files[0],metadata:$('#model-metadata').files[0],labels:$('#model-labels').files[0]};if(Object.values(files).some(x=>!x))return toast('请选择完整的三个文件',true);try{for(const [kind,file] of Object.entries(files)){toast(`正在校验并上传 ${file.name}…`);await api(`/api/model/upload?kind=${kind}&sha256=${await sha256(file)}`,{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:file})}toast('模型组上传与 SHA-256 校验完成');loadModels()}catch(e){toast(e.message,true)}};
$("#activate-model").onclick=async()=>{if(!confirm('切换模型会重启媒体管线，是否继续？'))return;try{await api('/api/model/activate',{method:'POST',headers:{'Content-Type':'application/json'},body:'{"confirm":true}'});toast('模型已切换，管线健康检查通过');loadModels();refreshAll()}catch(e){toast(e.message,true)}};

async function loadSecurity(){
  try{
    const [securityResponse,otaResponse,historyResponse]=await Promise.all([api('/api/security/status'),api('/api/ota/status'),api('/api/ota/history?limit=20')]);
    const security=await securityResponse.json(),ota=await otaResponse.json(),history=await historyResponse.json();
    $("#security-state").innerHTML=[["管理令牌",security.token_active?"有效":"已失效"],["HTTPS",security.https_required?"必需":"未强制"],["云端凭证",security.cloud_credential_provisioned?"已配置":"未配置"],["凭证隔离",security.cloud_credential_separate?"是":"否"]].map(x=>`<span>${x[0]}</span><strong>${x[1]}</strong>`).join('');
    $("#ota-state").textContent=JSON.stringify({status:ota,history:history.history||[]},null,2);
  }catch(e){toast(e.message,true)}
}
$("#rotate-token").onclick=async()=>{if(!confirm('轮换后旧令牌立即失效，是否继续？'))return;try{const r=await api('/api/auth/token/rotate',{method:'POST',headers:{'Content-Type':'application/json'},body:'{"confirm":true}'}),d=await r.json();token=d.token;sessionStorage.setItem('rv1126-token',token);toast('令牌已轮换，新令牌已保存在当前会话');loadSecurity()}catch(e){toast(e.message,true)}};
$("#invalidate-token").onclick=async()=>{if(!confirm('失效后必须在板端执行 token-reset 才能重新登录，确认继续？'))return;try{await api('/api/auth/token/invalidate',{method:'POST',headers:{'Content-Type':'application/json'},body:'{"confirm":true}'});token='';sessionStorage.removeItem('rv1126-token');toast('管理令牌已失效');openToken()}catch(e){toast(e.message,true)}};
$("#upload-ota").onclick=async()=>{const file=$("#ota-bundle").files[0];if(!file)return toast('请选择 .rvota 包',true);try{toast('正在计算SHA-256并上传…');const r=await api(`/api/ota/upload?sha256=${await sha256(file)}`,{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:file});await r.json();toast('OTA包上传完成，请继续验签')}catch(e){toast(e.message,true)}};
$("#verify-ota").onclick=async()=>{try{toast('正在验证签名、设备型号和文件摘要…');await api('/api/ota/verify',{method:'POST'});toast('OTA包验签通过并已写入备用版本');loadSecurity()}catch(e){toast(e.message,true)}};
$("#apply-ota").onclick=async()=>{if(!confirm('切换期间Web和RTSP会短暂中断，失败将自动回滚。确认继续？'))return;try{await api('/api/ota/apply',{method:'POST',headers:{'Content-Type':'application/json'},body:'{"confirm":true}'});toast('已开始A/B切换，约60秒内恢复');setTimeout(()=>location.reload(),30000)}catch(e){toast(e.message,true)}};
$("#rollback-ota").onclick=async()=>{if(!confirm('确认回滚到上一个已验证版本？'))return;try{await api('/api/ota/rollback',{method:'POST',headers:{'Content-Type':'application/json'},body:'{"confirm":true}'});toast('已开始回滚');setTimeout(()=>location.reload(),30000)}catch(e){toast(e.message,true)}};

$$('.tab').forEach(b=>b.onclick=()=>{$$('.tab').forEach(x=>x.classList.remove('active'));$$('.tab-page').forEach(x=>x.classList.remove('active'));b.classList.add('active');$(`#${b.dataset.tab}`).classList.add('active');if(b.dataset.tab==='events'){loadEvents();loadFiles()}if(b.dataset.tab==='models')loadModels();if(b.dataset.tab==='security')loadSecurity()});
$("#auth").onclick=openToken;$("#refresh").onclick=refreshAll;$("#reload-events").onclick=loadEvents;$("#file-kind").onchange=loadFiles;$("#reload-models").onclick=loadModels;
$("#restart").onclick=async()=>{if(!confirm('确认重启媒体管线？RTSP 将短暂中断。'))return;try{await api('/api/restart',{method:'POST'});toast('管线重启和健康检查完成');setTimeout(refreshAll,2000)}catch(e){toast(e.message,true)}};
$("#logs").onclick=()=>downloadAuthorized('/api/logs/export','rv1126-logs.tar.gz');
$("#reload-security").onclick=loadSecurity;$("#reload-ota").onclick=loadSecurity;
if(token)refreshAll();else openToken();
setInterval(()=>{if(token&&!document.hidden)Promise.all([api('/api/status'),api('/api/metrics')]).then(async([s,m])=>{state.status=await s.json();state.metrics=await m.json();render()}).catch(()=>{})},5000);
