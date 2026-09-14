import fs from 'node:fs';
import path from 'node:path';
import {spawn} from 'node:child_process';
// Node >=22; headed Chrome with an isolated profile. This is a diagnostic probe,
// not a pass/fail test: a site can reject a request independently of decoding.
// Usage: node tests/probe-playback.mjs DRIVER_DIR URL_OR_FILE RUN_DIR [SECONDS] [RATE]
// NVD_PROBE_SOFTWARE=1 disables accelerated decoding for a control run.
import {pathToFileURL} from 'node:url';
const [driverDir,input,runDir,seconds='480',rate='2']=process.argv.slice(2);
if(!driverDir||!input||!runDir)throw Error('Expected DRIVER_DIR URL_OR_FILE RUN_DIR [SECONDS] [RATE]');
if(!Number.isFinite(Number(seconds))||Number(seconds)<=0||Number(seconds)>3600||
   !Number.isFinite(Number(rate))||Number(rate)<0.25||Number(rate)>16)throw Error('Invalid duration or rate');
if(fs.existsSync(path.join(runDir,'profile')))throw Error('Use a fresh RUN_DIR for each probe');
const url=/^https?:/.test(input)?input:pathToFileURL(path.resolve(input)).href;
fs.mkdirSync(runDir,{recursive:true});
const profile=path.resolve(runDir,'profile'),events=path.resolve(runDir,'events.jsonl');
const append=(data)=>fs.appendFileSync(events,JSON.stringify({at:new Date().toISOString(),...data})+'\n');
const env={...process.env,LIBVA_DRIVER_NAME:'nvidia',LIBVA_DRIVERS_PATH:path.resolve(driverDir),NVD_BACKEND:'direct',NVD_EXPORT_LAYOUT:'packed',NVD_LOG:path.resolve(runDir,'driver.log'),NVD_LOG_VERBOSE:'1'};
const args=[`--user-data-dir=${profile}`,'--remote-debugging-port=0','--no-first-run','--no-default-browser-check','--autoplay-policy=no-user-gesture-required','--enable-features=AcceleratedVideoDecodeLinuxGL,VaapiOnNvidiaGPUs','--ignore-gpu-blocklist','--use-gl=angle','--use-angle=gl','--ozone-platform=wayland','about:blank'];
if(process.env.NVD_PROBE_SOFTWARE==='1')args.push('--disable-accelerated-video-decode');
const log=fs.openSync(path.resolve(runDir,'chrome.log'),'w');
const chrome=spawn('google-chrome-stable',args,{env,stdio:['ignore',log,log]});
let ws;
try {
 const active=path.join(profile,'DevToolsActivePort');
 for(let i=0;!fs.existsSync(active)&&i<150;i++)await new Promise(r=>setTimeout(r,100));
 const port=fs.readFileSync(active,'utf8').split('\n')[0];
 const targets=await(await fetch(`http://127.0.0.1:${port}/json/list`)).json();
 const target=targets.find(t=>t.type==='page');
 fs.writeFileSync(path.resolve(runDir,'cdp.json'),JSON.stringify({port,ws:target.webSocketDebuggerUrl,pid:chrome.pid}));
 ws=new WebSocket(target.webSocketDebuggerUrl);await new Promise((r,j)=>{ws.onopen=r;ws.onerror=j});
 let serial=0;const pending=new Map();
 const send=(method,params={})=>new Promise((resolve,reject)=>{const id=++serial;pending.set(id,{resolve,reject});ws.send(JSON.stringify({id,method,params}))});
 ws.onmessage=e=>{const m=JSON.parse(e.data);if(m.id){const p=pending.get(m.id);if(p){pending.delete(m.id);m.error?p.reject(Error(JSON.stringify(m.error))):p.resolve(m.result)}}else if(m.method?.startsWith('Media.')||m.method==='Inspector.targetCrashed')append(m)};
 ws.onclose=()=>{for(const p of pending.values())p.reject(Error('CDP closed'));pending.clear()};
 await send('Runtime.enable');await send('Media.enable');await send('Page.enable');
 await send('Page.addScriptToEvaluateOnNewDocument',{source:`
   document.addEventListener('play',e=>{if(e.target instanceof HTMLVideoElement){e.target.muted=true;e.target.playbackRate=${Number(rate)}}},true);
 `});
 await send('Page.navigate',{url});
 const end=Date.now()+Number(seconds)*1000;

 for(let tick=0;Date.now()<end;tick++) {
  const r=await send('Runtime.evaluate',{expression:`(()=>{
    const p=document.querySelector('#movie_player'),v=document.querySelector('video');
    if(!v)return {title:document.title,text:document.body?.innerText.slice(0,1000)};
    if(!window.nvdStarted&&v.readyState>=2){v.currentTime=0;v.playbackRate=${Number(rate)};v.muted=true;v.play();window.nvdStarted=true;}
    const s=p?.getVideoStats?.();const response=p?.getPlayerResponse?.();
    return {title:document.title,videoId:response?.videoDetails?.videoId,time:v.currentTime,duration:v.duration,rate:v.playbackRate,paused:v.paused,ready:v.readyState,ended:v.ended,error:v.error?.message,
      width:v.videoWidth,height:v.videoHeight,quality:JSON.parse(JSON.stringify(v.getVideoPlaybackQuality(), ["totalVideoFrames","droppedVideoFrames","corruptedVideoFrames"])),stats:s?{fmt:s.fmt,debug_error:s.debug_error}:null,
      available:p?.getAvailableQualityLevels?.(),playerState:p?.getPlayerState?.()};
  })()`,returnByValue:true});
  const sample=r.result.value??r.exceptionDetails;append({sample});
  if(tick%10===0)console.log(JSON.stringify({runDir,tick,sample}));
  if(sample?.ended)break;
  await new Promise(r=>setTimeout(r,1000));
 }
 await send('Page.captureScreenshot',{format:'png'}).then(r=>fs.writeFileSync(path.resolve(runDir,'last.png'),Buffer.from(r.data,'base64')));
}catch(error){append({error:String(error)});console.error(error);process.exitCode=1}
finally{ws?.close();chrome.kill('SIGTERM');fs.closeSync(log)}

const records=fs.readFileSync(events,'utf8').trim().split('\n').map(JSON.parse);
const samples=records.filter(r=>r.sample).map(r=>r.sample);
const summary={url,driverDir:path.resolve(driverDir),softwareControl:process.env.NVD_PROBE_SOFTWARE==='1',
  ended:samples.some(s=>s.ended),maxTime:Math.max(0,...samples.map(s=>s.time??0)),
  mediaErrors:records.filter(r=>r.method==='Media.playerErrorsRaised'),
  siteErrors:[...new Set(samples.map(s=>s.stats?.debug_error).filter(Boolean))],
  formats:[...new Set(samples.map(s=>s.stats?.fmt).filter(Boolean))],last:samples.at(-1)};
fs.writeFileSync(path.resolve(runDir,'summary.json'),JSON.stringify(summary,null,2)+'\n');
console.log(JSON.stringify(summary));
